#pragma once

// TCP IPC listeners — the conduit Wine's `lsteamclient.dll` uses to
// reach our libsteamclient.so. Valve's prebuilt binary reads two env
// vars (Steam3Master, SteamClientService) at module-init time, parses
// each as "host:port", and binds two TCP listeners. The bootstrap is
// already supplying these env vars; we just need to read + bind + serve.
//
// Stage 0 (this file): bind + accept + log the first bytes received.
// That's enough to prove the listeners are live + that Wine connects.
// Stage 1+ will layer the actual binary IPC protocol.
//
// Lifetime: start_tcp_services() is idempotent — repeat calls return
// without re-binding. Listener threads are detached and live for the
// process lifetime; tearing them down cleanly is a Stage 1+ concern.

#include <cstdint>

namespace wn_libsteamclient {

// Start both listeners. Idempotent. Reads Steam3Master /
// SteamClientService env vars; default fallback 127.0.0.1:57343/57344
// matches the bootstrap's apply_env defaults.
//
// Returns true if at least one listener bound successfully. False
// means the env vars were malformed AND defaults also failed (e.g.
// port already in use); caller logs and continues — game-side IPC
// is unavailable but the in-process API surface still works.
bool start_tcp_services();

// Diagnostic counter — total number of inbound connections accepted
// across all listeners. Used by HybridModeReceiver to verify the
// listeners are reachable; resets to 0 only on process restart.
int  accepted_connection_count();

}  // namespace wn_libsteamclient
