#pragma once

// In-process CCallbackBase registry — the table SteamAPI_RegisterCallback
// inserts into and SteamAPI_RunCallbacks drains against.
//
// CCallbackBase object layout (from public/steam/steam_api_common.h):
//
//   class CCallbackBase {
//     virtual void Run(void* pvParam) = 0;                            // slot 0
//     virtual void Run(void*, bool bIOFailure, SteamAPICall_t hCall); // slot 1
//     virtual int  GetCallbackSizeBytes() = 0;                        // slot 2
//   protected:
//     uint8 m_nCallbackFlags;   // at offset 8 under Itanium ABI
//     int   m_iCallback;        // at offset 12 (4-byte aligned)
//   };
//
// We don't include the SDK header — replicating offset constants
// suffices and avoids dragging in steam_api_common.h.
//
// The SDK's STEAM_CALLBACK macro instantiates a CCallback<T,P> subclass
// whose constructor sets m_iCallback to P::k_iCallback then calls
// SteamAPI_RegisterCallback(this, P::k_iCallback). When the registered
// callback id matches a message in our callback_queue, we invoke
// vtable[0](payload_bytes_ptr) — equivalent to calling Run(pvParam) at
// the SDK call-site.

#include <cstdint>
#include <vector>

namespace wn_libsteamclient {

// Offset constants for CCallbackBase. Verified against AArch64 Itanium
// ABI: vptr=8B at offset 0, m_nCallbackFlags=1B at offset 8,
// m_iCallback=4B at offset 12 (3B padding before the int).
constexpr size_t kCCallbackBaseFlagsOffset    = 8;
constexpr size_t kCCallbackBaseIdOffset       = 12;
constexpr uint8_t kCallbackFlagsRegistered    = 0x01;
constexpr uint8_t kCallbackFlagsGameServer    = 0x02;

// Register a CCallbackBase to receive callbacks of [iCallback] type.
// Idempotent — re-registering the same cb under the same id is a no-op.
// Sets the kCallbackFlagsRegistered bit on the cb's m_nCallbackFlags so
// the SDK's CCallback dtor knows whether to call Unregister.
void register_callback(void* cb, int iCallback);

// Remove every entry that matches [cb]. Clears the kCallbackFlagsRegistered
// bit. Safe to call on a cb that was never registered.
void unregister_callback(void* cb);

// Snapshot the set of currently-registered callbacks for [iCallback].
// Caller invokes vtable[0] on each. We snapshot under the registry
// mutex then release it BEFORE invoking, so a callback's Run() that
// re-enters register/unregister doesn't deadlock or invalidate
// iterators.
[[nodiscard]] std::vector<void*> find_callbacks(int iCallback);

// Diagnostic — total number of registered callbacks across all ids.
[[nodiscard]] size_t registry_size();

// -----------------------------------------------------------------------
// CCallResult-side registry — async results identified by SteamAPICall_t.
// The SDK uses CCallResult<T,P> for one-shot async ops: a service method
// returns hCall, the caller registers via SteamAPI_RegisterCallResult,
// SteamAPI_RunCallbacks invokes vtable[1](payload, bIOFailure, hCall)
// when the result arrives.
//
// Storage shape: map<hCall, vector<cb*>>. Multiple CCallResults can
// observe the same hCall (rare but allowed).
// -----------------------------------------------------------------------

void register_call_result(void* cb, uint64_t hCall);
void unregister_call_result(void* cb, uint64_t hCall);
[[nodiscard]] std::vector<void*> find_call_result_cbs(uint64_t hCall);

// Diagnostic — total number of registered call-result handles.
[[nodiscard]] size_t call_result_registry_size();

}  // namespace wn_libsteamclient
