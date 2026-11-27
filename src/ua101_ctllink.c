#include "ua101_ctllink.h"

#include <stdlib.h>
#include <string.h>
#include <xpc/xpc.h>

#include "ua101_link.h"

_Static_assert(UA101_CTLLINK_MAX_CHANNELS == UA101_LINK_MAX_CHANNELS, "channel limits");

struct ua101_ctllink {
    dispatch_queue_t queue; /* everything below is used on it only */
    /* XPC hands over the connection's events and the replies on this one, in the order they
       came, and each steps onto `queue` from there. (Giving XPC `queue` itself would serialize
       just as well, but the thread checker can't see through that, and this it can.) */
    dispatch_queue_t xpc_queue;
    ua101_ctllink_events events;
    void *context;
    xpc_connection_t conn; /* NULL until the first attach, and after the connection died */
    int replied;           /* the helper has answered on conn */
    int closed;            /* ua101_ctllink_close was called: no more events */
    unsigned attach_serial; /* the attach whose answer is still wanted */
    int refs;              /* the owner, each connection's handler, each reply awaited */
    /* The link version every request carries. Each attach starts with this build's; a helper
       of the one older version that answers UA101_LINK_ERR_VERSION is asked again in its own
       (link version 20: it then gives no memory preview, and everything else is as before). */
    unsigned version;
    unsigned answered_version; /* the version of the last attach that was answered */
};

static xpc_endpoint_t g_endpoint;

void ua101_ctllink_use_endpoint(void *xpc_endpoint)
{
    if (g_endpoint != NULL)
        xpc_release(g_endpoint);
    g_endpoint = xpc_endpoint != NULL ? (xpc_endpoint_t)xpc_retain((xpc_object_t)xpc_endpoint) : NULL;
}

static void link_release(ua101_ctllink *l)
{
    if (--l->refs != 0)
        return;
    dispatch_release(l->xpc_queue);
    dispatch_release(l->queue);
    free(l);
}

/* An attach is waiting and the helper can't be reached: say so, once. */
static void no_helper(ua101_ctllink *l)
{
    if (l->closed || l->attach_serial == 0)
        return;
    l->attach_serial = 0;
    if (l->events.attached != NULL)
        l->events.attached(l->context, UA101_LINK_ERR_NO_HELPER, 0, 0,
                           "the helper can't be reached: it isn't installed, or it is an older "
                           "one that doesn't know the control app");
}

static void on_event(ua101_ctllink *l, xpc_connection_t c, xpc_object_t event)
{
    xpc_type_t type = xpc_get_type(event);

    if (type == XPC_TYPE_DICTIONARY) {
        const char *what = xpc_dictionary_get_string(event, UA101_LINK_EVENT);

        if (l->closed || c != l->conn || what == NULL)
            return;
        if (strcmp(what, "ctl") == 0) {
            size_t len = 0;
            const uint8_t *data = xpc_dictionary_get_data(event, UA101_LINK_DATA, &len);

            if (data != NULL && len != 0 && l->events.message != NULL)
                l->events.message(l->context, data, len);
        } else if (strcmp(what, "audio") == 0) {
            if (l->events.audio != NULL)
                l->events.audio(l->context, xpc_dictionary_get_bool(event, UA101_LINK_STREAMING));
        } else if (strcmp(what, "changed") == 0) {
            if (l->events.changed != NULL)
                l->events.changed(l->context);
        } else if (strcmp(what, "midi_lamps") == 0) {
            if (l->events.midi != NULL)
                l->events.midi(l->context, xpc_dictionary_get_bool(event, UA101_LINK_MIDI_IN),
                               xpc_dictionary_get_bool(event, UA101_LINK_MIDI_OUT));
        }
    } else if (event == XPC_ERROR_CONNECTION_INTERRUPTED) {
        /* The helper exited or crashed (launchd starts it again on the next message), or it
           turned this connection away, as a helper older than the control app does. Only the
           first is news, and only if the helper had answered before: telling the app to attach
           again after being turned away would go round for ever. */
        if (!l->closed && c == l->conn) {
            if (l->replied) {
                l->replied = 0;
                if (l->events.changed != NULL)
                    l->events.changed(l->context);
            } else {
                no_helper(l);
            }
        }
    } else if (event == XPC_ERROR_CONNECTION_INVALID) {
        /* This connection's last event: the helper isn't installed, refused this user (an
           older helper does), or the link was closed or detached. */
        int was_current = c == l->conn, had_replied = l->replied;

        if (was_current) {
            l->conn = NULL;
            l->replied = 0;
        }
        xpc_release(c);
        if (was_current && !l->closed) {
            if (had_replied) {
                if (l->events.changed != NULL)
                    l->events.changed(l->context);
            } else {
                no_helper(l);
            }
        }
        link_release(l);
    }
}

static void connect_now(ua101_ctllink *l)
{
    xpc_connection_t c;

    if (g_endpoint != NULL) {
        c = xpc_connection_create_from_endpoint(g_endpoint);
        xpc_connection_set_target_queue(c, l->xpc_queue);
    } else {
        /* The helper is a LaunchDaemon, so its name is in the system's bootstrap. */
        c = xpc_connection_create_mach_service(UA101_SERVICE_NAME, l->xpc_queue,
                                               XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    }
    l->conn = c;
    l->replied = 0;
    l->refs++; /* the handler's, given back at the connection's last event */
    xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
      xpc_retain(event);
      dispatch_async(l->queue, ^{
        on_event(l, c, event);
        xpc_release(event);
      });
    });
    xpc_connection_resume(c);
}

static xpc_object_t request(const ua101_ctllink *l, const char *op)
{
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);

    xpc_dictionary_set_string(m, UA101_LINK_OP, op);
    xpc_dictionary_set_uint64(m, UA101_LINK_KEY_VERSION, l->version);
    return m;
}

/* Sends `m`, which it releases; `then` gets the helper's answer on the link's queue, unless
   the link was closed or the connection replaced meanwhile. An answer that is an error (the
   connection broke) is left to the connection's own events. */
static void ask(ua101_ctllink *l, xpc_object_t m, void (^then)(xpc_object_t reply))
{
    xpc_connection_t c = l->conn;

    l->refs++;
    xpc_connection_send_message_with_reply(c, m, l->xpc_queue, ^(xpc_object_t reply) {
      xpc_retain(reply);
      dispatch_async(l->queue, ^{
        if (!l->closed && c == l->conn && xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
            l->replied = 1;
            then(reply);
        }
        xpc_release(reply);
        link_release(l);
      });
    });
    xpc_release(m);
}

ua101_ctllink *ua101_ctllink_open(dispatch_queue_t queue, const ua101_ctllink_events *events,
                                  void *context)
{
    ua101_ctllink *l = calloc(1, sizeof(*l));

    if (l == NULL)
        return NULL;
    l->queue = queue;
    dispatch_retain(queue);
    l->xpc_queue = dispatch_queue_create("ua101.ctllink", DISPATCH_QUEUE_SERIAL);
    l->events = *events;
    l->context = context;
    l->refs = 1;
    l->version = UA101_LINK_VERSION;
    return l;
}

unsigned ua101_ctllink_version(const ua101_ctllink *l)
{
    return l->answered_version;
}

/* Sends ctl_open in the link's version now; the answer, for the attach `serial`. */
static void ask_open(ua101_ctllink *l, unsigned serial)
{
    ask(l, request(l, "ctl_open"), ^(xpc_object_t reply) {
      int status = (int)xpc_dictionary_get_int64(reply, UA101_LINK_STATUS);
      const char *why = xpc_dictionary_get_string(reply, UA101_LINK_WHY);
      uint64_t read;

      if (l->attach_serial != serial)
          return;
      /* A helper one version older: asked again in its own version, which differs only in
         the memory reads it doesn't pass on. Any other version stays an error. */
      if (status == UA101_LINK_ERR_VERSION && l->version == UA101_LINK_VERSION &&
          l->conn != NULL) {
          l->version = UA101_LINK_VERSION_OLDER_HELPER;
          ask_open(l, serial);
          return;
      }
      l->attach_serial = 0;
      l->answered_version = l->version;
      /* Only a helper of this build reads memories; an older one is never asked to. */
      read = l->version == UA101_LINK_VERSION
                 ? xpc_dictionary_get_uint64(reply, UA101_LINK_CTL_PASSES_READ)
                 : 0;
      if (status == UA101_LINK_OK && l->events.passes != NULL)
          l->events.passes(l->context, xpc_dictionary_get_uint64(reply, UA101_LINK_CTL_PASSES),
                           xpc_dictionary_get_uint64(reply, UA101_LINK_CTL_PASSES_ON),
                           xpc_dictionary_get_uint64(reply, UA101_LINK_CTL_PASSES_MEMORY), read);
      if (l->closed)
          return;
      if (l->events.attached != NULL)
          l->events.attached(l->context, status,
                             xpc_dictionary_get_bool(reply, UA101_LINK_STREAMING),
                             xpc_dictionary_get_bool(reply, UA101_LINK_WATCHING),
                             why != NULL ? why : "");
    });
}

void ua101_ctllink_attach(ua101_ctllink *l)
{
    static unsigned serials;
    unsigned serial = ++serials != 0 ? serials : ++serials;

    if (l->closed)
        return;
    if (l->conn == NULL)
        connect_now(l);
    l->attach_serial = serial;
    /* Each attach tries this build's version first: the helper may have been updated. */
    l->version = UA101_LINK_VERSION;
    ask_open(l, serial);
    /* A helper that is there but stuck never answers; say so instead of waiting for ever. */
    l->refs++;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(UA101_CTLLINK_ATTACH_SECONDS * NSEC_PER_SEC)),
                   l->queue, ^{
                     if (!l->closed && l->attach_serial == serial) {
                         l->attach_serial = 0;
                         if (l->events.attached != NULL)
                             l->events.attached(l->context, UA101_LINK_ERR_NO_HELPER, 0, 0,
                                                "the helper didn't answer in time");
                     }
                     link_release(l);
                   });
}

int ua101_ctllink_send(ua101_ctllink *l, const uint8_t *msg, size_t len)
{
    xpc_object_t m;

    if (l->closed || l->conn == NULL || msg == NULL || len == 0)
        return 0;
    m = request(l, "ctl_send");
    xpc_dictionary_set_data(m, UA101_LINK_DATA, msg, len);
    ask(l, m, ^(xpc_object_t reply) {
      int status = (int)xpc_dictionary_get_int64(reply, UA101_LINK_STATUS);
      const char *why = xpc_dictionary_get_string(reply, UA101_LINK_WHY);

      if (status != UA101_LINK_OK && l->events.refused != NULL)
          l->events.refused(l->context, status, why != NULL ? why : "");
    });
    return 1;
}

static uint32_t copy_peaks(xpc_object_t reply, const char *key, uint32_t *out)
{
    size_t bytes = 0;
    const void *data = xpc_dictionary_get_data(reply, key, &bytes);
    size_t n = bytes / sizeof(uint32_t);

    if (data == NULL)
        return 0;
    if (n > UA101_CTLLINK_MAX_CHANNELS)
        n = UA101_CTLLINK_MAX_CHANNELS;
    memcpy(out, data, n * sizeof(uint32_t));
    return (uint32_t)n;
}


void ua101_ctllink_ask_levels(ua101_ctllink *l)
{
    if (l->closed || l->conn == NULL)
        return;
    ask(l, request(l, "levels"), ^(xpc_object_t reply) {
      ua101_ctl_levels levels;

      if (xpc_dictionary_get_int64(reply, UA101_LINK_STATUS) != UA101_LINK_OK ||
          l->events.levels == NULL)
          return;
      memset(&levels, 0, sizeof(levels));
      levels.streaming = xpc_dictionary_get_bool(reply, UA101_LINK_STREAMING);
      levels.rate_hz = xpc_dictionary_get_double(reply, UA101_LINK_RATE);
      levels.capture_channels = copy_peaks(reply, UA101_LINK_CAPTURE_PEAKS, levels.capture);
      levels.playback_channels = copy_peaks(reply, UA101_LINK_PLAYBACK_PEAKS, levels.playback);
      l->events.levels(l->context, &levels);
    });
}

void ua101_ctllink_detach(ua101_ctllink *l)
{
    xpc_connection_t c = l->conn;

    l->attach_serial = 0;
    if (c == NULL)
        return;
    /* The helper is told, then the connection is dropped; its last event frees it. Dropping it
       also detaches, should the message not arrive. */
    {
        xpc_object_t m = request(l, "ctl_close");

        xpc_connection_send_message(c, m);
        xpc_release(m);
    }
    l->conn = NULL;
    l->replied = 0;
    /* After everything already handed over has gone out. */
    xpc_connection_send_barrier(c, ^{
      xpc_connection_cancel(c);
    });
}

void ua101_ctllink_close(ua101_ctllink *l)
{
    if (l->closed)
        return;
    ua101_ctllink_detach(l);
    l->closed = 1;
    link_release(l);
}
