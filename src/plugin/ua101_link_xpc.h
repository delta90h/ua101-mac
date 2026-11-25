/*
 * The XPC implementation of src/ua101_link.h. Installed, it connects to the helper's Mach
 * service through launchd. For a test in one process, it can connect to an endpoint instead
 * (ua101_service_start_anonymous), so the whole path runs without installing anything.
 */
#ifndef UA101_LINK_XPC_H
#define UA101_LINK_XPC_H

#include <xpc/xpc.h>

/* Links opened after this connect to `endpoint` instead of the Mach service; NULL undoes it. */
void ua101_link_xpc_use_endpoint(xpc_endpoint_t endpoint);

/* For a test: how long hello, start and stop each wait for the helper's reply, in seconds. */
void ua101_link_xpc_set_time_limits(double hello, double start, double stop);

#endif
