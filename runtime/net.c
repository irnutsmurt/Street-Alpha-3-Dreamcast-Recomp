/* UDP transport for netplay (POSIX sockets / Winsock), with a simulator that adds
 * delay, jitter and packet loss to outgoing packets for testing on one PC. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#endif

#include "net.h"
#include "runtime.h"

static sock_t s_sock = BAD_SOCK;

/* ---------------- simulator ---------------- */

static int s_sim_delay, s_sim_jitter, s_sim_loss;
static uint32_t s_sim_rng = 0x12345678;

typedef struct {
    uint64_t due;
    NetAddr to;
    size_t len;
    uint8_t data[1400];
} SimPacket;
static SimPacket s_sim_q[512];
static int s_sim_n;

void net_sim_config(int delay_ms, int jitter_ms, int loss_pct) {
    s_sim_delay = delay_ms;
    s_sim_jitter = jitter_ms;
    s_sim_loss = loss_pct;
    logf_("net: simulating %d ms delay, %d ms jitter, %d%% loss", delay_ms, jitter_ms, loss_pct);
}

static uint32_t sim_rand(void) {
    s_sim_rng ^= s_sim_rng << 13;
    s_sim_rng ^= s_sim_rng >> 17;
    s_sim_rng ^= s_sim_rng << 5;
    return s_sim_rng;
}

static void raw_send(const NetAddr *to, const void *buf, size_t len) {
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(to->port)};
    sa.sin_addr.s_addr = to->ip;
    sendto(s_sock, buf, (int)len, 0, (struct sockaddr *)&sa, sizeof sa);
}

static void sim_flush(void) {
    uint64_t now = SDL_GetTicksNS();
    for (int i = 0; i < s_sim_n;) {
        if (s_sim_q[i].due <= now) {
            raw_send(&s_sim_q[i].to, s_sim_q[i].data, s_sim_q[i].len);
            s_sim_q[i] = s_sim_q[--s_sim_n];
        } else {
            i++;
        }
    }
}

/* ---------------- sockets ---------------- */

int net_open(uint16_t port) {
    net_close();
#ifdef _WIN32
    static int wsa;
    if (!wsa) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d)) return 0;
        wsa = 1;
    }
#endif
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock == BAD_SOCK) return 0;
    int one = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY};
    if (bind(s_sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
        logf_("net: cannot bind UDP port %u", port);
        close_sock(s_sock);
        s_sock = BAD_SOCK;
        return 0;
    }
#ifdef _WIN32
    u_long nonblock = 1;
    ioctlsocket(s_sock, FIONBIO, &nonblock);
#else
    fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL) | O_NONBLOCK);
#endif
    const char *sim = getenv("SFA3_NET_SIM");
    if (sim) {
        int d = 0, j = 0, l = 0;
        sscanf(sim, "%d,%d,%d", &d, &j, &l);
        net_sim_config(d, j, l);
    }
    return 1;
}

void net_close(void) {
    if (s_sock != BAD_SOCK) close_sock(s_sock);
    s_sock = BAD_SOCK;
}

int net_resolve(const char *host, uint16_t port, NetAddr *out) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return 0;
    out->ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    out->port = port;
    freeaddrinfo(res);
    return 1;
}

void net_send(const NetAddr *to, const void *buf, size_t len) {
    if (s_sock == BAD_SOCK || len > sizeof s_sim_q[0].data) return;
    if (!s_sim_delay && !s_sim_jitter && !s_sim_loss) {
        raw_send(to, buf, len);
        return;
    }
    sim_flush();
    if (s_sim_loss && (int)(sim_rand() % 100) < s_sim_loss) return;
    if (s_sim_n == (int)(sizeof s_sim_q / sizeof s_sim_q[0])) return;
    int jit = s_sim_jitter ? (int)(sim_rand() % (uint32_t)(2 * s_sim_jitter + 1)) - s_sim_jitter : 0;
    int ms = s_sim_delay + jit;
    SimPacket *p = &s_sim_q[s_sim_n++];
    p->due = SDL_GetTicksNS() + (uint64_t)(ms > 0 ? ms : 0) * 1000000ull;
    p->to = *to;
    p->len = len;
    memcpy(p->data, buf, len);
}

int net_recv(void *buf, size_t cap, NetAddr *from) {
    if (s_sock == BAD_SOCK) return 0;
    sim_flush();
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int n = (int)recvfrom(s_sock, buf, (int)cap, 0, (struct sockaddr *)&sa, &sl);
    if (n <= 0) return 0;
    if (from) {
        from->ip = sa.sin_addr.s_addr;
        from->port = ntohs(sa.sin_port);
    }
    return (int)n;
}

int net_broadcast(uint16_t port, const void *buf, size_t len) {
    if (s_sock == BAD_SOCK) return 0;
    NetAddr a = {htonl(INADDR_BROADCAST), port};
    raw_send(&a, buf, len);
    return 1;
}

/* This machine's IPv4 addresses (not loopback), as text, for "join me at". */
#ifdef _WIN32
int net_local_addrs(char out[][16], int max) {
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = malloc(size);
    int n = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) == ERROR_BUFFER_OVERFLOW) {
        free(list);
        list = malloc(size);
    }
    if (list && GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = list; a && n < max; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            /* Hyper-V / WSL / VirtualBox virtual switches aren't reachable by a friend */
            if (wcsstr(a->Description, L"Hyper-V") || wcsstr(a->Description, L"VirtualBox") ||
                wcsstr(a->FriendlyName, L"WSL"))
                continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
                struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
                if (sin->sin_family != AF_INET) continue;
                inet_ntop(AF_INET, &sin->sin_addr, out[n], 16);
                n++;
            }
        }
    }
    free(list);
    return n;
}
#else
int net_local_addrs(char out[][16], int max) {
    struct ifaddrs *ifs = NULL;
    int n = 0;
    if (getifaddrs(&ifs)) return 0;
    for (struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!(i->ifa_flags & IFF_UP)) continue;
        /* containers' and VMs' virtual bridges aren't reachable by a friend */
        if (!strncmp(i->ifa_name, "docker", 6) || !strncmp(i->ifa_name, "br-", 3) ||
            !strncmp(i->ifa_name, "veth", 4) || !strncmp(i->ifa_name, "virbr", 5))
            continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out[n], 16);
        n++;
    }
    freeifaddrs(ifs);
    return n;
}
#endif

int net_is_open(void) { return s_sock != BAD_SOCK; }

const char *net_addr_str(const NetAddr *a) {
    static char s[32];
    struct in_addr in;
    in.s_addr = a->ip;
    snprintf(s, sizeof s, "%s:%u", inet_ntoa(in), a->port);
    return s;
}
