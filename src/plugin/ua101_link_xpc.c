#include "ua101_link_xpc.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../ua101_link.h"

struct ua101_link {
    dispatch_queue_t queue; /* the connection's events, and the fields below, are on it */
    xpc_connection_t conn;
    int dead;    /* conn got its last event: the next request makes a new one */
    int replied; /* the helper has answered on conn */
    int closing; /* ua101_link_close was called */
    void (*on_change)(void *context);
    void *context;
    void *map; /* used only by the caller's calls, which Core Audio's plug-in serializes */
    size_t map_bytes;
};

static xpc_endpoint_t g_endpoint;

/*
 * How long each request waits for the helper's reply, in seconds. Core Audio calls the plug-in
 * and waits, so a helper stuck inside a USB call must not hold it for ever. Hello covers
 * launchd starting the helper and the helper opening the unit: 0.2 to 0.3 s as measured, once
 * 8 s at the first start after the first install. Start only starts a thread. Stop joins the
 * USB thread, which takes up to 2.3 s when transfers must be aborted.
 */
static double g_hello_limit = 15.0, g_start_limit = 5.0, g_stop_limit = 5.0;

void ua101_link_xpc_set_time_limits(double hello, double start, double stop)
{
    g_hello_limit = hello;
    g_start_limit = start;
    g_stop_limit = stop;
}

/* One request's wait. The reply's handler and the waiting caller each hold it, so a reply that
   comes after the caller gave up finds it still there, and is dropped. */
typedef struct {
    dispatch_semaphore_t done;
    pthread_mutex_t lock;
    xpc_object_t reply; /* under lock */
    int abandoned;      /* under lock: the caller stopped waiting */
    _Atomic int refs;
} pending;

static void pending_release(pending *w)
{
    if (atomic_fetch_sub(&w->refs, 1) == 1) {
        if (w->reply != NULL)
            xpc_release(w->reply);
        dispatch_release(w->done);
        pthread_mutex_destroy(&w->lock);
        free(w);
    }
}

void ua101_link_xpc_use_endpoint(xpc_endpoint_t endpoint)
{
    if (g_endpoint != NULL)
        xpc_release(g_endpoint);
    g_endpoint = endpoint != NULL ? (xpc_endpoint_t)xpc_retain(endpoint) : NULL;
}

static void forget(ua101_link *l)
{
    xpc_release(l->conn);
    dispatch_release(l->queue);
    free(l);
}

/* On l->queue. */
static void on_event(ua101_link *l, xpc_connection_t c, xpc_object_t event)
{
    xpc_type_t type = xpc_get_type(event);

    if (c != l->conn)
        return; /* one replaced after it died; nothing more comes from it */
    if (type == XPC_TYPE_DICTIONARY) {
        /* The helper says something changed. */
        if (xpc_dictionary_get_string(event, UA101_LINK_EVENT) != NULL && l->on_change != NULL)
            l->on_change(l->context);
    } else if (event == XPC_ERROR_CONNECTION_INTERRUPTED) {
        /* The helper exited or crashed; launchd starts it again on the next message. */
        if (l->on_change != NULL)
            l->on_change(l->context);
    } else if (event == XPC_ERROR_CONNECTION_INVALID) {
        /* The connection's last event. The service isn't there (not installed, or unloaded),
           or this is the end of ua101_link_close. */
        if (l->closing) {
            forget(l);
            return;
        }
        l->dead = 1;
        /* Only news if it worked before; a failed hello already knows. */
        if (l->replied && l->on_change != NULL)
            l->on_change(l->context);
    }
}

/* On l->queue. */
static void connect_now(ua101_link *l)
{
    xpc_connection_t c;

    if (g_endpoint != NULL) {
        c = xpc_connection_create_from_endpoint(g_endpoint);
        xpc_connection_set_target_queue(c, l->queue);
    } else {
        /* The helper is a LaunchDaemon, so its name is in the system's bootstrap. */
        c = xpc_connection_create_mach_service(UA101_SERVICE_NAME, l->queue,
                                               XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    }
    l->conn = c;
    l->dead = 0;
    l->replied = 0;
    xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
      on_event(l, c, event);
    });
    xpc_connection_resume(c);
}

ua101_link *ua101_link_open(void (*on_change)(void *context), void *context)
{
    ua101_link *l = calloc(1, sizeof(*l));

    if (l == NULL)
        return NULL;
    l->on_change = on_change;
    l->context = context;
    l->queue = dispatch_queue_create("ua101.link", DISPATCH_QUEUE_SERIAL);
    dispatch_sync(l->queue, ^{
      connect_now(l);
    });
    return l;
}

/* The connection to send on, made again if the last one died. The caller releases it. */
static xpc_connection_t current(ua101_link *l)
{
    __block xpc_connection_t c;

    dispatch_sync(l->queue, ^{
      if (l->dead) {
          xpc_release(l->conn);
          connect_now(l);
      }
      c = (xpc_connection_t)xpc_retain(l->conn);
    });
    return c;
}

/* Sends `msg`, which it releases, and waits up to `limit` seconds for the reply. Returns NULL,
   with the reason, if there was none; *timed_out then says whether the helper is there but
   didn't answer, in which case it may still act on the message later. */
static xpc_object_t request_with(ua101_link *l, xpc_object_t msg, const char *op, double limit,
                                 int *timed_out, char *why, size_t why_bytes)
{
    xpc_object_t reply = NULL;
    xpc_connection_t c = current(l);
    pending *w = calloc(1, sizeof(*w));

    if (timed_out != NULL)
        *timed_out = 0;
    xpc_dictionary_set_string(msg, UA101_LINK_OP, op);
    xpc_dictionary_set_uint64(msg, UA101_LINK_KEY_VERSION, UA101_LINK_VERSION);
    if (w == NULL) {
        xpc_release(msg);
        xpc_release(c);
        snprintf(why, why_bytes, "out of memory");
        return NULL;
    }
    w->done = dispatch_semaphore_create(0);
    pthread_mutex_init(&w->lock, NULL);
    atomic_store(&w->refs, 2);
    xpc_connection_send_message_with_reply(c, msg, NULL, ^(xpc_object_t r) {
      pthread_mutex_lock(&w->lock);
      if (!w->abandoned)
          w->reply = xpc_retain(r);
      pthread_mutex_unlock(&w->lock);
      dispatch_semaphore_signal(w->done);
      pending_release(w);
    });
    xpc_release(msg);
    dispatch_semaphore_wait(w->done,
                            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(limit * NSEC_PER_SEC)));
    /* Takes the reply or gives up, never half: one that comes from here on is dropped. */
    pthread_mutex_lock(&w->lock);
    reply = w->reply;
    w->reply = NULL;
    w->abandoned = 1;
    pthread_mutex_unlock(&w->lock);
    pending_release(w);
    if (reply != NULL && xpc_get_type(reply) == XPC_TYPE_DICTIONARY)
        dispatch_sync(l->queue, ^{
          if (l->conn == c)
              l->replied = 1;
        });
    xpc_release(c);
    if (reply == NULL) {
        if (timed_out != NULL)
            *timed_out = 1;
        snprintf(why, why_bytes, "the helper (%s) didn't answer \"%s\" within %.1f s",
                 UA101_SERVICE_NAME, op, limit);
        return NULL;
    }
    if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY) {
        snprintf(why, why_bytes, "the helper (%s) %s", UA101_SERVICE_NAME,
                 reply == XPC_ERROR_CONNECTION_INVALID ? "isn't installed or isn't running"
                                                       : "went away");
        xpc_release(reply);
        return NULL;
    }
    return reply;
}

/* Sends `op` alone and waits for the reply. */
static xpc_object_t request(ua101_link *l, const char *op, double limit, int *timed_out,
                            char *why, size_t why_bytes)
{
    return request_with(l, xpc_dictionary_create(NULL, NULL, 0), op, limit, timed_out, why,
                        why_bytes);
}

static int status_of(xpc_object_t reply, char *why, size_t why_bytes)
{
    int64_t status = xpc_dictionary_get_int64(reply, UA101_LINK_STATUS);
    const char *text = xpc_dictionary_get_string(reply, UA101_LINK_WHY);

    if (status != UA101_LINK_OK)
        snprintf(why, why_bytes, "%s", text != NULL ? text : "the helper refused");
    return (int)status;
}

static void unmap(ua101_link *l)
{
    if (l->map != NULL) {
        munmap(l->map, l->map_bytes);
        l->map = NULL;
        l->map_bytes = 0;
    }
}

int ua101_link_hello(ua101_link *l, ua101_link_info *info, int keep_mapped, char *why,
                     size_t why_bytes)
{
    xpc_object_t reply = request(l, "hello", g_hello_limit, NULL, why, why_bytes), shmem;
    int status;

    memset(info, 0, sizeof(*info));
    if (reply == NULL)
        return UA101_LINK_ERR_NO_HELPER;
    status = status_of(reply, why, why_bytes);
    if (status == UA101_LINK_OK && xpc_dictionary_get_bool(reply, UA101_LINK_PRESENT)) {
        shmem = xpc_dictionary_get_value(reply, UA101_LINK_SHMEM);
        if (!keep_mapped || l->map == NULL) {
            void *region = NULL;
            size_t bytes;

            unmap(l);
            bytes = shmem != NULL && xpc_get_type(shmem) == XPC_TYPE_SHMEM
                        ? xpc_shmem_map(shmem, &region)
                        : 0;
            if (bytes == 0 || !ua101_shared_check(region, bytes)) {
                if (bytes != 0)
                    munmap(region, bytes);
                snprintf(why, why_bytes, "the helper's shared block isn't usable");
                xpc_release(reply);
                return UA101_LINK_ERR_REQUEST;
            }
            /* Kept in memory, as the helper keeps its side, so the I/O thread never waits for a
               page. If it can't be, I/O still works. */
            mlock(region, bytes);
            l->map = region;
            l->map_bytes = bytes;
        }
        info->present = 1;
        info->rate_hz = xpc_dictionary_get_double(reply, UA101_LINK_RATE);
        info->capture_channels = (uint32_t)xpc_dictionary_get_uint64(reply, UA101_LINK_IN);
        info->playback_channels = (uint32_t)xpc_dictionary_get_uint64(reply, UA101_LINK_OUT);
        info->input_offset = (uint32_t)xpc_dictionary_get_uint64(reply, UA101_LINK_INPUT_OFFSET);
        info->output_offset =
            (uint32_t)xpc_dictionary_get_uint64(reply, UA101_LINK_OUTPUT_OFFSET);
        info->block_id = xpc_dictionary_get_uint64(reply, UA101_LINK_BLOCK);
        info->shared = l->map;
        info->shared_bytes = l->map_bytes;
    } else {
        info->watching = xpc_dictionary_get_bool(reply, UA101_LINK_WATCHING);
    }
    xpc_release(reply);
    return status;
}

int ua101_link_start(ua101_link *l, char *why, size_t why_bytes)
{
    int timed_out = 0, status;
    xpc_object_t reply = request(l, "start", g_start_limit, &timed_out, why, why_bytes);

    if (reply == NULL) {
        if (timed_out) {
            /* The helper may still start streaming when it gets to the message. A stop sent
               behind it, waiting for nothing, makes sure it then stops again. */
            xpc_connection_t c = current(l);
            xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);

            xpc_dictionary_set_string(msg, UA101_LINK_OP, "stop");
            xpc_dictionary_set_uint64(msg, UA101_LINK_KEY_VERSION, UA101_LINK_VERSION);
            xpc_connection_send_message(c, msg);
            xpc_release(msg);
            xpc_release(c);
        }
        return UA101_LINK_ERR_NO_HELPER;
    }
    status = status_of(reply, why, why_bytes);
    xpc_release(reply);
    return status;
}

void ua101_link_stop(ua101_link *l, const ua101_link_counts *counts)
{
    char why[128];
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0), reply;

    xpc_dictionary_set_uint64(msg, UA101_LINK_MISSING, counts->capture_missing);
    xpc_dictionary_set_uint64(msg, UA101_LINK_CLIPPED, counts->clipped);
    xpc_dictionary_set_double(msg, UA101_LINK_PEAK, counts->clipped_peak);
    if (counts->corrected) {
        xpc_dictionary_set_uint64(msg, UA101_LINK_CUT, counts->cut);
        xpc_dictionary_set_double(msg, UA101_LINK_CUT_PEAK, counts->cut_peak);
    }
    if (counts->input_offset != 0 || counts->output_offset != 0) {
        xpc_dictionary_set_uint64(msg, UA101_LINK_INPUT_OFFSET, counts->input_offset);
        xpc_dictionary_set_uint64(msg, UA101_LINK_OUTPUT_OFFSET, counts->output_offset);
    }
    if (counts->capture_spare_known)
        xpc_dictionary_set_int64(msg, UA101_LINK_CAPTURE_SPARE, counts->capture_spare);
    reply = request_with(l, msg, "stop", g_stop_limit, NULL, why, sizeof(why));

    if (reply != NULL)
        xpc_release(reply);
}

void ua101_link_close(ua101_link *l)
{
    __block int dead = 0;

    if (l == NULL)
        return;
    unmap(l);
    dispatch_sync(l->queue, ^{
      l->closing = 1;
      l->on_change = NULL;
      dead = l->dead;
    });
    if (dead) {
        forget(l); /* its last event already came, so nothing else will free the link */
    } else {
        xpc_connection_cancel(l->conn); /* its last event frees the link */
    }
}
