#include "wn_steam/cm_client.h"
#include "wn_steam/cm_bridge.h"

#include <android/log.h>

#include <cctype>
#include <thread>
#include <zlib.h>

#include "wn_steam/pb/cmsg_client_get_app_ownership_ticket.h"
#include "wn_steam/pb/cmsg_client_license_list.h"
#include "wn_steam/pb/cmsg_clientserver_login.h"
#include "wn_steam/proto_envelope.h"
#include "wn_steam/proto_wire.h"
#include "wn_steam/wire_format.h"
#include "wn_steam/ws_connection.h"

namespace wn_steam {

namespace {
constexpr const char* kLogTag = "WnSteamCM";
#define WN_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  kLogTag, __VA_ARGS__)
#define WN_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

template <typename Cb, typename... Args>
void safe_invoke(Cb& cb, Args&&... args) {
    if (!cb) return;
    try { cb(std::forward<Args>(args)...); }
    catch (const std::exception& e) { WN_LOGE("client callback threw: %s", e.what()); }
    catch (...) { WN_LOGE("client callback threw unknown"); }
}

// CMsgMulti wraps length-prefixed records, optionally compressed.
struct CMsgMulti {
    uint32_t              size_unzipped = 0;
    std::vector<uint8_t>  message_body;
};

bool parse_cmsg_multi(std::span<const uint8_t> body, CMsgMulti& out) {
    proto::Reader r(body);
    while (!r.eof()) {
        auto t = r.next_tag();
        if (!t) return r.ok();
        switch (t->field_number) {
            case 1:
                if (auto v = r.u32(); v) out.size_unzipped = *v; else return false;
                break;
            case 2:
                if (auto v = r.bytes(); v) {
                    out.message_body.assign(v->begin(), v->end());
                } else return false;
                break;
            default:
                if (!r.skip(t->wire_type)) return false;
                break;
        }
    }
    return true;
}

std::vector<uint8_t> gunzip(std::span<const uint8_t> compressed,
                            size_t expected_size) {
    std::vector<uint8_t> out;
    out.resize(expected_size > 0 ? expected_size
                                 : std::max<size_t>(compressed.size() * 4, 1024));

    z_stream zs{};
    // 15 = max window bits; +32 enables auto-detection of gzip/zlib wrapper.
    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
        WN_LOGE("inflateInit2 failed");
        return {};
    }
    zs.next_in   = const_cast<Bytef*>(compressed.data());
    zs.avail_in  = static_cast<uInt>(compressed.size());
    zs.next_out  = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    int ret;
    while (true) {
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            WN_LOGE("inflate failed rc=%d (avail_out=%u total_out=%lu)",
                    ret, zs.avail_out, static_cast<unsigned long>(zs.total_out));
            inflateEnd(&zs);
            return {};
        }
        if (zs.avail_out == 0) {
            const size_t old_size = out.size();
            out.resize(old_size * 2);
            zs.next_out  = out.data() + old_size;
            zs.avail_out = static_cast<uInt>(out.size() - old_size);
        }
    }
    out.resize(zs.total_out);
    inflateEnd(&zs);
    return out;
}
}  // namespace

CMClient::CMClient() {
    auto ws = std::make_unique<WsConnection>();
    channel_ = std::make_unique<EncryptedChannel>(std::move(ws));
    channel_->set_on_connected([this]() { on_channel_connected(); });
    channel_->set_on_disconnected(
        [this](ChannelDisconnectReason r, const std::string& d) {
            on_channel_disconnected(r, d);
        });
    channel_->set_on_message(
        [this](std::span<const uint8_t> bytes) { on_channel_message(bytes); });
}

CMClient::~CMClient() {
    disconnect();
}

void CMClient::set_ca_bundle_path(const std::string& path) {
    if (channel_) channel_->set_ca_bundle_path(path);
}

bool CMClient::connect(const std::string& url) {
    auto expected = ClientState::Disconnected;
    if (!state_.compare_exchange_strong(expected, ClientState::Connecting)) {
        return false;
    }
    set_state_locked_(ClientState::Connecting);
    if (!channel_->connect(url)) {
        set_state_locked_(ClientState::Disconnected);
        return false;
    }
    return true;
}

void CMClient::disconnect() {
    heartbeat_.stop();
    if (channel_) channel_->disconnect();
    jobs_.fail_all("CMClient disconnected");
    set_state_locked_(ClientState::Disconnected);
    steam_id_.store(0);
    session_id_.store(0);
    family_group_id_.store(0);
}

void CMClient::log_off_and_disconnect(std::chrono::milliseconds flush_window) {
    // CMsgClientLogOff has an empty body; the EMsg id is what matters.
    // Send + briefly let the channel flush before tearing down so Steam
    // sees the logoff before the socket goes away. We do NOT wait for
    // CMsgClientLoggedOff's eresult — best-effort: most of the value
    // is in Steam's CM processing the LogOff record before our follow-up
    // bootstrap LogonWithRefreshToken races in.
    if (state_.load() == ClientState::LoggedOn) {
        pb::CMsgClientLogOff msg;
        const bool ok = send_proto_message(EMsg::ClientLogOff, msg.serialize());
        WN_LOGI("CMsgClientLogOff sent (ok=%d) before hand-off disconnect; "
                "flush_window=%lldms",
                ok ? 1 : 0, static_cast<long long>(flush_window.count()));
        if (ok && flush_window.count() > 0) {
            std::this_thread::sleep_for(flush_window);
        }
    } else {
        WN_LOGI("log_off_and_disconnect: not LoggedOn (state=%d) — "
                "skipping LogOff send, going straight to disconnect",
                static_cast<int>(state_.load()));
    }
    disconnect();
}

void CMClient::call_service_method(std::string_view method_name,
                                   bool authed,
                                   std::span<const uint8_t> request_body,
                                   JobContinuation cb,
                                   std::chrono::seconds timeout) {
    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, std::move(cb), timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    hdr.target_job_name.assign(method_name.begin(), method_name.end());

    const EMsg outbound = authed ? EMsg::ServiceMethodCallFromClient
                                 : EMsg::ServiceMethodCallFromClientNonAuthed;
    WN_LOGI("outbound service_method=\"%.*s\" authed=%d jobid_source=0x%llx body=%zu bytes",
            static_cast<int>(method_name.size()), method_name.data(),
            authed ? 1 : 0,
            static_cast<unsigned long long>(job_id),
            request_body.size());
    auto wire = encode_proto_envelope(outbound, hdr, request_body);
    if (!channel_->send(wire)) {
        WN_LOGE("channel->send failed for service method \"%.*s\"",
                static_cast<int>(method_name.size()), method_name.data());
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

bool CMClient::send_proto_message(EMsg emsg, std::span<const uint8_t> body,
                                  uint32_t routing_appid) {
    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.routing_appid    = routing_appid;
    // Pre-logon ClientLogon needs SteamKit's anonymous desktop placeholder ID.
    if (emsg == EMsg::ClientLogon && hdr.steamid == 0) {
        hdr.steamid = 0x0110000100000000ULL;
    }
    auto wire = encode_proto_envelope(emsg, hdr, body);
    return channel_->send(wire);
}

bool CMClient::logon_with_refresh_token(const std::string& refresh_token,
                                         const std::string& account_name,
                                         uint64_t client_supplied_steam_id) {
    if (state_.load() != ClientState::Connected) return false;

    pb::CMsgClientLogon msg;
    msg.access_token             = refresh_token;
    msg.client_supplied_steam_id = client_supplied_steam_id;
    msg.account_name             = account_name;
    // Steam rejects user logon with an empty machine_id.
    static constexpr const char kMachineIdMarker[] = "WN-Steam-Client";
    msg.machine_id.assign(kMachineIdMarker,
                          kMachineIdMarker + sizeof(kMachineIdMarker) - 1);

    // Random LoginID prevents same-account session collisions.
    auto k = generate_session_key();
    if (k) {
        const auto& b = k->bytes;
        uint64_t r = 0;
        for (int i = 0; i < 8; ++i) r |= static_cast<uint64_t>(b[i]) << (i * 8);
        if (r == 0) r = 1;
        msg.client_instance_id = r;

        uint32_t login_id = 0;
        for (int i = 8; i < 12; ++i)
            login_id |= static_cast<uint32_t>(b[i]) << ((i - 8) * 8);
        if (login_id == 0) login_id = 0x57'4E'53'01;
        msg.obfuscated_private_ip = login_id;
    }
    return send_proto_message(EMsg::ClientLogon, msg.serialize());
}

void CMClient::pics_get_access_tokens(std::vector<uint32_t> packageids,
                                      std::vector<uint32_t> appids,
                                      PicsAccessTokenCallback cb,
                                      std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientPICSAccessTokenRequest req;
    req.packageids = std::move(packageids);
    req.appids     = std::move(appids);

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure || r.eresult <= 0) {
            cb(std::nullopt);
            return;
        }
        cb(pb::CMsgClientPICSAccessTokenResponse::deserialize(r.body));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientPICSAccessTokenRequest, hdr,
                                      req.serialize());
    WN_LOGI("outbound PICS access tokens jobid=0x%llx packages=%zu apps=%zu",
            static_cast<unsigned long long>(job_id),
            req.packageids.size(), req.appids.size());
    if (!channel_->send(wire)) {
        WN_LOGE("PICS access-token send failed");
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

void CMClient::pics_get_changes_since(uint32_t since_change_number,
                                      PicsChangesSinceCallback cb,
                                      std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientPICSChangesSinceRequest req;
    req.since_change_number = since_change_number;

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [since_change_number, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientPICSChangesSinceResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("PICS changes-since: parse failed (%zu bytes)", r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("PICS changes-since: since=%u current=%u apps=%zu packages=%zu full=%d",
                since_change_number, resp->current_change_number,
                resp->app_changes.size(), resp->package_changes.size(),
                static_cast<int>(resp->force_full_update));
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientPICSChangesSinceRequest, hdr,
                                      req.serialize());
    WN_LOGI("outbound PICS changes-since jobid=0x%llx since=%u",
            static_cast<unsigned long long>(job_id), since_change_number);
    if (!channel_->send(wire)) {
        WN_LOGE("PICS changes-since send failed");
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

void CMClient::pics_get_product_info(std::vector<pb::PicsPackageInfoReq> packages,
                                     std::vector<pb::PicsAppInfoReq> apps,
                                     bool meta_data_only,
                                     PicsProductInfoCallback cb,
                                     std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientPICSProductInfoRequest req;
    req.packages       = std::move(packages);
    req.apps           = std::move(apps);
    req.meta_data_only = meta_data_only;

    const uint64_t job_id = jobs_.next_job_id();

    // Response can race send completion.
    {
        std::lock_guard<std::mutex> lk(pics_mu_);
        pics_pending_[job_id] = PicsAggregate{{}, std::move(cb)};
    }

    // JobManager only handles timeout/disconnect here.
    jobs_.track(job_id, [this, job_id](JobResult r) {
        if (!r.synthetic_failure) return;
        PicsProductInfoCallback cb;
        {
            std::lock_guard<std::mutex> lk(pics_mu_);
            auto it = pics_pending_.find(job_id);
            if (it == pics_pending_.end()) return;
            cb = std::move(it->second.cb);
            pics_pending_.erase(it);
        }
        if (cb) cb(std::nullopt);
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientPICSProductInfoRequest, hdr,
                                      req.serialize());
    WN_LOGI("outbound PICS product info jobid=0x%llx packages=%zu apps=%zu meta=%d",
            static_cast<unsigned long long>(job_id),
            req.packages.size(), req.apps.size(),
            meta_data_only ? 1 : 0);
    if (!channel_->send(wire)) {
        WN_LOGE("PICS product-info send failed");
        PicsProductInfoCallback cb_local;
        {
            std::lock_guard<std::mutex> lk(pics_mu_);
            auto it = pics_pending_.find(job_id);
            if (it != pics_pending_.end()) {
                cb_local = std::move(it->second.cb);
                pics_pending_.erase(it);
            }
        }
        if (cb_local) cb_local(std::nullopt);
    }
}

void CMClient::get_app_ownership_ticket(uint32_t app_id,
                                         AppOwnershipTicketCallback cb,
                                         std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientGetAppOwnershipTicket req;
    req.app_id = app_id;

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [this, app_id, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientGetAppOwnershipTicketResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("ownership ticket: parse failed for app %u (%zu bytes)",
                    app_id, r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        // Cache only successful, non-empty tickets.
        if (resp->eresult == 1 && !resp->ticket.empty()) {
            tickets_.store(resp->app_id, resp->eresult, resp->ticket);
            WN_LOGI("ownership ticket: cached %u bytes for app %u",
                    static_cast<unsigned>(resp->ticket.size()), resp->app_id);
        } else {
            WN_LOGI("ownership ticket: app %u eresult=%u ticket=%zu bytes (not cached)",
                    app_id, resp->eresult, resp->ticket.size());
        }
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientGetAppOwnershipTicket, hdr,
                                      req.serialize());
    WN_LOGI("outbound ownership ticket request: app=%u jobid=0x%llx",
            app_id, static_cast<unsigned long long>(job_id));
    if (!channel_->send(wire)) {
        WN_LOGE("ownership ticket: channel send failed for app %u", app_id);
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

void CMClient::request_encrypted_app_ticket(uint32_t app_id,
                                            EncryptedAppTicketCallback cb,
                                            std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientRequestEncryptedAppTicket req;
    req.app_id = app_id;

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [app_id, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp =
            pb::CMsgClientRequestEncryptedAppTicketResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("encrypted app ticket: parse failed for app %u (%zu bytes)",
                    app_id, r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("encrypted app ticket: app %u eresult=%d ticket=%zu bytes",
                app_id, resp->eresult, resp->encrypted_app_ticket.size());
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientRequestEncryptedAppTicket, hdr,
                                      req.serialize());
    WN_LOGI("outbound encrypted app ticket request: app=%u jobid=0x%llx",
            app_id, static_cast<unsigned long long>(job_id));
    if (!channel_->send(wire)) {
        WN_LOGE("encrypted app ticket: channel send failed for app %u", app_id);
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

void CMClient::get_user_stats(uint32_t app_id,
                              UserStatsCallback cb,
                              std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientGetUserStats req;
    req.game_id           = app_id;
    req.steam_id_for_user = steam_id_.load();

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [app_id, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientGetUserStatsResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("user stats: parse failed for app %u (%zu bytes)",
                    app_id, r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("user stats: app %u eresult=%d schema=%zu bytes",
                app_id, resp->eresult, resp->schema.size());
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientGetUserStats, hdr,
                                      req.serialize());
    WN_LOGI("outbound user stats request: app=%u jobid=0x%llx",
            app_id, static_cast<unsigned long long>(job_id));
    if (!channel_->send(wire)) {
        WN_LOGE("user stats: channel send failed for app %u", app_id);
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

void CMClient::store_user_stats(
        uint32_t app_id, uint64_t steam_id, uint32_t crc_stats,
        const std::vector<std::pair<uint32_t, uint32_t>>& stats) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("store_user_stats: not logged on, dropping");
        return;
    }
    pb::CMsgClientStoreUserStats2 msg;
    msg.game_id         = app_id;
    msg.settor_steam_id = steam_id;
    msg.settee_steam_id = steam_id;
    msg.crc_stats       = crc_stats;
    msg.stats.reserve(stats.size());
    for (const auto& [id, val] : stats) {
        msg.stats.push_back(pb::CMsgClientStoreUserStats2::Stat{id, val});
    }
    // routing_appid lets Steam's GS backend route the write to this app.
    if (send_proto_message(EMsg::ClientStoreUserStats2, msg.serialize(), app_id)) {
        WN_LOGI("store_user_stats: sent app=%u stats=%zu crc=%u",
                app_id, stats.size(), crc_stats);
    } else {
        WN_LOGE("store_user_stats: send failed for app %u", app_id);
    }
}

void CMClient::get_depot_decryption_key(uint32_t depot_id, uint32_t app_id,
                                         DepotDecryptionKeyCallback cb,
                                         std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientGetDepotDecryptionKey req;
    req.depot_id = depot_id;
    req.app_id   = app_id;

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [depot_id, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientGetDepotDecryptionKeyResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("depot key: parse failed for depot %u (%zu bytes)",
                    depot_id, r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("depot key: depot %u eresult=%u key=%zu bytes",
                resp->depot_id, resp->eresult, resp->depot_encryption_key.size());
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientGetDepotDecryptionKey, hdr,
                                      req.serialize());
    WN_LOGI("outbound depot key request: depot=%u app=%u jobid=0x%llx",
            depot_id, app_id, static_cast<unsigned long long>(job_id));
    if (!channel_->send(wire)) {
        WN_LOGE("depot key: channel send failed for depot %u", depot_id);
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

void CMClient::get_manifest_request_code(uint32_t app_id, uint32_t depot_id,
                                          uint64_t manifest_id, std::string branch,
                                          ManifestRequestCodeCallback cb,
                                          std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CContentServerDirectory_GetManifestRequestCode_Request req;
    req.app_id      = app_id;
    req.depot_id    = depot_id;
    req.manifest_id = manifest_id;
    // JavaSteam omits app_branch for public/empty branches.
    std::string lower;
    lower.reserve(branch.size());
    for (char c : branch) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (!lower.empty() && lower != "public") {
        req.app_branch = std::move(branch);
    }

    call_service_method(
        "ContentServerDirectory.GetManifestRequestCode#1",
        /*authed=*/true,
        req.serialize(),
        [depot_id, manifest_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("manifest request code: depot %u gid %llu failed eresult=%d",
                        depot_id, static_cast<unsigned long long>(manifest_id), r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CContentServerDirectory_GetManifestRequestCode_Response
                            ::deserialize(r.body);
            if (!resp) {
                WN_LOGE("manifest request code: parse failed (%zu bytes)", r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("manifest request code: depot %u gid %llu -> code %llu",
                    depot_id, static_cast<unsigned long long>(manifest_id),
                    static_cast<unsigned long long>(resp->manifest_request_code));
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::get_cdn_servers(uint32_t cell_id, CdnServersCallback cb,
                                std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CContentServerDirectory_GetServersForSteamPipe_Request req;
    req.cell_id = cell_id;

    call_service_method(
        "ContentServerDirectory.GetServersForSteamPipe#1",
        /*authed=*/true,
        req.serialize(),
        [cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cdn servers: request failed eresult=%d", r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CContentServerDirectory_GetServersForSteamPipe_Response
                            ::deserialize(r.body);
            if (!resp) {
                WN_LOGE("cdn servers: parse failed (%zu bytes)", r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("cdn servers: %zu server(s)", resp->servers.size());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_get_user_quota(CloudUserQuotaCallback cb,
                                    std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CCloud_GetUserQuota_Request req;
    call_service_method(
        "Cloud.GetUserQuota#1",
        /*authed=*/true,
        req.serialize(),
        [cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cloud user-quota: failed eresult=%d", r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CCloud_GetUserQuota_Response::deserialize(r.body);
            if (!resp) {
                WN_LOGE("cloud user-quota: parse failed (%zu bytes)", r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("cloud user-quota: total=%llu used=%llu",
                    static_cast<unsigned long long>(resp->total_bytes),
                    static_cast<unsigned long long>(resp->used_bytes));
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_get_app_file_changelist(uint32_t app_id,
                                             uint64_t synced_change_number,
                                             CloudFileChangelistCallback cb,
                                             std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CCloud_GetAppFileChangelist_Request req;
    req.appid                = app_id;
    req.synced_change_number = synced_change_number;

    call_service_method(
        "Cloud.GetAppFileChangelist#1",
        /*authed=*/true,
        req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cloud changelist: app %u failed eresult=%d",
                        app_id, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CCloud_GetAppFileChangelist_Response::deserialize(r.body);
            if (!resp) {
                WN_LOGE("cloud changelist: parse failed for app %u (%zu bytes)",
                        app_id, r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("cloud changelist: app %u change=%llu files=%zu",
                    app_id,
                    static_cast<unsigned long long>(resp->current_change_number),
                    resp->files.size());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::inventory_get_item_def_meta(uint32_t app_id,
                                           ItemDefMetaCallback cb,
                                           std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CInventory_GetItemDefMeta_Request req;
    req.appid = app_id;

    call_service_method(
        "Inventory.GetItemDefMeta#1",
        /*authed=*/true,
        req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("itemdef meta: app %u failed eresult=%d",
                        app_id, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CInventory_GetItemDefMeta_Response::deserialize(r.body);
            if (!resp) {
                WN_LOGE("itemdef meta: parse failed for app %u (%zu bytes)",
                        app_id, r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("itemdef meta: app %u modified=%u digest=%s",
                    app_id, resp->modified,
                    resp->digest.empty() ? "<empty>" : resp->digest.c_str());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::published_file_get_subscribed(uint32_t app_id, uint32_t page,
                                             uint32_t num_per_page,
                                             PublishedFileUserFilesCallback cb,
                                             std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CPublishedFile_GetUserFiles_Request req;
    req.steamid    = steam_id_.load();
    req.appid      = app_id;
    req.page       = page;
    req.numperpage = num_per_page;
    req.type       = "mysubscriptions";
    req.filetype   = 0xFFFFFFFFu;

    call_service_method(
        "PublishedFile.GetUserFiles#1",
        /*authed=*/true,
        req.serialize(),
        [app_id, page, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("workshop subs: app %u page %u failed eresult=%d",
                        app_id, page, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp =
                pb::CPublishedFile_GetUserFiles_Response::deserialize(r.body);
            if (!resp) {
                WN_LOGE("workshop subs: parse failed app %u page %u (%zu bytes)",
                        app_id, page, r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("workshop subs: app %u page %u total=%u items=%zu",
                    app_id, page, resp->total,
                    resp->publishedfiledetails.size());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_get_file_download_info(uint32_t app_id, std::string filename,
                                            CloudFileDownloadCallback cb,
                                            std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CCloud_ClientFileDownload_Request req;
    req.appid    = app_id;
    req.filename = std::move(filename);
    req.realm    = 1;

    call_service_method(
        "Cloud.ClientFileDownload#1",
        /*authed=*/true,
        req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cloud download info: app %u failed eresult=%d",
                        app_id, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CCloud_ClientFileDownload_Response::deserialize(r.body);
            if (!resp) {
                WN_LOGE("cloud download info: parse failed for app %u (%zu bytes)",
                        app_id, r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("cloud download info: app %u host=%s size=%u https=%d enc=%d",
                    app_id, resp->url_host.c_str(), resp->raw_file_size,
                    static_cast<int>(resp->use_https),
                    static_cast<int>(resp->encrypted));
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_begin_app_upload_batch(uint32_t app_id, std::string machine_name,
                                            std::vector<std::string> files_to_upload,
                                            std::vector<std::string> files_to_delete,
                                            uint64_t client_id,
                                            CloudBeginBatchCallback cb,
                                            std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CCloud_BeginAppUploadBatch_Request req;
    req.appid           = app_id;
    req.machine_name    = std::move(machine_name);
    req.files_to_upload = std::move(files_to_upload);
    req.files_to_delete = std::move(files_to_delete);
    req.client_id       = client_id;

    call_service_method(
        "Cloud.BeginAppUploadBatch#1", /*authed=*/true, req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cloud begin batch: app %u failed eresult=%d", app_id, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CCloud_BeginAppUploadBatch_Response::deserialize(r.body);
            if (!resp) { if (cb) cb(std::nullopt); return; }
            WN_LOGI("cloud begin batch: app %u batch_id=%llu", app_id,
                    static_cast<unsigned long long>(resp->batch_id));
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_begin_file_upload(uint32_t app_id, std::string filename,
                                       uint32_t file_size, uint32_t raw_file_size,
                                       std::vector<uint8_t> file_sha, uint64_t time_stamp,
                                       uint64_t upload_batch_id,
                                       CloudBeginFileUploadCallback cb,
                                       std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CCloud_ClientBeginFileUpload_Request req;
    req.appid           = app_id;
    req.filename        = std::move(filename);
    req.file_size       = file_size;
    req.raw_file_size   = raw_file_size;
    req.file_sha        = std::move(file_sha);
    req.time_stamp      = time_stamp;
    req.upload_batch_id = upload_batch_id;

    call_service_method(
        "Cloud.ClientBeginFileUpload#1", /*authed=*/true, req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cloud begin file upload: app %u failed eresult=%d", app_id, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CCloud_ClientBeginFileUpload_Response::deserialize(r.body);
            if (!resp) { if (cb) cb(std::nullopt); return; }
            WN_LOGI("cloud begin file upload: app %u blocks=%zu", app_id,
                    resp->block_requests.size());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_commit_file_upload(bool transfer_succeeded, uint32_t app_id,
                                        std::vector<uint8_t> file_sha, std::string filename,
                                        CloudCommitFileUploadCallback cb,
                                        std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CCloud_ClientCommitFileUpload_Request req;
    req.transfer_succeeded = transfer_succeeded;
    req.appid              = app_id;
    req.file_sha           = std::move(file_sha);
    req.filename           = std::move(filename);

    call_service_method(
        "Cloud.ClientCommitFileUpload#1", /*authed=*/true, req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cloud commit upload: app %u failed eresult=%d", app_id, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CCloud_ClientCommitFileUpload_Response::deserialize(r.body);
            if (!resp) { if (cb) cb(std::nullopt); return; }
            WN_LOGI("cloud commit upload: app %u committed=%d", app_id,
                    static_cast<int>(resp->file_committed));
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_complete_app_upload_batch(uint32_t app_id, uint64_t batch_id,
                                               uint32_t batch_eresult,
                                               CloudCompleteBatchCallback cb,
                                               std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(false);
        return;
    }
    pb::CCloud_CompleteAppUploadBatch_Request req;
    req.appid         = app_id;
    req.batch_id      = batch_id;
    req.batch_eresult = batch_eresult;

    call_service_method(
        "Cloud.CompleteAppUploadBatchBlocking#1", /*authed=*/true, req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            const bool ok = !r.synthetic_failure && r.eresult == 1;
            WN_LOGI("cloud complete batch: app %u ok=%d eresult=%d",
                    app_id, static_cast<int>(ok), r.eresult);
            if (cb) cb(ok);
        },
        timeout);
}

void CMClient::notify_games_played(const pb::CMsgClientGamesPlayed& msg) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("notify_games_played: not logged on, dropping");
        return;
    }
    if (send_proto_message(EMsg::ClientGamesPlayedWithDataBlob, msg.serialize())) {
        WN_LOGI("notify_games_played: %zu game(s) reported", msg.games_played.size());
    } else {
        WN_LOGE("notify_games_played: send failed");
    }
}

void CMClient::set_rich_presence(
        uint32_t app_id,
        const std::vector<pb::CPlayer_SetRichPresence_KV>& kv) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("set_rich_presence(%u): not logged on, dropping", app_id);
        return;
    }
    pb::CPlayer_SetRichPresence_Request req;
    req.appid         = app_id;
    req.rich_presence = kv;
    // Fire-and-forget — Steam doesn't ack the RP write directly; the
    // follow-up CMsgClientPersonaState for self carries the echoed
    // rich_presence map which our persona_observer mirrors back.
    // Calling with no callback gives a 5-second default timeout we
    // ignore (the response is just an empty acknowledgment).
    call_service_method(
        "Player.SetRichPresence#1",
        /*authed=*/true,
        req.serialize(),
        [app_id, count = kv.size()](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("set_rich_presence(%u, %zu keys): eresult=%d",
                        app_id, count, r.eresult);
            } else {
                WN_LOGI("set_rich_presence(%u, %zu keys): OK", app_id, count);
            }
        },
        std::chrono::seconds{5});
}

void CMClient::cloud_signal_app_launch_intent(uint32_t app_id, uint64_t client_id,
                                              std::string machine_name,
                                              bool ignore_pending_operations,
                                              int32_t os_type,
                                              CloudAppLaunchIntentCallback cb,
                                              std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CCloud_AppLaunchIntent_Request req;
    req.appid                     = app_id;
    req.client_id                 = client_id;
    req.machine_name              = std::move(machine_name);
    req.ignore_pending_operations = ignore_pending_operations;
    req.os_type                   = os_type;

    call_service_method(
        "Cloud.SignalAppLaunchIntent#1", /*authed=*/true, req.serialize(),
        [app_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("cloud launch intent: app %u failed eresult=%d", app_id, r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CCloud_AppLaunchIntent_Response::deserialize(r.body);
            if (!resp) { if (cb) cb(std::nullopt); return; }
            WN_LOGI("cloud launch intent: app %u pending_ops=%zu",
                    app_id, resp->pending_operation_codes.size());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::cloud_signal_app_exit_sync_done(uint32_t app_id, uint64_t client_id,
                                               bool uploads_completed,
                                               bool uploads_required) {
    if (state_.load() != ClientState::LoggedOn) return;
    pb::CCloud_AppExitSyncDone_Notification req;
    req.appid             = app_id;
    req.client_id         = client_id;
    req.uploads_completed = uploads_completed;
    req.uploads_required  = uploads_required;
    call_service_method(
        "Cloud.SignalAppExitSyncDone#1", /*authed=*/true, req.serialize(),
        [app_id](JobResult /*r*/) {
            WN_LOGI("cloud exit sync done: app %u signalled", app_id);
        });
}

void CMClient::set_persona_state(uint32_t persona_state) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("set_persona_state: not logged on, dropping");
        return;
    }
    pb::CMsgClientChangeStatus msg;
    msg.persona_state = persona_state;
    if (send_proto_message(EMsg::ClientChangeStatus, msg.serialize())) {
        WN_LOGI("set_persona_state: sent (state=%u)", persona_state);
    } else {
        WN_LOGE("set_persona_state: send failed");
    }
}

void CMClient::set_persona_name(const std::string& name,
                                uint32_t persona_state_keep_current) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("set_persona_name: not logged on, dropping");
        return;
    }
    pb::CMsgClientChangeStatus msg;
    msg.persona_state = persona_state_keep_current;
    msg.player_name   = name;
    if (send_proto_message(EMsg::ClientChangeStatus, msg.serialize())) {
        WN_LOGI("set_persona_name: sent (name='%s' state=%u %zu B)",
                name.c_str(), persona_state_keep_current, msg.serialize().size());
    } else {
        WN_LOGE("set_persona_name: send failed");
    }
}

void CMClient::request_user_persona() {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("request_user_persona: not logged on, dropping");
        return;
    }
    pb::CMsgClientRequestFriendData req;
    req.persona_state_requested = 0xFFFF;   // request all standard fields
    req.friends.push_back(steam_id_.load());
    if (send_proto_message(EMsg::ClientRequestFriendData, req.serialize())) {
        WN_LOGI("request_user_persona: requested for steamid=%llu",
                static_cast<unsigned long long>(steam_id_.load()));
    } else {
        WN_LOGE("request_user_persona: send failed");
    }
}

void CMClient::request_friend_personas(const std::vector<uint64_t>& sids,
                                       uint32_t persona_state_requested) {
    if (sids.empty()) return;
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("request_friend_personas: not logged on, dropping (n=%zu)",
                sids.size());
        return;
    }
    // CMsgClientRequestFriendData accepts a repeated list; sending a
    // single message is more efficient than fanning out per friend.
    pb::CMsgClientRequestFriendData req;
    req.persona_state_requested = persona_state_requested;
    req.friends = sids;  // copy is fine — caller's vector is small
    if (send_proto_message(EMsg::ClientRequestFriendData, req.serialize())) {
        WN_LOGI("request_friend_personas: requested %zu friend(s) flags=0x%x",
                sids.size(), static_cast<unsigned>(persona_state_requested));
    } else {
        WN_LOGE("request_friend_personas: send failed (n=%zu)", sids.size());
    }
}

std::optional<pb::PersonaStateFriend> CMClient::self_persona() const {
    std::lock_guard<std::mutex> lk(persona_mu_);
    return self_persona_;
}

std::vector<pb::License> CMClient::license_list() const {
    std::lock_guard<std::mutex> lk(license_mu_);
    return license_list_;
}

std::vector<uint64_t> CMClient::friends_list() const {
    std::lock_guard<std::mutex> lk(friends_mu_);
    // Only surface mutual friends (relationship == 3 / Friend) — the
    // public ISteamFriends.GetFriendCount semantics expect "people I am
    // friends with", not pending requests / blocked / etc. Other states
    // are still tracked internally for future EFriendFlags filtering.
    std::vector<uint64_t> out;
    out.reserve(friends_.size());
    for (const auto& [sid, rel] : friends_) {
        if (rel == 3) out.push_back(sid);
    }
    return out;
}

std::vector<CMClient::FriendPersonaSnapshot>
CMClient::friend_personas() const {
    std::lock_guard<std::mutex> lk(friend_personas_mu_);
    std::vector<FriendPersonaSnapshot> out;
    out.reserve(friend_personas_.size());
    for (const auto& [sid, snap] : friend_personas_) {
        if (!snap.player_name.empty()) out.push_back(snap);
    }
    return out;
}

void CMClient::get_family_group(uint64_t family_group_id,
                                FamilyGroupCallback cb,
                                std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CFamilyGroups_GetFamilyGroup_Request req;
    req.family_groupid = family_group_id;

    call_service_method(
        "FamilyGroups.GetFamilyGroup#1",
        /*authed=*/true,
        req.serialize(),
        [family_group_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("get_family_group: group %llu failed eresult=%d",
                        static_cast<unsigned long long>(family_group_id),
                        r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CFamilyGroups_GetFamilyGroup_Response::deserialize(r.body);
            if (!resp) {
                WN_LOGE("get_family_group: parse failed (%zu bytes)", r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("get_family_group: '%s' members=%zu",
                    resp->name.c_str(), resp->members.size());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::get_owned_games(uint64_t steam_id, OwnedGamesCallback cb,
                               std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CPlayer_GetOwnedGames_Request req;
    req.steamid                   = steam_id;
    req.include_appinfo           = true;
    req.include_played_free_games = true;
    req.include_free_sub          = true;
    req.include_extended_appinfo  = true;

    call_service_method(
        "Player.GetOwnedGames#1",
        /*authed=*/true,
        req.serialize(),
        [steam_id, cb = std::move(cb)](JobResult r) {
            if (r.synthetic_failure || r.eresult != 1) {
                WN_LOGE("get_owned_games: steamid %llu failed eresult=%d",
                        static_cast<unsigned long long>(steam_id), r.eresult);
                if (cb) cb(std::nullopt);
                return;
            }
            auto resp = pb::CPlayer_GetOwnedGames_Response::deserialize(r.body);
            if (!resp) {
                WN_LOGE("get_owned_games: parse failed (%zu bytes)", r.body.size());
                if (cb) cb(std::nullopt);
                return;
            }
            WN_LOGI("get_owned_games: steamid %llu games=%zu",
                    static_cast<unsigned long long>(steam_id),
                    resp->games.size());
            if (cb) cb(std::move(resp));
        },
        timeout);
}

void CMClient::kick_playing_session(bool only_stop_game) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("kick_playing_session: not logged on, dropping");
        return;
    }
    pb::CMsgClientKickPlayingSession msg;
    msg.only_stop_game = only_stop_game;
    if (send_proto_message(EMsg::ClientKickPlayingSession, msg.serialize())) {
        WN_LOGI("kick_playing_session: sent (only_stop_game=%d)",
                static_cast<int>(only_stop_game));
    } else {
        WN_LOGE("kick_playing_session: send failed");
    }
}

void CMClient::prepare_app(uint32_t app_id,
                            std::vector<uint32_t> dlc_app_ids,
                            PrepareAppCallback cb,
                            std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        if (cb) cb(false, "not logged on");
        return;
    }

    std::vector<uint32_t> all_ids;
    all_ids.reserve(1 + dlc_app_ids.size());
    if (app_id != 0) all_ids.push_back(app_id);
    for (uint32_t d : dlc_app_ids) {
        if (d == 0 || d == app_id) continue;
        if (std::find(all_ids.begin(), all_ids.end(), d) != all_ids.end()) continue;
        all_ids.push_back(d);
    }
    if (all_ids.empty()) {
        if (cb) cb(true, "");
        return;
    }

    WN_LOGI("prepare_app(%u): pre-warming %zu app(s) (1 parent + %zu DLC)",
            app_id, all_ids.size(), all_ids.size() - 1);

    // Force a fresh PICS read for the app and DLC right before launch.
    auto missing_tokens = std::vector<uint32_t>{};
    for (uint32_t id : all_ids) {
        auto entry = library_.find_app(id);
        if (entry && entry->missing_token && entry->access_token == 0) {
            missing_tokens.push_back(id);
        }
    }

    auto do_product_info = [this, all_ids, app_id, cb = std::move(cb), timeout]() mutable {
        std::vector<pb::PicsAppInfoReq> req;
        req.reserve(all_ids.size());
        for (uint32_t id : all_ids) {
            uint64_t token = 0;
            auto e = library_.find_app(id);
            if (e) token = e->access_token;
            req.push_back(pb::PicsAppInfoReq{id, token, false});
        }
        pics_get_product_info({}, std::move(req), /*meta_data_only=*/false,
            [this, all_ids = std::move(all_ids), app_id,
             cb = std::move(cb), timeout](std::optional<pb::CMsgClientPICSProductInfoResponse> resp) mutable {
                if (!resp) {
                    WN_LOGE("prepare_app(%u): PICS product info failed", app_id);
                    if (cb) cb(false, "PICS product info failed");
                    return;
                }
                library_.ingest_app_pics_response(*resp);
                size_t ready = 0;
                for (uint32_t id : all_ids) {
                    auto e = library_.find_app(id);
                    if (e && (e->pics_fetched || !e->name.empty())) ++ready;
                }
                WN_LOGI("prepare_app(%u): PICS ready (%zu/%zu apps cached); "
                        "fetching ownership tickets",
                        app_id, ready, all_ids.size());

                // Fetch tickets concurrently; partial caches are still useful.
                struct Counter {
                    std::atomic<size_t> remaining;
                    std::atomic<size_t> ok{0};
                    AppOwnershipTicketCallback nop = nullptr;
                    PrepareAppCallback final_cb;
                    uint32_t parent_app_id;
                    size_t   total;
                };
                auto counter = std::make_shared<Counter>();
                counter->remaining = all_ids.size();
                counter->total     = all_ids.size();
                counter->parent_app_id = app_id;
                counter->final_cb  = std::move(cb);

                for (uint32_t id : all_ids) {
                    get_app_ownership_ticket(id,
                        [counter](std::optional<pb::CMsgClientGetAppOwnershipTicketResponse> r) {
                            if (r && r->eresult == 1 && !r->ticket.empty()) {
                                counter->ok.fetch_add(1);
                            }
                            if (counter->remaining.fetch_sub(1) == 1) {
                                size_t ok = counter->ok.load();
                                WN_LOGI("prepare_app(%u): ownership tickets %zu/%zu OK",
                                        counter->parent_app_id, ok, counter->total);
                                if (counter->final_cb) {
                                    counter->final_cb(true, "");
                                }
                            }
                        }, timeout);
                }
            }, timeout);
    };

    if (missing_tokens.empty()) {
        do_product_info();
        return;
    }
    WN_LOGI("prepare_app(%u): requesting access tokens for %zu restricted apps",
            app_id, missing_tokens.size());
    pics_get_access_tokens({}, std::move(missing_tokens),
        [this, do_product_info = std::move(do_product_info), app_id](
            std::optional<pb::CMsgClientPICSAccessTokenResponse> resp) mutable {
            if (resp) library_.ingest_app_access_tokens(*resp);
            else WN_LOGE("prepare_app(%u): access-token request failed; continuing anyway", app_id);
            do_product_info();
        }, timeout);
}

void CMClient::set_on_state(StateCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    on_state_ = std::move(cb);
}

void CMClient::set_on_client_message(ClientMessageCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mu_);
    on_client_message_ = std::move(cb);
}

void CMClient::on_channel_connected() {
    set_state_locked_(ClientState::Connected);
    WN_LOGI("encrypted channel up; sending ClientHello");
    pb::CMsgClientHello hello;
    send_proto_message(EMsg::ClientHello, hello.serialize());
}

void CMClient::on_channel_disconnected(ChannelDisconnectReason r, const std::string& detail) {
    (void)r;
    WN_LOGI("channel disconnected: %s", detail.c_str());
    heartbeat_.stop();
    jobs_.fail_all("channel disconnected: " + detail);
    steam_id_.store(0);
    session_id_.store(0);
    family_group_id_.store(0);
    set_state_locked_(ClientState::Disconnected);
}

void CMClient::on_channel_message(std::span<const uint8_t> bytes) {
    auto env = decode_proto_envelope(bytes);
    if (env) {
        route_inbound_(env->emsg, env->header, env->body);
        return;
    }

    // WSS sometimes sends legacy ChannelEncrypt* frames; TLS already handles it.
    if (bytes.size() >= 4) {
        const uint32_t raw_emsg = wire::read_u32_le(bytes.data());
        if (!emsg_has_proto_flag(raw_emsg)) {
            const EMsg legacy = emsg_strip_proto_flag(raw_emsg);
            switch (legacy) {
                case EMsg::ChannelEncryptRequest:
                case EMsg::ChannelEncryptResponse:
                case EMsg::ChannelEncryptResult:
                    WN_LOGI("ignored legacy %u-byte ChannelEncrypt* message on WSS "
                            "(emsg=%u — TLS handles encryption)",
                            static_cast<unsigned>(bytes.size()),
                            static_cast<unsigned>(legacy));
                    return;
                default:
                    WN_LOGE("unexpected non-proto inbound emsg=%u, size=%zu (dropping)",
                            static_cast<unsigned>(legacy), bytes.size());
                    return;
            }
        }
    }

    WN_LOGE("decode_proto_envelope failed (size=%zu)", bytes.size());
}

void CMClient::route_inbound_(EMsg emsg,
                              const CMsgProtoBufHeader& header,
                              std::span<const uint8_t> body) {
    WN_LOGI("inbound emsg=%u eresult=%d jobid_target=0x%llx "
            "target_job_name=\"%s\" body=%zu bytes",
            static_cast<unsigned>(emsg),
            header.eresult,
            static_cast<unsigned long long>(header.jobid_target),
            header.target_job_name.c_str(),
            body.size());

    if (!body.empty()) {
        char hex[3 * 32 + 1];
        size_t n = std::min<size_t>(body.size(), 32);
        size_t off = 0;
        for (size_t i = 0; i < n; ++i) {
            off += static_cast<size_t>(std::snprintf(hex + off, sizeof(hex) - off,
                                                     "%02x ", body[i]));
        }
        WN_LOGI("  body[0..%zu]: %s", n, hex);
    }

    switch (emsg) {
        case EMsg::Multi: {
            // Re-dispatch inner CMsgMulti records through the same router.
            CMsgMulti multi;
            if (!parse_cmsg_multi(body, multi)) {
                WN_LOGE("CMsgMulti parse failed");
                return;
            }
            std::vector<uint8_t> unzipped;
            std::span<const uint8_t> records;
            if (multi.size_unzipped > 0) {
                unzipped = gunzip(multi.message_body, multi.size_unzipped);
                if (unzipped.empty()) {
                    WN_LOGE("CMsgMulti: gunzip yielded empty payload "
                            "(size_unzipped=%u, compressed=%zu bytes)",
                            multi.size_unzipped, multi.message_body.size());
                    return;
                }
                records = unzipped;
            } else {
                records = multi.message_body;
            }
            size_t off = 0;
            int dispatched = 0;
            while (off + 4 <= records.size()) {
                const uint32_t inner_len = wire::read_u32_le(records.data() + off);
                off += 4;
                if (inner_len == 0 || off + inner_len > records.size()) {
                    WN_LOGE("CMsgMulti: malformed inner record at offset %zu "
                            "(len=%u, remaining=%zu)",
                            off - 4, inner_len, records.size() - off);
                    break;
                }
                on_channel_message(records.subspan(off, inner_len));
                ++dispatched;
                off += inner_len;
            }
            WN_LOGI("CMsgMulti: dispatched %d inner messages", dispatched);
            break;
        }

        case EMsg::ServiceMethodResponse:
            jobs_.deliver(header.jobid_target,
                          header.eresult,
                          header.error_message,
                          body);
            break;

        case EMsg::ClientPICSAccessTokenResponse:
        case EMsg::ClientPICSChangesSinceResponse:
        case EMsg::ClientGetAppOwnershipTicketResponse:
        case EMsg::ClientRequestEncryptedAppTicketResponse:
        case EMsg::ClientGetUserStatsResponse:
        case EMsg::ClientGetDepotDecryptionKeyResponse:
        case EMsg::ClientMMSCreateLobbyResponse:
        case EMsg::ClientMMSJoinLobbyResponse:
        case EMsg::ClientMMSLeaveLobbyResponse:
        case EMsg::ClientMMSGetLobbyListResponse:
        case EMsg::ClientMMSSetLobbyDataResponse:
        case EMsg::ClientMMSSetLobbyOwnerResponse:
        case EMsg::ClientMMSGetLobbyStatusResponse:
            // Single-shot response — JobManager handles routing + parse.
            jobs_.deliver(header.jobid_target,
                          header.eresult,
                          header.error_message,
                          body);
            break;

        case EMsg::ClientMMSLobbyData: {
            // Server push — emitted whenever a lobby the client is
            // subscribed to changes (metadata edit, member join/leave,
            // owner change). Decode + forward to the observer; the
            // observer (registered by libsteamclient.so) mirrors into
            // pushed().active_lobbies and emits LobbyDataUpdate_t.
            auto resp = pb::CMsgClientMMSLobbyData::deserialize(body);
            if (!resp) {
                WN_LOGE("MMSLobbyData parse failed (%zu bytes)", body.size());
                break;
            }
            WN_LOGI("MMSLobbyData push: lobby=0x%llx owner=0x%llx members=%d/%d",
                    static_cast<unsigned long long>(resp->steam_id_lobby),
                    static_cast<unsigned long long>(resp->steam_id_owner),
                    static_cast<int>(resp->members.size()),
                    resp->max_members);
            if (lobby_data_observer_) lobby_data_observer_(*resp);
            break;
        }
        case EMsg::ClientMMSLobbyChatMsg: {
            auto resp = pb::CMsgClientMMSLobbyChatMsg::deserialize(body);
            if (!resp) {
                WN_LOGE("MMSLobbyChatMsg parse failed (%zu bytes)", body.size());
                break;
            }
            WN_LOGI("MMSLobbyChatMsg push: lobby=0x%llx sender=0x%llx body=%zuB",
                    static_cast<unsigned long long>(resp->steam_id_lobby),
                    static_cast<unsigned long long>(resp->steam_id_sender),
                    resp->lobby_message.size());
            if (lobby_chat_msg_observer_) lobby_chat_msg_observer_(*resp);
            break;
        }
        case EMsg::ClientMMSUserJoinedLobby:
        case EMsg::ClientMMSUserLeftLobby: {
            auto resp = pb::CMsgClientMMSUserJoinedOrLeftLobby::deserialize(body);
            if (!resp) {
                WN_LOGE("MMSUser{Join,Left}Lobby parse failed (%zu bytes)",
                        body.size());
                break;
            }
            const bool joined = (emsg == EMsg::ClientMMSUserJoinedLobby);
            WN_LOGI("MMS%s push: lobby=0x%llx user=0x%llx '%s'",
                    joined ? "UserJoinedLobby" : "UserLeftLobby",
                    static_cast<unsigned long long>(resp->steam_id_lobby),
                    static_cast<unsigned long long>(resp->steam_id_user),
                    resp->persona_name.c_str());
            if (lobby_membership_observer_)
                lobby_membership_observer_(joined, *resp);
            break;
        }

        case EMsg::ClientPICSProductInfoResponse: {
            // Merge multi-part PICS replies before firing the callback.
            auto resp = pb::CMsgClientPICSProductInfoResponse::deserialize(body);
            if (!resp) {
                WN_LOGE("PICS product-info parse failed (%zu bytes)", body.size());
                PicsProductInfoCallback cb;
                {
                    std::lock_guard<std::mutex> lk(pics_mu_);
                    auto it = pics_pending_.find(header.jobid_target);
                    if (it != pics_pending_.end()) {
                        cb = std::move(it->second.cb);
                        pics_pending_.erase(it);
                    }
                }
                if (cb) cb(std::nullopt);
                break;
            }
            PicsProductInfoCallback final_cb;
            pb::CMsgClientPICSProductInfoResponse merged;
            {
                std::lock_guard<std::mutex> lk(pics_mu_);
                auto it = pics_pending_.find(header.jobid_target);
                if (it == pics_pending_.end()) {
                    WN_LOGI("PICS product-info: unknown jobid_target=0x%llx (timed out?)",
                            static_cast<unsigned long long>(header.jobid_target));
                    break;
                }
                auto& acc = it->second.acc;
                acc.apps.insert(acc.apps.end(),
                                std::make_move_iterator(resp->apps.begin()),
                                std::make_move_iterator(resp->apps.end()));
                acc.packages.insert(acc.packages.end(),
                                    std::make_move_iterator(resp->packages.begin()),
                                    std::make_move_iterator(resp->packages.end()));
                acc.unknown_appids.insert(acc.unknown_appids.end(),
                                          resp->unknown_appids.begin(),
                                          resp->unknown_appids.end());
                acc.unknown_packageids.insert(acc.unknown_packageids.end(),
                                              resp->unknown_packageids.begin(),
                                              resp->unknown_packageids.end());
                if (resp->http_min_size > 0) acc.http_min_size = resp->http_min_size;
                if (!resp->http_host.empty()) acc.http_host    = resp->http_host;
                acc.meta_data_only = resp->meta_data_only;

                if (resp->response_pending) {
                    WN_LOGI("PICS product-info partial: jobid=0x%llx +apps=%zu "
                            "+packages=%zu (pending more)",
                            static_cast<unsigned long long>(header.jobid_target),
                            resp->apps.size(), resp->packages.size());
                    break;
                }
                // Fire outside the lock to avoid re-entrancy hazards.
                final_cb = std::move(it->second.cb);
                merged   = std::move(it->second.acc);
                pics_pending_.erase(it);
            }
            WN_LOGI("PICS product-info final: apps=%zu packages=%zu unknown_apps=%zu "
                    "unknown_packages=%zu http_min_size=%u",
                    merged.apps.size(), merged.packages.size(),
                    merged.unknown_appids.size(), merged.unknown_packageids.size(),
                    merged.http_min_size);
            if (final_cb) final_cb(std::move(merged));
            break;
        }

        case EMsg::ClientLogonResponse: {
            auto resp = pb::CMsgClientLogonResponse::deserialize(body);
            if (!resp) {
                WN_LOGE("CMsgClientLogonResponse parse failed");
                return;
            }
            if (resp->eresult == 1 /* EResult.OK */) {
                steam_id_.store(resp->client_supplied_steamid);
                family_group_id_.store(resp->family_group_id);
                // session_id is set in the response header, not the body.
                session_id_.store(header.client_sessionid);
                set_state_locked_(ClientState::LoggedOn);
                if (resp->heartbeat_seconds > 0) {
                    heartbeat_.start(
                        std::chrono::seconds(resp->heartbeat_seconds),
                        [this]() {
                            pb::CMsgClientHeartBeat hb;
                            send_proto_message(EMsg::ClientHeartBeat, hb.serialize());
                        });
                }
                if (resp->rtime32_server_time != 0) {
                    wn_cm_bridge_dispatch_server_realtime(resp->rtime32_server_time);
                }
            }
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }

        case EMsg::ClientLoggedOff:
        case EMsg::ClientServerUnavailable: {
            // Keep server EResult visible for mid-session logoff diagnosis.
            if (auto off = pb::CMsgClientLoggedOff::deserialize(body)) {
                WN_LOGE("ClientLoggedOff: emsg=%d eresult=%d — session ended",
                        static_cast<int>(emsg), off->eresult);
            } else {
                WN_LOGE("ClientLoggedOff: emsg=%d (eresult parse failed, "
                        "%zu bytes) — session ended",
                        static_cast<int>(emsg), body.size());
            }
            heartbeat_.stop();
            steam_id_.store(0);
            session_id_.store(0);
            family_group_id_.store(0);
            // Drop the LoggedOn flag while the channel is still up so the
            // bridge fires dispatch_logon_state(false) and the
            // libsteamclient mirror flips pushed.logged_on=false. Stays at
            // Connected (post-handshake, pre-logon) so the SDK consumer
            // can drive a re-logon if it wants to.
            if (state_.load() == ClientState::LoggedOn) {
                set_state_locked_(ClientState::Connected);
            }
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }

        case EMsg::ClientPersonaState: {
            // Server-pushed persona updates; cache the entry for our own
            // SteamID so self_persona() can surface name/avatar/game,
            // AND cache friend personas separately so the ISteamFriends
            // queries (via libsteamclient.so) can return real names.
            auto resp = pb::CMsgClientPersonaState::deserialize(body);
            if (resp) {
                const uint64_t self = steam_id_.load();
                size_t friend_updates = 0;
                for (auto& f : resp->friends) {
                    if (f.friendid == 0) continue;
                    bool is_self = (f.friendid == self);
                    if (is_self) {
                        WN_LOGI("persona state: self name='%s' app=%u",
                                f.player_name.c_str(), f.game_played_app_id);
                        std::lock_guard<std::mutex> lk(persona_mu_);
                        self_persona_ = f;  // copy: cached for self_persona() reader
                        // Fall through to the bridge dispatch so libsteamclient.so
                        // mirrors self persona into its pushed.persona_name /
                        // persona_state too. The observer differentiates self
                        // from friend by comparing pushed().steam_id, so the
                        // same dispatch wires both paths.
                    } else {
                        // Cache only entries that carry an actual name —
                        // CMsgClientPersonaState can be sliced by
                        // EClientPersonaStateFlag and arrive with just the
                        // state, game, or avatar alone. Don't overwrite a
                        // previously-cached name with an empty string.
                        std::lock_guard<std::mutex> lk(friend_personas_mu_);
                        auto& slot = friend_personas_[f.friendid];
                        slot.sid = f.friendid;
                        if (!f.player_name.empty()) slot.player_name = f.player_name;
                        slot.persona_state      = f.persona_state;
                        slot.game_played_app_id = f.game_played_app_id;
                        // Avatar hash arrives only when EClientPersonaStateFlag_Avatar
                        // is set on the request flags; an empty bytes vec means
                        // "this push didn't carry one" — preserve the previously-
                        // cached hash so a stats-only push doesn't wipe the
                        // avatar.
                        if (!f.avatar_hash.empty()) slot.avatar_hash = f.avatar_hash;
                    }
                    // Reactive bridge dispatch — libsteamclient.so registers
                    // an observer that mirrors this update into its pushed_
                    // state + emits PersonaStateChange_t with zero Kotlin
                    // round-trip. Observer fires AFTER the cache update so a
                    // re-entrant friend_personas() read sees consistent data.
                    {
                        WnCmPersonaEvent ev{};
                        ev.sid              = f.friendid;
                        ev.persona_state    = f.persona_state;
                        ev.game_played_app  = f.game_played_app_id;
                        ev.name             = f.player_name.empty()
                                                ? nullptr : f.player_name.c_str();
                        ev.avatar_hash      = f.avatar_hash.empty()
                                                ? nullptr : f.avatar_hash.data();
                        ev.avatar_hash_len  = f.avatar_hash.size();
                        // Build pointer-pair array for the RP map. Stack-
                        // allocated; lives for the observer call duration.
                        std::vector<WnCmRichPresenceKV> kv;
                        kv.reserve(f.rich_presence.size());
                        for (const auto& [k, v] : f.rich_presence) {
                            kv.push_back({k.c_str(), v.c_str()});
                        }
                        ev.rp_pairs = kv.empty() ? nullptr : kv.data();
                        ev.rp_count = kv.size();
                        wn_cm_bridge_dispatch_persona(&ev);
                    }
                    ++friend_updates;
                }
                if (friend_updates > 0) {
                    WN_LOGI("persona state: cached %zu friend persona(s)",
                            friend_updates);
                }
            }
            break;
        }

        case EMsg::ClientLicenseList: {
            auto msg = pb::CMsgClientLicenseList::deserialize(body);
            std::vector<WnCmLicenseEntry> bridge_entries;
            if (!msg) {
                WN_LOGE("CMsgClientLicenseList parse failed (%zu bytes)", body.size());
            } else {
                WN_LOGI("ClientLicenseList: eresult=%d licenses=%zu",
                        msg->eresult, msg->licenses.size());
                {
                    std::lock_guard<std::mutex> lk(license_mu_);
                    license_list_ = msg->licenses;
                }
                // Build the bridge-shaped POD array for the observer.
                // Field-for-field projection; observer reads each entry's
                // package_id + owner_id for family-share resolution.
                bridge_entries.reserve(msg->licenses.size());
                for (const auto& lic : msg->licenses) {
                    bridge_entries.push_back({
                        lic.package_id,
                        lic.owner_id,
                        lic.time_created,
                        lic.license_type,
                        lic.flags,
                        lic.change_number,
                        lic.minute_limit,
                        lic.minutes_used,
                    });
                }
                library_.ingest_license_list(*msg);
                if (auto_populate_library_.load()) {
                    library_populate_step_();
                } else {
                    WN_LOGI("library populate: skipped (auto-populate disabled "
                            "for this session)");
                }
            }
            // Reactive bridge dispatch — fires after cache + library
            // ingest so re-entrant readers see consistent state. No-op
            // when no observer is registered.
            wn_cm_bridge_dispatch_license_list(
                bridge_entries.empty() ? nullptr : bridge_entries.data(),
                bridge_entries.size());
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }

        case EMsg::ClientPlayingSessionState: {
            auto msg = pb::CMsgClientPlayingSessionState::deserialize(body);
            if (msg) {
                playing_blocked_.store(msg->playing_blocked);
                WN_LOGI("playing session state: blocked=%d app=%u",
                        msg->playing_blocked ? 1 : 0, msg->playing_app);
            } else {
                WN_LOGE("CMsgClientPlayingSessionState parse failed (%zu bytes)",
                        body.size());
            }
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }

        case EMsg::ClientFriendsList: {
            // Server-pushed at logon (full snapshot, bincremental=false)
            // and on every relationship change (bincremental=true) — we
            // merge incrementals into the cached map. Powers
            // ISteamFriends queries via WnLibSteamClient.setFriendsList.
            auto msg = pb::CMsgClientFriendsList::deserialize(body);
            std::vector<uint64_t> mutual_sids;
            if (msg) {
                std::lock_guard<std::mutex> lk(friends_mu_);
                if (!msg->bincremental) friends_.clear();
                size_t mutual = 0;
                for (const auto& e : msg->friends) {
                    if (e.efriendrelationship == 0) {
                        // None = removed; drop the entry.
                        friends_.erase(e.ulfriendid);
                    } else {
                        friends_[e.ulfriendid] = e.efriendrelationship;
                    }
                    if (e.efriendrelationship == 3) ++mutual;
                }
                // Collect ALL current mutual friends for the observer —
                // matches the WnLibSteamClient.setFriendsList contract
                // of full-set replacement on each push. Incremental
                // pushes still produce a complete current snapshot here
                // because we merged into friends_ first.
                mutual_sids.reserve(friends_.size());
                for (const auto& [sid, rel] : friends_) {
                    if (rel == 3) mutual_sids.push_back(sid);
                }
                WN_LOGI("ClientFriendsList: incremental=%d entries=%zu "
                        "(mutual=%zu in payload) total_now=%zu",
                        msg->bincremental ? 1 : 0,
                        msg->friends.size(), mutual, friends_.size());
            } else {
                WN_LOGE("CMsgClientFriendsList parse failed (%zu bytes)",
                        body.size());
            }
            // Reactive bridge dispatch — libsteamclient.so observer mirrors
            // into pushed.friends. Friends mutex already released above; the
            // pointer-into-vector lives for this scope only, observer must
            // copy if it retains (the registered handler does — std::vector
            // assignment into pushed.friends).
            wn_cm_bridge_dispatch_friends_list(
                mutual_sids.empty() ? nullptr : mutual_sids.data(),
                mutual_sids.size());
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }

        case EMsg::ClientAccountInfo: {
            // CMsgClientAccountInfo fields we surface:
            //   field 1  persona_name             (string)
            //   field 2  ip_country               (string, ISO 3166-1)
            //   field 15 two_factor_state         (varint enum; 0 = none)
            //   field 17 is_phone_verified        (varint bool)
            //   field 19 is_phone_identifying     (varint bool)
            //   field 20 is_phone_need_verification (varint bool)
            // Other fields (count_authed_computers, locked, tutorial …)
            // are skipped.
            WnCmAccountInfo info{};
            std::string persona_name;
            std::string ip_country;
            wn_steam::proto::Reader r{std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(body.data()), body.size())};
            while (auto tag = r.next_tag()) {
                switch (tag->field_number) {
                    case 1: {  // persona_name
                        auto v = r.string();
                        if (v) persona_name.assign(v->data(), v->size());
                        else r.skip(tag->wire_type);
                        break;
                    }
                    case 2: {  // ip_country
                        auto v = r.string();
                        if (v) ip_country.assign(v->data(), v->size());
                        else r.skip(tag->wire_type);
                        break;
                    }
                    case 15: {  // two_factor_state — enum (0=none, 1=mobile-auth)
                        auto v = r.u32();
                        if (v) info.two_factor_enabled = (*v != 0);
                        else r.skip(tag->wire_type);
                        break;
                    }
                    case 17: {
                        auto v = r.boolean();
                        if (v) info.phone_verified = *v;
                        else r.skip(tag->wire_type);
                        break;
                    }
                    case 19: {
                        auto v = r.boolean();
                        if (v) info.phone_identifying = *v;
                        else r.skip(tag->wire_type);
                        break;
                    }
                    case 20: {
                        auto v = r.boolean();
                        if (v) info.phone_requires_verification = *v;
                        else r.skip(tag->wire_type);
                        break;
                    }
                    default:
                        if (!r.skip(tag->wire_type)) {
                            WN_LOGE("ClientAccountInfo: skip failed at field %d (wt=%d)",
                                    tag->field_number, static_cast<int>(tag->wire_type));
                            goto account_info_done;
                        }
                        break;
                }
            }
            account_info_done:
            if (!persona_name.empty()) {
                info.persona_name     = persona_name.c_str();
                info.persona_name_len = persona_name.size();
            }
            if (!ip_country.empty()) {
                info.ip_country     = ip_country.c_str();
                info.ip_country_len = ip_country.size();
            }
            WN_LOGI("ClientAccountInfo: persona='%.*s' ip='%.*s' 2FA=%d phone_v=%d phone_id=%d phone_nv=%d",
                    static_cast<int>(persona_name.size()), persona_name.c_str(),
                    static_cast<int>(ip_country.size()),   ip_country.c_str(),
                    info.two_factor_enabled, info.phone_verified,
                    info.phone_identifying, info.phone_requires_verification);
            wn_cm_bridge_dispatch_account_info(&info);
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }
        case EMsg::ClientEmailAddrInfo: {
            // Email-address info is a separate message; we don't track
            // email status today. Forward to the upstream observer for
            // visibility (and future wires).
            WN_LOGI("ClientEmailAddrInfo (%u bytes) — forwarded, not parsed",
                    static_cast<unsigned>(body.size()));
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }

        default: {
            ClientMessageCallback cb;
            { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_client_message_; }
            safe_invoke(cb, emsg, header, body);
            break;
        }
    }
}

void CMClient::set_state_locked_(ClientState s) {
    ClientState prev = state_.load();
    state_.store(s);
    StateCallback cb;
    { std::lock_guard<std::mutex> lk(cb_mu_); cb = on_state_; }
    safe_invoke(cb, s);
    // Reactive bridge — libsteamclient.so observes this directly without
    // a Kotlin poll hop. Map ClientState → bool logged_on:
    //   Disconnected / Connecting / Connected = not logged on
    //   LoggedOn                                = logged on
    //
    // CRITICAL: only dispatch when the LoggedOn-vs-not edge actually
    // CHANGES. Without this guard, a normal CM cycle
    //   Disconnected → Connecting → Connected → LoggedOn
    // dispatches three (logon_state=false) events before the final
    // (true) — undoing the warm "logged_on=true" that seedFromPref
    // Manager set from cached credentials on cold-start. The Compose
    // UI gates Sign-In affordance on this flag, so the user perceives
    // "still asking me to sign in" while a perfectly valid reconnect
    // is in flight.
    bool was_logged_on = (prev == ClientState::LoggedOn);
    bool is_logged_on  = (s    == ClientState::LoggedOn);
    if (was_logged_on != is_logged_on) {
        wn_cm_bridge_dispatch_logon_state(is_logged_on);
    }
}

// Event-driven PICS crawl: packages, access tokens, then apps.
void CMClient::library_populate_step_() {
    auto pkg_batch = library_.get_pending_package_pics_request();
    if (!pkg_batch.empty()) {
        WN_LOGI("library populate: requesting PICS for %zu packages "
                "(pkgs_known=%zu apps_known=%zu)",
                pkg_batch.size(), library_.package_count(), library_.app_count());
        pics_get_product_info(std::move(pkg_batch), {}, /*meta_data_only=*/false,
            [this](std::optional<pb::CMsgClientPICSProductInfoResponse> resp) {
                if (!resp) {
                    WN_LOGE("library populate: package PICS failed");
                    return;
                }
                library_.ingest_package_pics_response(*resp);
                library_populate_step_();
            });
        return;
    }

    auto tok_batch = library_.get_apps_needing_access_token();
    if (!tok_batch.empty()) {
        WN_LOGI("library populate: requesting access tokens for %zu apps",
                tok_batch.size());
        pics_get_access_tokens({}, std::move(tok_batch),
            [this](std::optional<pb::CMsgClientPICSAccessTokenResponse> resp) {
                if (!resp) {
                    WN_LOGE("library populate: app access-token request failed");
                    return;
                }
                library_.ingest_app_access_tokens(*resp);
                library_populate_step_();
            });
        return;
    }

    auto app_batch = library_.get_pending_app_pics_request();
    if (!app_batch.empty()) {
        WN_LOGI("library populate: requesting PICS for %zu apps", app_batch.size());
        pics_get_product_info({}, std::move(app_batch), /*meta_data_only=*/false,
            [this](std::optional<pb::CMsgClientPICSProductInfoResponse> resp) {
                if (!resp) {
                    WN_LOGE("library populate: app PICS failed");
                    return;
                }
                library_.ingest_app_pics_response(*resp);
                library_populate_step_();
            });
        return;
    }

    size_t packages   = library_.package_count();
    size_t apps_total = library_.app_count();
    size_t apps_owned = library_.owned_app_count();
    WN_LOGI("library populate complete: %zu packages, %zu apps (%zu truly owned, "
            "%zu parent-stubs for owned-DLC)",
            packages, apps_total, apps_owned, apps_total - apps_owned);

    auto owned = library_.owned_apps();
    size_t games = 0, dlc = 0, demo = 0, tool = 0, other = 0;
    for (const auto& a : owned) {
        if      (a.type == "Game" || a.type == "game")          ++games;
        else if (a.type == "DLC"  || a.type == "dlc")           ++dlc;
        else if (a.type == "Demo" || a.type == "demo")          ++demo;
        else if (a.type == "Tool" || a.type == "tool")          ++tool;
        else                                                    ++other;
    }
    WN_LOGI("  owned breakdown: games=%zu dlc=%zu demo=%zu tool=%zu other=%zu",
            games, dlc, demo, tool, other);

    auto pkgs_snap = library_.packages();
    std::unordered_map<uint32_t, OwnedPackage> pkg_by_id;
    pkg_by_id.reserve(pkgs_snap.size());
    for (auto& p : pkgs_snap) pkg_by_id[p.package_id] = std::move(p);
    int shown = 0;
    for (const auto& a : owned) {
        if (a.type != "Game" && a.type != "game") continue;
        std::string pkg_summary;
        for (uint32_t pid : a.source_package_ids) {
            auto it = pkg_by_id.find(pid);
            if (it == pkg_by_id.end()) {
                pkg_summary += " " + std::to_string(pid);
            } else {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              " %u(t=%u f=0x%x)",
                              pid, it->second.license_type, it->second.license_flags);
                pkg_summary += buf;
            }
        }
        WN_LOGI("  [%u] '%s' dlc=%zu src_pkgs:%s",
                a.app_id, a.name.c_str(), a.dlc_app_ids.size(),
                pkg_summary.c_str());
        if (++shown >= 10) break;
    }
}

// ---------------------------------------------------------------------------
// ISteamMatchmaking lobby browser (Phase A) — single-shot CMsgClient
// MMSGetLobbyList → ClientMMSGetLobbyListResponse (EMsgs 6607/6608).
// Mirror of get_user_stats — same JobManager-tracked envelope-and-send
// pattern, same synthetic-failure handling on timeout / disconnect.
// ---------------------------------------------------------------------------
void CMClient::lobby_get_list(
        uint32_t app_id,
        std::vector<pb::CMsgClientMMSGetLobbyListFilter> filters,
        int32_t num_lobbies_requested,
        LobbyListCallback cb,
        std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_get_list: not logged on, app=%u", app_id);
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientMMSGetLobbyList req;
    req.app_id                = app_id;
    req.num_lobbies_requested = num_lobbies_requested > 0
                                    ? num_lobbies_requested
                                    : 50;
    req.filters               = std::move(filters);

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [app_id, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            WN_LOGI("lobby_get_list: synthetic failure for app %u", app_id);
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientMMSGetLobbyListResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("lobby_get_list: parse failed for app %u (%zu bytes)",
                    app_id, r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("lobby_get_list: app %u eresult=%d lobbies=%zu",
                app_id, resp->eresult, resp->lobbies.size());
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientMMSGetLobbyList, hdr,
                                       req.serialize());
    WN_LOGI("outbound lobby_get_list: app=%u jobid=0x%llx filters=%zu n=%d",
            app_id, static_cast<unsigned long long>(job_id),
            req.filters.size(), req.num_lobbies_requested);
    if (!channel_->send(wire)) {
        WN_LOGE("lobby_get_list: channel send failed for app %u", app_id);
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

void CMClient::set_lobby_data_observer(LobbyDataObserver obs) {
    lobby_data_observer_ = std::move(obs);
}

void CMClient::set_lobby_chat_msg_observer(LobbyChatMsgObserver obs) {
    lobby_chat_msg_observer_ = std::move(obs);
}

void CMClient::set_lobby_membership_observer(LobbyMembershipObserver obs) {
    lobby_membership_observer_ = std::move(obs);
}

void CMClient::lobby_send_chat(uint32_t app_id, uint64_t lobby_sid,
                                std::vector<uint8_t> chat_bytes) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_send_chat: not logged on, dropping");
        return;
    }
    pb::CMsgClientMMSSendLobbyChatMsg msg;
    msg.app_id         = app_id;
    msg.steam_id_lobby = lobby_sid;
    msg.lobby_message  = std::move(chat_bytes);
    if (send_proto_message(EMsg::ClientMMSSendLobbyChatMsg, msg.serialize(), app_id)) {
        WN_LOGI("lobby_send_chat: sent (lobby=0x%llx body=%zuB)",
                static_cast<unsigned long long>(lobby_sid), msg.lobby_message.size());
    } else {
        WN_LOGE("lobby_send_chat: send failed");
    }
}

// ---------------------------------------------------------------------------
// InviteToLobby — CMsgClientMMSInviteToLobby (6621). Fire-and-forget.
// Steam routes the invite to the target user's online client (overlay
// notification + Friends-list popup) or queues it as a Friends chat
// message if they're offline.
// ---------------------------------------------------------------------------
void CMClient::lobby_invite_user(uint32_t app_id, uint64_t lobby_sid,
                                  uint64_t invitee_sid) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_invite_user: not logged on, dropping");
        return;
    }
    pb::CMsgClientMMSInviteToLobby msg;
    msg.app_id                = app_id;
    msg.steam_id_lobby        = lobby_sid;
    msg.steam_id_user_invited = invitee_sid;
    if (send_proto_message(EMsg::ClientMMSInviteToLobby, msg.serialize(), app_id)) {
        WN_LOGI("lobby_invite_user: sent (lobby=0x%llx invitee=0x%llx)",
                static_cast<unsigned long long>(lobby_sid),
                static_cast<unsigned long long>(invitee_sid));
    } else {
        WN_LOGE("lobby_invite_user: send failed");
    }
}

// ---------------------------------------------------------------------------
// CreateLobby — CMsgClientMMSCreateLobby (6601) → response 6602
// ---------------------------------------------------------------------------
void CMClient::lobby_create(uint32_t app_id, int32_t lobby_type,
                            int32_t max_members, LobbyCreateCallback cb,
                            std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_create: not logged on, app=%u", app_id);
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientMMSCreateLobby req;
    req.app_id      = app_id;
    req.lobby_type  = lobby_type;
    req.max_members = max_members > 0 ? max_members : 4;

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [app_id, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            WN_LOGI("lobby_create: synthetic failure for app %u", app_id);
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientMMSCreateLobbyResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("lobby_create: parse failed (%zu bytes)", r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("lobby_create: app %u eresult=%d lobby=0x%llx",
                app_id, resp->eresult,
                static_cast<unsigned long long>(resp->steam_id_lobby));
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientMMSCreateLobby, hdr,
                                       req.serialize());
    WN_LOGI("outbound lobby_create: app=%u type=%d max=%d jobid=0x%llx",
            app_id, lobby_type, req.max_members,
            static_cast<unsigned long long>(job_id));
    if (!channel_->send(wire)) {
        WN_LOGE("lobby_create: channel send failed for app %u", app_id);
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

// ---------------------------------------------------------------------------
// JoinLobby — CMsgClientMMSJoinLobby (6603) → response 6604
// ---------------------------------------------------------------------------
void CMClient::lobby_join(uint32_t app_id, uint64_t lobby_sid,
                          LobbyJoinCallback cb,
                          std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_join: not logged on, lobby=0x%llx",
                static_cast<unsigned long long>(lobby_sid));
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientMMSJoinLobby req;
    req.app_id         = app_id;
    req.steam_id_lobby = lobby_sid;

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [lobby_sid, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            WN_LOGI("lobby_join: synthetic failure for lobby=0x%llx",
                    static_cast<unsigned long long>(lobby_sid));
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientMMSJoinLobbyResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("lobby_join: parse failed (%zu bytes)", r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("lobby_join: lobby=0x%llx chat_resp=%d members=%zu owner=0x%llx",
                static_cast<unsigned long long>(resp->steam_id_lobby),
                resp->chat_room_enter_response, resp->members.size(),
                static_cast<unsigned long long>(resp->steam_id_owner));
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientMMSJoinLobby, hdr,
                                       req.serialize());
    WN_LOGI("outbound lobby_join: app=%u lobby=0x%llx jobid=0x%llx",
            app_id, static_cast<unsigned long long>(lobby_sid),
            static_cast<unsigned long long>(job_id));
    if (!channel_->send(wire)) {
        WN_LOGE("lobby_join: channel send failed for lobby=0x%llx",
                static_cast<unsigned long long>(lobby_sid));
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

// ---------------------------------------------------------------------------
// LeaveLobby — CMsgClientMMSLeaveLobby (6605), fire-and-forget.
// Steam sends 6606 LeaveLobbyResponse but the SDK contract doesn't gate
// any callback on it — the game has already moved on by the time the
// ack arrives. We send through `send_proto_message` which doesn't track
// the response.
// ---------------------------------------------------------------------------
void CMClient::lobby_leave(uint32_t app_id, uint64_t lobby_sid) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_leave: not logged on, dropping");
        return;
    }
    pb::CMsgClientMMSLeaveLobby msg;
    msg.app_id         = app_id;
    msg.steam_id_lobby = lobby_sid;
    if (send_proto_message(EMsg::ClientMMSLeaveLobby, msg.serialize(), app_id)) {
        WN_LOGI("lobby_leave: sent (app=%u lobby=0x%llx)", app_id,
                static_cast<unsigned long long>(lobby_sid));
    } else {
        WN_LOGE("lobby_leave: send failed (app=%u lobby=0x%llx)", app_id,
                static_cast<unsigned long long>(lobby_sid));
    }
}

// ---------------------------------------------------------------------------
// SetLobbyData — CMsgClientMMSSetLobbyData (6609) → response 6610.
// Same recipe as lobby_create / lobby_join. Steam also fires a 6612
// LobbyData push to every member after a successful SetLobbyData; the
// existing LobbyData observer arm handles that — no extra wiring
// needed here.
// ---------------------------------------------------------------------------
void CMClient::lobby_set_data(uint32_t app_id, uint64_t lobby_sid,
                              uint64_t steam_id_member,
                              std::vector<uint8_t> metadata,
                              int32_t max_members, int32_t lobby_type,
                              int32_t lobby_flags,
                              LobbySetDataCallback cb,
                              std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_set_data: not logged on, lobby=0x%llx",
                static_cast<unsigned long long>(lobby_sid));
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientMMSSetLobbyData req;
    req.app_id          = app_id;
    req.steam_id_lobby  = lobby_sid;
    req.steam_id_member = steam_id_member;
    req.max_members     = max_members;
    req.lobby_type      = lobby_type;
    req.lobby_flags     = lobby_flags;
    req.metadata        = std::move(metadata);

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [lobby_sid, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            WN_LOGI("lobby_set_data: synthetic failure for lobby=0x%llx",
                    static_cast<unsigned long long>(lobby_sid));
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientMMSSetLobbyDataResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("lobby_set_data: parse failed (%zu bytes)", r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("lobby_set_data: lobby=0x%llx eresult=%d",
                static_cast<unsigned long long>(resp->steam_id_lobby),
                resp->eresult);
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientMMSSetLobbyData, hdr,
                                       req.serialize());
    WN_LOGI("outbound lobby_set_data: app=%u lobby=0x%llx member=0x%llx meta=%zuB",
            app_id, static_cast<unsigned long long>(lobby_sid),
            static_cast<unsigned long long>(steam_id_member),
            req.metadata.size());
    if (!channel_->send(wire)) {
        WN_LOGE("lobby_set_data: channel send failed");
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

// ---------------------------------------------------------------------------
// SetLobbyOwner — CMsgClientMMSSetLobbyOwner (6615) → response 6616.
// Host-only operation. On success Steam pushes a 6612 LobbyData with the
// updated owner_sid; the existing LobbyData observer mirrors that back
// into pushed().active_lobbies so GetLobbyOwner re-reads cleanly.
// ---------------------------------------------------------------------------
void CMClient::lobby_set_owner(uint32_t app_id, uint64_t lobby_sid,
                                uint64_t new_owner_sid,
                                LobbySetOwnerCallback cb,
                                std::chrono::seconds timeout) {
    if (state_.load() != ClientState::LoggedOn) {
        WN_LOGI("lobby_set_owner: not logged on, lobby=0x%llx",
                static_cast<unsigned long long>(lobby_sid));
        if (cb) cb(std::nullopt);
        return;
    }
    pb::CMsgClientMMSSetLobbyOwner req;
    req.app_id             = app_id;
    req.steam_id_lobby     = lobby_sid;
    req.steam_id_new_owner = new_owner_sid;

    const uint64_t job_id = jobs_.next_job_id();
    jobs_.track(job_id, [lobby_sid, cb = std::move(cb)](JobResult r) {
        if (r.synthetic_failure) {
            WN_LOGI("lobby_set_owner: synthetic failure for lobby=0x%llx",
                    static_cast<unsigned long long>(lobby_sid));
            if (cb) cb(std::nullopt);
            return;
        }
        auto resp = pb::CMsgClientMMSSetLobbyOwnerResponse::deserialize(r.body);
        if (!resp) {
            WN_LOGE("lobby_set_owner: parse failed (%zu bytes)", r.body.size());
            if (cb) cb(std::nullopt);
            return;
        }
        WN_LOGI("lobby_set_owner: lobby=0x%llx eresult=%d",
                static_cast<unsigned long long>(resp->steam_id_lobby),
                resp->eresult);
        if (cb) cb(std::move(resp));
    }, timeout);

    CMsgProtoBufHeader hdr;
    hdr.steamid          = steam_id_.load();
    hdr.client_sessionid = session_id_.load();
    hdr.jobid_source     = job_id;
    hdr.jobid_target     = kInvalidJobId;
    auto wire = encode_proto_envelope(EMsg::ClientMMSSetLobbyOwner, hdr,
                                       req.serialize());
    WN_LOGI("outbound lobby_set_owner: app=%u lobby=0x%llx new_owner=0x%llx",
            app_id, static_cast<unsigned long long>(lobby_sid),
            static_cast<unsigned long long>(new_owner_sid));
    if (!channel_->send(wire)) {
        WN_LOGE("lobby_set_owner: channel send failed");
        jobs_.deliver(job_id, -1, "channel send failed", {});
    }
}

}  // namespace wn_steam
