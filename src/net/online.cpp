#include "bm64_online.h"
#include "bm64_coop.h"
#include "bm64_ice.h"
#include "bm64_signalling.h"

#include "bm64_debug.h"
#include "bm64_rollback.h"
#include "bm64_session.h"
#include "bm64_state.h"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif


namespace bm64::online {
namespace {

constexpr uint32_t kProtocol = 4;
// bm64::state keeps 16 snapshots, and a rollback needs one for every tick it can rewind.
constexpr int kMaxRollback = 12;
// A UDP datagram cannot carry the 128 KB save image. Handshake chunks stay under a typical MTU.
constexpr uint32_t kSaveChunk = 1000;

struct Held {
    std::chrono::steady_clock::time_point due;
    std::vector<uint8_t> bytes;
};

class UdpLink : public bm64::rollback::Transport {
public:
    SOCKET sock = INVALID_SOCKET;
    std::vector<sockaddr_in> peers;
    int lag_ms = 0;
    int jitter_ms = 0;
    int loss = 0;
    uint32_t rng = 0xA5A5A5A5u;
    std::vector<Held> held;

    ~UdpLink() override {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
        }
    }

    void send(const uint8_t* data, size_t size) override {
        for (const sockaddr_in& dest : peers) {
            rng = rng * 1664525u + 1013904223u;
            if (loss > 0 && static_cast<int>(rng % 100) < loss) {
                continue;
            }
            int sent = sendto(sock, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            if (sent < 0) {
                static int logged = 0;
                if (logged < 3) {
                    logged++;
                    std::fprintf(stderr, "[bm64 net] sendto failed for %d bytes (WSA %d)\n", static_cast<int>(size), WSAGetLastError());
                }
            }
        }
    }

    int recv(uint8_t* data, size_t cap) override {
        // One poll used to take a single datagram. A tick retransmits the whole input window, so the
        // socket filled and localhost dropped hashes.
        uint8_t buf[512];
        for (;;) {
            sockaddr_in from{};
            int from_len = sizeof(from);
            int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0) {
                // A datagram too big for a session packet (a handshake save chunk the host is still
                // resending) or a stale port-unreachable report: skip it and keep reading. Stopping
                // there left the rest of the queue for later polls.
                int err = n < 0 ? WSAGetLastError() : 0;
                if (err == WSAEMSGSIZE || err == WSAECONNRESET) {
                    continue;
                }
                break;
            }
            Held item;
            int extra = 0;
            if (jitter_ms > 0) {
                rng = rng * 1664525u + 1013904223u;
                extra = static_cast<int>(rng % static_cast<uint32_t>(jitter_ms + 1));
            }
            item.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(lag_ms + extra);
            item.bytes.assign(buf, buf + n);
            held.push_back(std::move(item));
        }
        auto now = std::chrono::steady_clock::now();
        for (auto it = held.begin(); it != held.end(); ++it) {
            if (it->due <= now) {
                size_t copy = it->bytes.size();
                if (copy > cap) {
                    copy = cap;
                }
                std::memcpy(data, it->bytes.data(), copy);
                held.erase(it);
                return static_cast<int>(copy);
            }
        }
        return 0;
    }
};

struct Runtime {
    bool on = false;
    bool spec = false;
    bool accept = false;
    bool confirmed = false;
    uint64_t step_tick = 0;
    bm64::rollback::FrameInput input{};
    std::unique_ptr<UdpLink> link;
    std::unique_ptr<bm64::rollback::Session> session;
    int player = 0;
    int players = 1;
    bool leave_logged[bm64::rollback::kMaxPlayers]{};
    bool heard_logged[bm64::rollback::kMaxPlayers]{};
    bool first_frame_logged = false;
    std::chrono::steady_clock::time_point handshake_done{};
    bool launch_set = false;
    bool launch_host = true;
};

Runtime& rt() {
    static Runtime r;
    return r;
}

bool winsock_up() {
    static bool up = false;
    if (!up) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
        up = true;
    }
    return true;
}

bool parse_endpoint(const char* text, sockaddr_in& out) {
    std::string s = text ? text : "";
    auto colon = s.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    std::string host = s.substr(0, colon);
    int port = std::atoi(s.substr(colon + 1).c_str());
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) != 1) {
        return false;
    }
    return port > 0;
}

bool parse_peer_list(const char* text, std::vector<sockaddr_in>& out) {
    std::string s = text ? text : "";
    size_t start = 0;
    while (start < s.size()) {
        size_t comma = s.find(',', start);
        std::string item = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        sockaddr_in addr{};
        if (!item.empty()) {
            if (!parse_endpoint(item.c_str(), addr)) {
                return false;
            }
            out.push_back(addr);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return !out.empty();
}

uint64_t rom_hash() {
    const char* path = std::getenv("BM64_ROM");
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    std::ifstream in{ path, std::ios::binary };
    if (!in) {
        return 0;
    }
    std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    return XXH3_64bits(bytes.data(), bytes.size());
}

void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 24));
}

void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    put_u32(o, static_cast<uint32_t>(v));
    put_u32(o, static_cast<uint32_t>(v >> 32));
}

uint32_t get_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

uint64_t get_u64(const uint8_t* p) {
    return get_u32(p) | (static_cast<uint64_t>(get_u32(p + 4)) << 32);
}

bool wait_packet(UdpLink& link, std::vector<uint8_t>& out, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t buf[256 * 1024];
        int n = recvfrom(link.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
        if (n > 0) {
            out.assign(buf, buf + n);
            return true;
        }
        // Only sleep when the queue is empty. A port-unreachable report (the peer's port was not
        // open yet) is an error per datagram, and sleeping on each one delayed the handshake by
        // seconds after the guest had already started its session.
        if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

bool active() {
    return rt().on;
}

int player_count() {
    return rt().players;
}

int local_player() {
    return rt().player;
}

bool speculative() {
    return rt().spec;
}

bool accepting_sample() {
    return rt().accept;
}

std::atomic<int> launch_players_{2};
std::atomic<int> launch_delay_{2};
std::atomic<int> launch_unlimited_{0};

void prepare_loopback(bool host_side) {
    rt().launch_set = true;
    rt().launch_host = host_side;
}

void set_launch_options(int players, int delay, bool unlimited_lives) {
    if (players < 2) {
        players = 2;
    }
    if (players > 4) {
        players = 4;
    }
    if (delay < 0) {
        delay = 0;
    }
    if (delay > 8) {
        delay = 8;
    }
    launch_players_.store(players);
    launch_delay_.store(delay);
    launch_unlimited_.store(unlimited_lives ? 1 : 0);
}

int launch_players() {
    return launch_players_.load();
}

int launch_delay() {
    return launch_delay_.load();
}

bool launch_unlimited_lives() {
    return launch_unlimited_.load() != 0;
}

bool want_unlimited_lives() {
    if (const char* v = std::getenv("BM64_NET_LIVES")) {
        return v[0] != '\0' && std::strcmp(v, "0") != 0;
    }
    return launch_unlimited_.load() != 0;
}

void handshake() {
    std::string mode_storage;
    const char* mode = std::getenv("BM64_NET");
    if ((mode == nullptr || mode[0] == '\0') && rt().launch_set) {
        mode_storage = rt().launch_host ? "host" : "guest";
        mode = mode_storage.c_str();
    }
    if (mode == nullptr || mode[0] == '\0') {
        return;
    }
    if (std::getenv("BM64_REPLAY") != nullptr) {
        std::printf("[bm64 net] BM64_REPLAY is set, so BM64_NET is ignored\n");
        return;
    }
    bool host = std::strcmp(mode, "host") == 0;
    bool guest = std::strcmp(mode, "guest") == 0;
    if (!host && !guest) {
        std::fprintf(stderr, "[bm64 net] BM64_NET must be host or guest\n");
        std::exit(1);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!winsock_up()) {
        std::fprintf(stderr, "[bm64 net] Winsock failed to start\n");
        std::exit(1);
    }

    const char* bind_text = std::getenv("BM64_NET_BIND");
    const char* peer_text = std::getenv("BM64_NET_PEER");
    std::string bind_default = host ? "127.0.0.1:27015" : "127.0.0.1:27016";
    std::string peer_default = host ? "127.0.0.1:27016" : "127.0.0.1:27015";
    sockaddr_in bind_addr{};
    std::vector<sockaddr_in> peer_addrs;
    if (!parse_endpoint(bind_text ? bind_text : bind_default.c_str(), bind_addr) ||
        !parse_peer_list(peer_text ? peer_text : peer_default.c_str(), peer_addrs)) {
        std::fprintf(stderr, "[bm64 net] Could not parse BM64_NET_BIND / BM64_NET_PEER\n");
        std::exit(1);
    }

    auto link = std::make_unique<UdpLink>();
    link->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (link->sock == INVALID_SOCKET || bind(link->sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        std::fprintf(stderr, "[bm64 net] Could not bind %s\n", bind_text ? bind_text : bind_default.c_str());
        std::exit(1);
    }
    u_long nonblock = 1;
    ioctlsocket(link->sock, FIONBIO, &nonblock);
    // Windows reports an ICMP port-unreachable for an earlier send as WSAECONNRESET on a later
    // recvfrom. The host sends its hello and save chunks before the guest has bound its port, and
    // draining those errors kept the host in the handshake for about 9 s after the guest had
    // finished (loopback, 2026-11-25). Turn the reports off.
    BOOL report_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(link->sock, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), nullptr, 0, &returned, nullptr, nullptr);
    int rcvbuf = 256 * 1024;
    setsockopt(link->sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    link->peers = std::move(peer_addrs);
    const char* bound = (bind_text != nullptr && bind_text[0] != '\0') ? bind_text : bind_default.c_str();
    const char* candidate = bm64::ice::host_candidate(bound);
    bm64::signalling::publish_candidate(candidate);
    std::printf("[bm64 net] ICE candidate %s\n", candidate);
    if (const char* lag = std::getenv("BM64_NET_LAG_MS")) {
        link->lag_ms = std::atoi(lag);
    }
    if (const char* jitter = std::getenv("BM64_NET_JITTER_MS")) {
        link->jitter_ms = std::atoi(jitter);
    }
    if (const char* loss = std::getenv("BM64_NET_LOSS")) {
        link->loss = std::atoi(loss);
    }

    int players = launch_players_.load();
    if (const char* p = std::getenv("BM64_NET_PLAYERS")) {
        players = std::atoi(p);
    }
    int player = host ? 0 : 1;
    if (const char* p = std::getenv("BM64_NET_PLAYER")) {
        player = std::atoi(p);
    }
    uint64_t mine_hash = rom_hash();
    bm64::debug::set_cheats_disabled(true);

    if (host) {
        if (!bm64::session::sandbox_active()) {
            bm64::session::begin_sandbox(nullptr, 0);
        }
        uint32_t seed = bm64::session::resolve_seed();
        std::vector<uint8_t> save(ultramodern::save_image_size());
        if (!save.empty()) {
            ultramodern::export_save_image(save.data(), save.size());
        }
        std::vector<uint8_t> hello;
        hello.insert(hello.end(), { 'B', 'M', '6', '4', 'H', 'E', 'L', '\0' });
        put_u32(hello, kProtocol);
        put_u32(hello, seed);
        put_u32(hello, static_cast<uint32_t>(players));
        put_u32(hello, static_cast<uint32_t>(save.size()));
        put_u64(hello, mine_hash);
        bool unlimited = want_unlimited_lives();
        put_u32(hello, unlimited ? 1u : 0u);
        uint32_t chunks = save.empty() ? 0u : static_cast<uint32_t>((save.size() + kSaveChunk - 1) / kSaveChunk);
        std::vector<char> saw_ack(bm64::rollback::kMaxPlayers, 0);
        int ack_count = 0;
        int ack_need = static_cast<int>(link->peers.size());
        for (int attempt = 0; attempt < 300 && ack_count < ack_need; attempt++) {
            link->send(hello.data(), hello.size());
            for (uint32_t index = 0; index < chunks; index++) {
                uint32_t offset = index * kSaveChunk;
                uint32_t length = std::min(kSaveChunk, static_cast<uint32_t>(save.size() - offset));
                std::vector<uint8_t> chunk;
                chunk.insert(chunk.end(), { 'B', 'M', '6', '4', 'C', 'H', 'K', '\0' });
                put_u32(chunk, index);
                put_u32(chunk, offset);
                put_u32(chunk, length);
                chunk.insert(chunk.end(), save.begin() + offset, save.begin() + offset + length);
                link->send(chunk.data(), chunk.size());
            }
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
            while (std::chrono::steady_clock::now() < deadline && ack_count < ack_need) {
                std::vector<uint8_t> packet;
                if (!wait_packet(*link, packet, 30)) {
                    continue;
                }
                if (packet.size() >= 24 && std::memcmp(packet.data(), "BM64ACK", 7) == 0) {
                    uint32_t proto = get_u32(packet.data() + 8);
                    uint64_t theirs = get_u64(packet.data() + 16);
                    if (proto != kProtocol || (mine_hash != 0 && theirs != 0 && mine_hash != theirs)) {
                        std::fprintf(stderr, "[bm64 net] Peer protocol or ROM hash does not match\n");
                        std::exit(1);
                    }
                    uint32_t who = get_u32(packet.data() + 12);
                    if (who < saw_ack.size() && saw_ack[who] == 0) {
                        saw_ack[who] = 1;
                        ack_count++;
                    }
                }
            }
        }
        if (ack_count < ack_need) {
            std::fprintf(stderr, "[bm64 net] Timed out waiting for the guest\n");
            std::exit(1);
        }
        bm64::coop::set_session(players, player, unlimited);
        std::printf("[bm64 net] Host ready, seed %08X, %zu byte save in %u chunks, lives %s\n",
            seed, save.size(), chunks, unlimited ? "unlimited" : "shared");
        std::printf("[bm64 net] %s. %s.\n", bm64::ice::describe(), bm64::signalling::describe());
        std::fflush(stdout);
    }
    else {
        std::vector<uint8_t> save;
        std::vector<char> have;
        uint32_t proto = 0;
        uint32_t seed = 0;
        uint32_t save_size = 0;
        uint64_t theirs = 0;
        uint32_t flags = 0;
        int got_chunks = 0;
        bool have_meta = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<uint8_t> packet;
            if (!wait_packet(*link, packet, 50)) {
                continue;
            }
            if (packet.size() >= 36 && std::memcmp(packet.data(), "BM64HEL", 7) == 0) {
                uint32_t new_size = get_u32(packet.data() + 20);
                if (!have_meta || new_size != save_size) {
                    proto = get_u32(packet.data() + 8);
                    seed = get_u32(packet.data() + 12);
                    uint32_t announced = get_u32(packet.data() + 16);
                    if (announced >= 1 && announced <= 4) {
                        players = static_cast<int>(announced);
                    }
                    save_size = new_size;
                    theirs = get_u64(packet.data() + 24);
                    flags = get_u32(packet.data() + 32);
                    save.assign(save_size, 0);
                    have.assign(save_size == 0 ? 0 : (save_size + kSaveChunk - 1) / kSaveChunk, 0);
                    got_chunks = 0;
                    have_meta = true;
                }
            }
            else if (have_meta && packet.size() >= 20 && std::memcmp(packet.data(), "BM64CHK", 7) == 0) {
                uint32_t index = get_u32(packet.data() + 8);
                uint32_t offset = get_u32(packet.data() + 12);
                uint32_t length = get_u32(packet.data() + 16);
                if (index < have.size() && have[index] == 0 && offset + length <= save.size() && packet.size() >= 20u + length) {
                    std::memcpy(save.data() + offset, packet.data() + 20, length);
                    have[index] = 1;
                    got_chunks++;
                }
            }
            if (have_meta && got_chunks == static_cast<int>(have.size())) {
                break;
            }
        }
        if (!have_meta || got_chunks != static_cast<int>(have.size())) {
            std::fprintf(stderr, "[bm64 net] Timed out waiting for the host\n");
            std::exit(1);
        }
        if (proto != kProtocol) {
            std::fprintf(stderr, "[bm64 net] Bad hello from the host\n");
            std::exit(1);
        }
        if (mine_hash != 0 && theirs != 0 && mine_hash != theirs) {
            std::fprintf(stderr, "[bm64 net] ROM hash does not match the host\n");
            std::exit(1);
        }
        bm64::session::set_seed(seed);
        if (!bm64::session::begin_sandbox(save.data(), save.size())) {
            std::fprintf(stderr, "[bm64 net] Could not install the host save\n");
            std::exit(1);
        }
        std::vector<uint8_t> ack;
        ack.insert(ack.end(), { 'B', 'M', '6', '4', 'A', 'C', 'K', '\0' });
        put_u32(ack, kProtocol);
        put_u32(ack, static_cast<uint32_t>(player));
        put_u64(ack, mine_hash);
        for (int i = 0; i < 5; i++) {
            link->send(ack.data(), ack.size());
        }
        bm64::coop::set_session(players, player, (flags & 1u) != 0);
        std::printf("[bm64 net] Guest ready, seed %08X, player %d, %u byte save, lives %s\n",
            seed, player, save_size, (flags & 1u) ? "unlimited" : "shared");
        std::printf("[bm64 net] %s. %s.\n", bm64::ice::describe(), bm64::signalling::describe());
        std::fflush(stdout);
    }

    bm64::rollback::Config cfg;
    cfg.local_player = player;
    cfg.player_count = players;
    cfg.input_delay = launch_delay_.load();
    cfg.max_rollback = 8;
    if (const char* d = std::getenv("BM64_NET_DELAY")) {
        cfg.input_delay = std::atoi(d);
    }
    if (const char* r = std::getenv("BM64_NET_ROLLBACK")) {
        cfg.max_rollback = std::clamp(std::atoi(r), 1, kMaxRollback);
    }
    rt().session = std::make_unique<bm64::rollback::Session>(cfg, *link);
    rt().link = std::move(link);
    rt().player = player;
    rt().players = players;
    rt().handshake_done = std::chrono::steady_clock::now();
    rt().on = true;
}

int begin_frame(uint8_t* rdram) {
    Runtime& r = rt();
    if (!r.session) {
        return 1;
    }
    auto ms_since_handshake = [&]() {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - r.handshake_done).count());
    };
    if (!r.first_frame_logged) {
        r.first_frame_logged = true;
        std::printf("[bm64 net] First frame %lld ms after the handshake\n", ms_since_handshake());
    }
    // Longer than noticing a silent player and the host deciding their leave. Before every peer
    // has been heard, longer than the startup allowance, since the other game may still be booting.
    auto give_up = std::chrono::steady_clock::now() +
        std::chrono::seconds(r.session->all_peers_seen() ? 20 : 40);
    while (std::chrono::steady_clock::now() < give_up) {
        for (int p = 0; p < r.players; p++) {
            if (p != r.player && !r.heard_logged[p] && r.session->peer_seen(p)) {
                r.heard_logged[p] = true;
                std::printf("[bm64 net] First packet from player %d, %lld ms after the handshake\n", p, ms_since_handshake());
            }
        }
        if (r.session->ended()) {
            std::fprintf(stderr, "[bm64 net] %s\n", r.session->end_reason());
            std::exit(1);
        }
        bm64::rollback::Step step;
        bool stall = false;
        bool ok = r.session->poll(step, stall, [&](uint64_t tick) {
            if (!bm64::state::load_tick(tick, rdram)) {
                std::fprintf(stderr, "[bm64 net] Rollback load failed at tick %llu\n", static_cast<unsigned long long>(tick));
                std::exit(1);
            }
        });
        if (!ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        r.input = step.input;
        r.spec = step.speculative;
        r.accept = step.accept_sample;
        r.confirmed = step.confirmed;
        r.step_tick = step.tick;
        if (step.loaded) {
            std::printf("[bm64 net] Rollback to tick %llu (depth %d, %d total)\n",
                static_cast<unsigned long long>(step.tick), r.session->max_depth(), r.session->rollbacks());
        }
        for (int p = 0; p < r.players; p++) {
            if (r.session->left(p) && !r.leave_logged[p]) {
                r.leave_logged[p] = true;
                std::printf("[bm64 net] Player %d left; the AI takes over from tick %llu\n",
                    p, static_cast<unsigned long long>(r.session->leave_tick(p)));
            }
        }
        return step.render ? 1 : 0;
    }
    std::fprintf(stderr, "[bm64 net] Stalled too long waiting for the peer\n");
    std::exit(1);
}

void end_frame(uint8_t* rdram, uint64_t hash) {
    Runtime& r = rt();
    if (!r.session) {
        return;
    }
    // Only final ticks: a tick built on an earlier guess is resimulated later and would report a
    // false desync. Intermediate resim steps skip render-side writes, so they are left out too.
    if (r.confirmed && !r.spec) {
        r.session->notify_hash(r.step_tick, hash);
    }
    if (r.session->desynced()) {
        std::filesystem::path dir = "desync";
        if (const char* d = std::getenv("BM64_DESYNC_DIR")) {
            dir = d;
        }
        auto path = dir / ("player" + std::to_string(r.player) + "_tick_" + std::to_string(r.session->desync_tick()) + ".bin");
        bm64::debug::dump_to(rdram, path);
        bm64::debug::dump_slots_to(rdram + (bm64::debug::kSlotDumpBase - 0x80000000u),
            dir / ("player" + std::to_string(r.player) + "_tick_" + std::to_string(r.session->desync_tick()) + ".slots.bin"));
        std::printf("BM64 DESYNC tick %llu local_hash %016llx\n",
            static_cast<unsigned long long>(r.session->desync_tick()),
            static_cast<unsigned long long>(r.session->local_hash_at(r.session->desync_tick())));
        std::fflush(stdout);
        std::exit(1);
    }
}

void note_barrier(int32_t overlay) {
    Runtime& r = rt();
    if (!r.session) {
        return;
    }
    uint64_t tick = bm64::state::sim_tick();
    if (!r.session->notify_barrier(tick, overlay)) {
        std::printf("[bm64 net] Load of overlay %d at tick %llu came from a guess; rolling back\n",
            overlay, static_cast<unsigned long long>(tick));
        return;
    }
    bm64::state::drop_snapshots_before(tick);
    std::printf("[bm64 net] Load barrier overlay %d at tick %llu\n", overlay, static_cast<unsigned long long>(tick));
}

bool player_left_this_step(int player) {
    if (!rt().on || player < 0 || player >= bm64::rollback::kMaxPlayers) {
        return false;
    }
    return rt().input.player[player].err == bm64::rollback::kLeftErr;
}

bool pads_for_step(uint8_t out[24]) {
    if (!rt().on) {
        return false;
    }
    std::memset(out, 0, 24);
    for (int i = 0; i < 4; i++) {
        const auto& pad = rt().input.player[i];
        uint16_t button = pad.button;
        std::memcpy(out + i * 6, &button, 2);
        out[i * 6 + 2] = static_cast<uint8_t>(pad.stick_x);
        out[i * 6 + 3] = static_cast<uint8_t>(pad.stick_y);
        out[i * 6 + 4] = pad.err;
    }
    return true;
}

void status_line(char* buf, size_t cap) {
    if (buf == nullptr || cap == 0) {
        return;
    }
    buf[0] = '\0';
    Runtime& r = rt();
    if (!r.on || !r.session) {
        return;
    }
    uint64_t silent = 0;
    for (int p = 0; p < r.players; p++) {
        if (p == r.player) {
            continue;
        }
        uint64_t age = r.session->ticks_since_peer(p);
        if (age > silent) {
            silent = age;
        }
    }
    std::snprintf(buf, cap, "net p%d/%d delay %d rollbacks %d silent %llu",
        r.player, r.players, r.session->input_delay(), r.session->rollbacks(),
        static_cast<unsigned long long>(silent));
}

void push_local_pad(const uint8_t pad[6]) {
    if (!rt().session) {
        return;
    }
    bm64::rollback::Pad value;
    uint16_t button = 0;
    std::memcpy(&button, pad, 2);
    value.button = button;
    value.stick_x = static_cast<int8_t>(pad[2]);
    value.stick_y = static_cast<int8_t>(pad[3]);
    value.err = pad[4];
    rt().session->push_local(value);
}

} // namespace bm64::online
