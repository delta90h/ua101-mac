/* A MIDI client's end of the link: see src/ua101_midilink.h. Built as src/ua101_ctllink.c is. */
#include "ua101_midilink.h"

#include <stdlib.h>
#include <string.h>
#include <xpc/xpc.h>

#include "ua101_link.h"

struct ua101_midilink {
    dispatch_queue_t queue; /* everything below is used on it only */
    /* XPC hands over the connection's events and the replies on this one, in the order they
       came, and each steps onto `queue` from there. */
    dispatch_queue_t xpc_queue;
    ua101_midilink_events events;
    void *context;
    xpc_connection_t conn;  /* NULL until the first attach, and after the connection died */
    int replied;            /* the helper has answered on conn */
    int closed;             /* ua101_midilink_close was called: no more events */
    unsigned attach_serial; /* the attach whose answer is still wanted */
    int refs;               /* the owner, each connection's handler, each reply awaited */
};

static xpc_endpoint_t g_endpoint;

void ua101_midilink_use_endpoint(void *xpc_endpoint)
{
    if (g_endpoint != NULL)
        xpc_release(g_endpoint);
    g_endpoint = xpc_endpoint != NULL ? (xpc_endpoint_t)xpc_retain((xpc_object_t)xpc_endpoint) : NULL;
}

static void link_release(ua101_midilink *l)
{
    if (--l->refs != 0)
        return;
    dispatch_release(l->xpc_queue);
    dispatch_release(l->queue);
    free(l);
}

/* An attach is waiting and the helper can't be reached: say so, once. */
static void no_helper(ua101_midilink *l)
{
    if (l->closed || l->attach_serial == 0)
        return;
    l->attach_serial = 0;
    if (l->events.attached != NULL)
        l->events.attached(l->context, UA101_LINK_ERR_NO_HELPER, 0,
                           "the helper can't be reached: it isn't installed, or it is an older "
                           "one that doesn't know MIDI clients");
}

static void on_event(ua101_midilink *l, xpc_connection_t c, xpc_object_t event)
{
    xpc_type_t type = xpc_get_type(event);

    if (type == XPC_TYPE_DICTIONARY) {
        const char *what = xpc_dictionary_get_string(event, UA101_LINK_EVENT);

        if (l->closed || c != l->conn || what == NULL)
            return;
        if (strcmp(what, "midi") == 0) {
            size_t len = 0;
            const uint8_t *data = xpc_dictionary_get_data(event, UA101_LINK_DATA, &len);

            if (data != NULL && len != 0 && l->events.midi != NULL)
                l->events.midi(l->context, data, len,
                               xpc_dictionary_get_uint64(event, UA101_LINK_TIME));
        } else if (strcmp(what, "changed") == 0) {
            if (l->events.changed != NULL)
                l->events.changed(l->context);
        }
    } else if (event == XPC_ERROR_CONNECTION_INTERRUPTED) {
        /* The helper exited or crashed (launchd starts it again on the next message), or it
           turned this connection away. Only the first is news, and only if the helper had
           answered before. */
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
        /* This connection's last event: the helper isn't installed, refused this user, or the
           link was closed or detached. */
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

static void connect_now(ua101_midilink *l)
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

static xpc_object_t request(const char *op)
{
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);

    xpc_dictionary_set_string(m, UA101_LINK_OP, op);
    xpc_dictionary_set_uint64(m, UA101_LINK_KEY_VERSION, UA101_LINK_VERSION);
    return m;
}

ua101_midilink *ua101_midilink_open(dispatch_queue_t queue, const ua101_midilink_events *events,
                                    void *context)
{
    ua101_midilink *l = calloc(1, sizeof(*l));

    if (l == NULL)
        return NULL;
    l->queue = queue;
    dispatch_retain(queue);
    l->xpc_queue = dispatch_queue_create("ua101.midilink", DISPATCH_QUEUE_SERIAL);
    l->events = *events;
    l->context = context;
    l->refs = 1;
    return l;
}

void ua101_midilink_attach(ua101_midilink *l)
{
    static unsigned serials;
    unsigned serial = ++serials != 0 ? serials : ++serials;
    xpc_connection_t c;

    if (l->closed)
        return;
    if (l->conn == NULL)
        connect_now(l);
    c = l->conn;
    l->attach_serial = serial;
    l->refs++;
    {
        xpc_object_t m = request("midi_open");

        xpc_connection_send_message_with_reply(c, m, l->xpc_queue, ^(xpc_object_t reply) {
          xpc_retain(reply);
          dispatch_async(l->queue, ^{
            /* An answer that is an error (the connection broke) is left to the connection's
               own events. */
            if (!l->closed && c == l->conn && xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
                l->attach_serial == serial) {
                const char *why = xpc_dictionary_get_string(reply, UA101_LINK_WHY);

                l->replied = 1;
                l->attach_serial = 0;
                if (l->events.attached != NULL)
                    l->events.attached(l->context,
                                       (int)xpc_dictionary_get_int64(reply, UA101_LINK_STATUS),
                                       xpc_dictionary_get_bool(reply, UA101_LINK_WATCHING),
                                       why != NULL ? why : "");
            }
            xpc_release(reply);
            link_release(l);
          });
        });
        xpc_release(m);
    }
    /* A helper that is there but stuck never answers; say so instead of waiting for ever. */
    l->refs++;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(UA101_MIDILINK_ATTACH_SECONDS * NSEC_PER_SEC)),
                   l->queue, ^{
                     if (!l->closed && l->attach_serial == serial) {
                         l->attach_serial = 0;
                         if (l->events.attached != NULL)
                             l->events.attached(l->context, UA101_LINK_ERR_NO_HELPER, 0,
                                                "the helper didn't answer in time");
                     }
                     link_release(l);
                   });
}

int ua101_midilink_send(ua101_midilink *l, const uint8_t *bytes, size_t len)
{
    if (l->closed || l->conn == NULL || bytes == NULL || len == 0)
        return 0;
    /* In pieces the helper takes, in order. No answer is asked for. */
    while (len != 0) {
        size_t n = len < UA101_LINK_MIDI_SEND_MAX ? len : UA101_LINK_MIDI_SEND_MAX;
        xpc_object_t m = request("midi_send");

        xpc_dictionary_set_data(m, UA101_LINK_DATA, bytes, n);
        xpc_connection_send_message(l->conn, m);
        xpc_release(m);
        bytes += n;
        len -= n;
    }
    return 1;
}

void ua101_midilink_detach(ua101_midilink *l)
{
    xpc_connection_t c = l->conn;

    l->attach_serial = 0;
    if (c == NULL)
        return;
    /* The helper is told, then the connection is dropped; its last event frees it. Dropping it
       also detaches, should the message not arrive. */
    {
        xpc_object_t m = request("midi_close");

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

void ua101_midilink_close(ua101_midilink *l)
{
    if (l->closed)
        return;
    ua101_midilink_detach(l);
    l->closed = 1;
    link_release(l);
}
