#include "wn_libsteamclient/callback_registry.h"

#include <android/log.h>
#include <mutex>
#include <unordered_map>
#include <vector>

#define WN_TAG "WnLibSteamClient"
#define WN_LOGI(...) __android_log_print(ANDROID_LOG_INFO, WN_TAG, __VA_ARGS__)

namespace wn_libsteamclient {

namespace {

// Multimap keyed by callback id (the SDK lets multiple CCallback<T,P>
// instances coexist on the same id — different observers of the same
// event). Stored as unordered_map<int, vector<void*>> for O(1) lookup
// and trivial snapshot semantics.
std::mutex                                g_registry_mu;
std::unordered_map<int, std::vector<void*>> g_registry;
size_t                                    g_total_size = 0;

inline uint8_t* flags_ptr(void* cb) {
    return reinterpret_cast<uint8_t*>(cb) + kCCallbackBaseFlagsOffset;
}

}  // namespace

void register_callback(void* cb, int iCallback) {
    if (!cb) return;
    std::lock_guard<std::mutex> lk(g_registry_mu);
    auto& bucket = g_registry[iCallback];
    // Idempotent re-register — the SDK's CCallback::Register may call
    // SteamAPI_RegisterCallback twice if the user calls Register on an
    // already-registered object. Don't double-insert.
    for (void* existing : bucket) {
        if (existing == cb) return;
    }
    bucket.push_back(cb);
    ++g_total_size;
    *flags_ptr(cb) |= kCallbackFlagsRegistered;
    WN_LOGI("register_callback(cb=%p, iCallback=%d) total=%zu",
            cb, iCallback, g_total_size);
}

void unregister_callback(void* cb) {
    if (!cb) return;
    std::lock_guard<std::mutex> lk(g_registry_mu);
    // Walk every bucket — we don't track which id `cb` was registered
    // under, so we may have to scan multiple. A single cb is normally
    // in exactly one bucket (the SDK's CCallback enforces this), but
    // a misuse that registered the same cb under several ids gets
    // cleanly torn down here.
    for (auto& [id, bucket] : g_registry) {
        for (auto it = bucket.begin(); it != bucket.end(); ) {
            if (*it == cb) {
                it = bucket.erase(it);
                --g_total_size;
            } else {
                ++it;
            }
        }
    }
    *flags_ptr(cb) &= ~kCallbackFlagsRegistered;
}

std::vector<void*> find_callbacks(int iCallback) {
    std::lock_guard<std::mutex> lk(g_registry_mu);
    auto it = g_registry.find(iCallback);
    if (it == g_registry.end()) return {};
    return it->second;  // copy — caller invokes outside the lock
}

size_t registry_size() {
    std::lock_guard<std::mutex> lk(g_registry_mu);
    return g_total_size;
}

// -----------------------------------------------------------------------
// Call-result registry (CCallResult<T,P>, keyed by SteamAPICall_t).
// -----------------------------------------------------------------------

namespace {
std::mutex                                       g_cr_mu;
std::unordered_map<uint64_t, std::vector<void*>> g_cr_registry;
size_t                                           g_cr_total_size = 0;
}  // namespace

void register_call_result(void* cb, uint64_t hCall) {
    if (!cb || hCall == 0) return;
    std::lock_guard<std::mutex> lk(g_cr_mu);
    auto& bucket = g_cr_registry[hCall];
    for (void* existing : bucket) {
        if (existing == cb) return;  // idempotent
    }
    bucket.push_back(cb);
    ++g_cr_total_size;
    *flags_ptr(cb) |= kCallbackFlagsRegistered;
    WN_LOGI("register_call_result(cb=%p, hCall=%llu) total=%zu",
            cb, static_cast<unsigned long long>(hCall), g_cr_total_size);
}

void unregister_call_result(void* cb, uint64_t hCall) {
    if (!cb) return;
    std::lock_guard<std::mutex> lk(g_cr_mu);
    // hCall=0 → unregister this cb from every hCall it's bound to.
    auto unbind_one = [&](std::vector<void*>& bucket) {
        for (auto it = bucket.begin(); it != bucket.end(); ) {
            if (*it == cb) { it = bucket.erase(it); --g_cr_total_size; }
            else { ++it; }
        }
    };
    if (hCall == 0) {
        for (auto& [_, bucket] : g_cr_registry) unbind_one(bucket);
    } else {
        auto it = g_cr_registry.find(hCall);
        if (it != g_cr_registry.end()) unbind_one(it->second);
    }
    *flags_ptr(cb) &= ~kCallbackFlagsRegistered;
}

std::vector<void*> find_call_result_cbs(uint64_t hCall) {
    std::lock_guard<std::mutex> lk(g_cr_mu);
    auto it = g_cr_registry.find(hCall);
    if (it == g_cr_registry.end()) return {};
    return it->second;
}

size_t call_result_registry_size() {
    std::lock_guard<std::mutex> lk(g_cr_mu);
    return g_cr_total_size;
}

}  // namespace wn_libsteamclient
