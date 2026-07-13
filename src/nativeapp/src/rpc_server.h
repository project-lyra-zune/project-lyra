#pragma once

#include "nativeapp_common.h"

// TCP accept loop on port 1337. Rebinds when the interface set changes
// (CE 6 binds to the set of interfaces present at bind() time, so a fresh
// PPP-over-USB interface that came up after the daemon started would be
// invisible without rebind). Per-connection handler runs under SEH so an
// access-violation in any opcode doesn't kill the listener.
DWORD Server(void* sd_);

// Per-connection request dispatcher. Reads request header bytes off the
// socket and dispatches by inbuf[0] opcode. Uses a `done:` goto-cleanup
// pattern because CE 6 ARMV4I MSVC doesn't support local SEH unwind
// (C2822). SEH propagates to Server's __except.
void connection(SOCKET client);
