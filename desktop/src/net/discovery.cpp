// Discovery of CrossPoint devices. Three paths run in this order:
// 1. UDP "hello" to port 8134 as a broadcast on every interface.
// 2. UDP "hello" as a unicast to every host of each local /24. Wi-Fi access
//    points and ESP32 power save often drop broadcasts, but not unicasts.
// 3. A TCP probe of GET /api/status on port 80 across the same hosts.
// Each CrossPoint replies to UDP with "crosspoint (on <hostname>);81". The
// sender address is the device IP. The mDNS name is the last fallback.
#include "net/net_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace ks::net {

namespace {

constexpr unsigned short kDiscoveryPort = 8134;

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
void closeSock(sock_t s) { closesocket(s); }
bool ensureSockets() {
    static bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
#else
using sock_t = int;
constexpr sock_t kBadSock = -1;
void closeSock(sock_t s) { close(s); }
bool ensureSockets() { return true; }
#endif

// IPv4 broadcast addresses of the up interfaces (network byte order).
std::set<uint32_t> broadcastAddresses() {
    std::set<uint32_t> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::unique_ptr<unsigned char[]> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.reset(new unsigned char[size]);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            unsigned prefix = u->OnLinkPrefixLength;
            if (prefix == 0 || prefix >= 32) continue;
            uint32_t ip = ntohl(sin->sin_addr.s_addr);
            uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
            out.insert(htonl(ip | ~mask));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
            out.insert(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
    }
    freeifaddrs(list);
#endif
    return out;
}

// Local IPv4 networks: (host address, prefix length), host byte order.
struct LocalNet { uint32_t ip; unsigned prefix; };
std::vector<LocalNet> localNets() {
    std::vector<LocalNet> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::unique_ptr<unsigned char[]> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.reset(new unsigned char[size]);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            unsigned prefix = u->OnLinkPrefixLength;
            if (prefix == 0 || prefix >= 32) continue;
            out.push_back({ntohl(sin->sin_addr.s_addr), prefix});
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t mask = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
        unsigned prefix = 0;
        while (prefix < 32 && (mask & (0x80000000u >> prefix))) ++prefix;
        if (prefix == 0 || prefix >= 32) continue;
        out.push_back({ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr), prefix});
    }
    freeifaddrs(list);
#endif
    return out;
}

// Every host address of the local networks, host byte order. Networks larger
// than /22 are cut to the /24 around the local address. At most kMaxHosts.
constexpr size_t kMaxHosts = 2048;
std::vector<uint32_t> sweepHosts() {
    std::vector<uint32_t> hosts;
    std::set<uint32_t> seen;
    for (const LocalNet& n : localNets()) {
        unsigned prefix = n.prefix < 22 ? 24 : n.prefix;
        uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
        uint32_t base = n.ip & mask;
        uint32_t bcast = base | ~mask;
        for (uint32_t h = base + 1; h < bcast && hosts.size() < kMaxHosts; ++h) {
            if (h == n.ip) continue;
            if (seen.insert(h).second) hosts.push_back(h);
        }
    }
    return hosts;
}

std::string ipToString(uint32_t hostOrder) {
    in_addr a;
    a.s_addr = htonl(hostOrder);
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

// TCP probe: connect to ip:80, GET /api/status, true when the reply looks
// like CrossPoint JSON. Blocks at most about 2 * timeoutMs.
bool probeStatus(uint32_t hostOrder, int timeoutMs, std::string& info) {
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSock) return false;
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    to.sin_addr.s_addr = htonl(hostOrder);
    connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to);
    fd_set ws;
    FD_ZERO(&ws);
    FD_SET(s, &ws);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    bool ok = false;
    if (select(static_cast<int>(s + 1), nullptr, &ws, nullptr, &tv) > 0) {
        int err = 0;
#ifdef _WIN32
        int len = sizeof err;
#else
        socklen_t len = sizeof err;
#endif
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err == 0) {
            std::string req = "GET /api/status HTTP/1.0\r\nHost: " + ipToString(hostOrder) + "\r\nConnection: close\r\n\r\n";
            send(s, req.data(), static_cast<int>(req.size()), 0);
            std::string resp;
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < end && resp.size() < 8192) {
                fd_set rs;
                FD_ZERO(&rs);
                FD_SET(s, &rs);
                timeval rtv;
                rtv.tv_sec = 0;
                rtv.tv_usec = 100 * 1000;
                if (select(static_cast<int>(s + 1), &rs, nullptr, nullptr, &rtv) <= 0) continue;
                char buf[2048];
                int got = static_cast<int>(recv(s, buf, sizeof buf, 0));
                if (got <= 0) break;
                resp.append(buf, static_cast<size_t>(got));
            }
            auto body = resp.find("\r\n\r\n");
            if (resp.rfind("HTTP/1.", 0) == 0 && body != std::string::npos) {
                std::string b = resp.substr(body + 4);
                std::string low = toLower(b);
                // CrossPoint /api/status has "version" and "mode" ("AP" or "STA").
                if (low.find("\"version\"") != std::string::npos && low.find("\"mode\"") != std::string::npos) {
                    info = trim(b);
                    ok = true;
                }
            }
        }
    }
    closeSock(s);
    return ok;
}

// Resolves crosspoint.local in a detached thread. Waits at most waitMs.
std::string resolveMdns(int waitMs) {
    struct State {
        std::mutex m;
        std::string ip;
        bool done = false;
    };
    auto st = std::make_shared<State>();
    std::thread([st] {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string ip;
        if (getaddrinfo("crosspoint.local", nullptr, &hints, &res) == 0 && res) {
            char buf[INET_ADDRSTRLEN] = {0};
            auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) ip = buf;
            freeaddrinfo(res);
        }
        std::lock_guard<std::mutex> lk(st->m);
        st->ip = ip;
        st->done = true;
    }).detach();
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < end) {
        {
            std::lock_guard<std::mutex> lk(st->m);
            if (st->done) return st->ip;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {};
}

} // namespace

bool parseDiscoveryReply(const std::string& raw, Discovered& out) {
    std::string s = trim(raw);
    std::string low = toLower(s);
    if (low.rfind("crosspoint", 0) != 0) return false;
    std::string host;
    int port = 81;
    auto on = low.find("(on ");
    if (on != std::string::npos) {
        auto close = s.find(')', on);
        if (close == std::string::npos) return false;
        host = trim(s.substr(on + 4, close - on - 4));
    }
    auto semi = s.rfind(';');
    if (semi != std::string::npos) {
        std::string p = trim(s.substr(semi + 1));
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
        port = std::stoi(p);
        if (port < 1 || port > 65535) return false;
    }
    out.hostname = host;
    out.wsPort = port;
    out.raw = s;
    return true;
}

std::vector<Discovered> discoverCrossPoint(int timeoutMs) {
    std::vector<Discovered> found;
    if (!ensureSockets()) return found;
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSock) return found;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    bind(s, reinterpret_cast<sockaddr*>(&local), sizeof local);

    std::set<uint32_t> targets = broadcastAddresses();
    targets.insert(htonl(INADDR_BROADCAST));
    static const char hello[] = "hello";
    auto sendHello = [&](uint32_t addrNet) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kDiscoveryPort);
        to.sin_addr.s_addr = addrNet;
        sendto(s, hello, static_cast<int>(sizeof hello - 1), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    };
    for (uint32_t addr : targets) sendHello(addr);
    // Unicast sweep. Access points often drop broadcasts to Wi-Fi clients.
    const std::vector<uint32_t> hosts = sweepHosts();
    for (uint32_t h : hosts) sendHello(htonl(h));
    // Send the broadcast once more. The first one can go out before Wi-Fi wakes.
    for (uint32_t addr : targets) sendHello(addr);

    std::set<std::string> seen;
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        int left = timeoutMs - detail::msSince(t0);
        if (left <= 0) break;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        timeval tv;
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        int n = select(static_cast<int>(s + 1), &rs, nullptr, nullptr, &tv);
        if (n <= 0) break;
        char buf[512];
        sockaddr_in from{};
#ifdef _WIN32
        int fromLen = sizeof from;
#else
        socklen_t fromLen = sizeof from;
#endif
        int got = static_cast<int>(recvfrom(s, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
        if (got <= 0) continue;
        Discovered d;
        if (!parseDiscoveryReply(std::string(buf, static_cast<size_t>(got)), d)) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip)) continue;
        d.ip = ip;
        if (seen.insert(d.ip).second) found.push_back(d);
    }
    closeSock(s);

    if (found.empty() && !hosts.empty()) {
        // TCP probe of /api/status on port 80. Runs in 32 worker threads.
        std::mutex m;
        std::atomic<size_t> next{0};
        std::vector<std::thread> workers;
        const size_t nWorkers = std::min<size_t>(32, hosts.size());
        for (size_t w = 0; w < nWorkers; ++w) {
            workers.emplace_back([&] {
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= hosts.size()) return;
                    std::string info;
                    if (!probeStatus(hosts[i], 400, info)) continue;
                    Discovered d;
                    d.ip = ipToString(hosts[i]);
                    d.hostname = "crosspoint";
                    d.wsPort = 81;
                    d.raw = "http " + info;
                    std::lock_guard<std::mutex> lk(m);
                    if (seen.insert(d.ip).second) found.push_back(d);
                }
            });
        }
        for (auto& t : workers) t.join();
    }

    if (found.empty()) {
        // Fallback: the mDNS name. It resolves on most LANs.
        std::string ip = resolveMdns(1000);
        if (!ip.empty()) {
            Discovered d;
            d.ip = ip;
            d.hostname = "crosspoint.local";
            d.wsPort = 81;
            d.raw = "mdns";
            found.push_back(d);
        }
    }
    return found;
}

} // namespace ks::net
