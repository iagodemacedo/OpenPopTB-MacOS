// WSOCK32 on top of macOS BSD sockets (used by the recompiled weanetr.dll for TCP/IP multiplayer).
// Winsock's SOL_SOCKET/SO_*/AF_*/SOCK_* constants match the BSD ones.
#include "handles.h"
#include "input.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>

#define SOCK_BASE 0x1000u
#define INVALID_SOCK 0xffffffffu
#define SOCK_ERR 0xffffffffu
#define MAXSOCK 4096

// Socket handles as on Windows: never reused, and closing a socket wakes whoever is blocked
// on it with an error (on macOS close() doesn't wake a blocked recvfrom and the fd gets reused).
typedef struct { int fd; volatile int closing; int busy; int nonblock; } Sock;
static Sock socks[MAXSOCK];
static uint32_t next_sock = 1;
static pthread_mutex_t sock_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t sock_new(int fd) {
    pthread_mutex_lock(&sock_mu);
    uint32_t i = next_sock++;
    if (i >= MAXSOCK) { pthread_mutex_unlock(&sock_mu); close(fd); return INVALID_SOCK; }
    socks[i].fd = fd; socks[i].closing = 0; socks[i].busy = 0;
    pthread_mutex_unlock(&sock_mu);
    return SOCK_BASE + i * 4;
}
static Sock *sock_of(uint32_t s) {
    if (s < SOCK_BASE || s == INVALID_SOCK || (s & 3)) return NULL;
    uint32_t i = (s - SOCK_BASE) / 4;
    if (i >= MAXSOCK || !socks[i].fd || socks[i].closing) return NULL;
    return &socks[i];
}
static int fd_of(uint32_t s) { Sock *k = sock_of(s); return k ? k->fd : -1; }
// marks the socket as in use by a blocking call (without the GIL)
static Sock *sock_enter(uint32_t s) {
    pthread_mutex_lock(&sock_mu);
    Sock *k = sock_of(s);
    if (k) k->busy++;
    pthread_mutex_unlock(&sock_mu);
    return k;
}
// waits for the socket to become ready in 50 ms slices, giving up if another thread closes it
// (on macOS neither close nor shutdown wakes a blocked UDP recvfrom)
static int wait_ready(Sock *k, short ev) {
    if (k->nonblock) return 1;
    for (;;) {
        if (k->closing) return 0;
        struct pollfd p = {k->fd, ev, 0};
        int n = poll(&p, 1, 50);
        if (n > 0 || (n < 0 && errno != EINTR)) return 1;
    }
}
// returns 1 if the socket was closed during the call
static int sock_leave(Sock *k) {
    pthread_mutex_lock(&sock_mu);
    int closed = k->closing;
    if (--k->busy == 0 && k->closing) { close(k->fd); k->fd = 0; }
    pthread_mutex_unlock(&sock_mu);
    return closed;
}

static uint32_t wsa_err(int e, int connecting) {
    switch (e) {
    case EWOULDBLOCK: return 10035;
    case EINPROGRESS: return connecting ? 10035 : 10036;   // non-blocking connect: Windows returns WOULDBLOCK
    case EALREADY: return 10037;
    case ENOTSOCK: case EBADF: return 10038;
    case EMSGSIZE: return 10040;
    case EINVAL: return 10022;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: case EPIPE: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    default: return 10004 + 0 * e;   // generic WSAEINTR
    }
}
static uint32_t fail(CPU *R, int connecting) { set_last_error(R, wsa_err(errno, connecting)); return SOCK_ERR; }

// Win32 sockaddr {u16 family, u16 port, u32 addr, zero[8]} <-> BSD {u8 len, u8 family, ...}
static void sa_in(uint32_t ga, struct sockaddr_in *sa) {
    memset(sa, 0, sizeof *sa);
    sa->sin_len = sizeof *sa;
    sa->sin_family = (sa_family_t)rd16(ga);
    sa->sin_port = (in_port_t)rd16(ga + 2);
    sa->sin_addr.s_addr = rd32(ga + 4);
}
static void sa_out(uint32_t ga, uint32_t plen, const struct sockaddr_in *sa) {
    if (!ga) return;
    wr16(ga, sa->sin_family); wr16(ga + 2, sa->sin_port); wr32(ga + 4, sa->sin_addr.s_addr);
    memset(PTR(ga + 8), 0, 8);
    if (plen) wr32(plen, 16);
}

SHIM(WSAStartup, 2) {
    uint32_t d = ARG(1);
    memset(PTR(d), 0, 400);
    wr16(d, 0x0101); wr16(d + 2, 0x0202);
    strcpy(gstr(d + 4), "OpenPopTB sockets");
    strcpy(gstr(d + 261), "Running");
    wr16(d + 390, 256); wr16(d + 392, 65467);
    return 0;
}
SHIM(WSACleanup, 0) { return 0; }
SHIM(WSAGetLastError, 0) { return nthread(R)->last_error; }

SHIM(socket, 3) {
    int fd = socket((int)ARG(0), (int)ARG(1), (int)ARG(2));
    if (fd < 0) { LOG("socket(%u,%u,%u) failed: %s", ARG(0), ARG(1), ARG(2), strerror(errno)); set_last_error(R, wsa_err(errno, 0)); return INVALID_SOCK; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    uint32_t h = sock_new(fd);
    LOG("socket(%u,%u,%u) -> %x (fd %d)", ARG(0), ARG(1), ARG(2), h, fd);
    return h;
}
SHIM(closesocket, 1) {
    pthread_mutex_lock(&sock_mu);
    Sock *k = sock_of(ARG(0));
    if (!k) { pthread_mutex_unlock(&sock_mu); set_last_error(R, 10038); return SOCK_ERR; }
    LOG("closesocket %x (fd %d, %d in use)", ARG(0), k->fd, k->busy);
    k->closing = 1;
    shutdown(k->fd, SHUT_RDWR);   // wakes blocked recv/recvfrom/accept
    if (!k->busy) { close(k->fd); k->fd = 0; }
    pthread_mutex_unlock(&sock_mu);
    return 0;
}
SHIM(shutdown, 2) { int fd = fd_of(ARG(0)); if (shutdown(fd, (int)ARG(1)) < 0) return fail(R, 0); return 0; }
SHIM(bind, 3) {
    struct sockaddr_in sa; sa_in(ARG(1), &sa);
    int fd = fd_of(ARG(0));
    // multiple instances on the same machine (testing/LAN) need to share the UDP discovery port
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    // on Windows a UDP socket bound to the interface IP receives broadcasts; on BSD/macOS it doesn't.
    // For game discovery to work, UDP bound to a local IP listens on all interfaces.
    int type = 0; socklen_t tl = sizeof type;
    getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl);
    char ip[32]; snprintf(ip, sizeof ip, "%s", inet_ntoa(sa.sin_addr));
    if (type == SOCK_DGRAM && sa.sin_addr.s_addr != htonl(INADDR_ANY) && sa.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) return fail(R, 0);
    LOG("bind fd %d %s:%u (%s)", fd, ip, ntohs(sa.sin_port), type == SOCK_DGRAM ? "udp" : "tcp");
    return 0;
}
SHIM(listen, 2) { if (listen(fd_of(ARG(0)), (int)ARG(1)) < 0) return fail(R, 0); return 0; }
SHIM(accept, 3) {
    struct sockaddr_in sa; socklen_t l = sizeof sa;
    Sock *k = sock_enter(ARG(0));
    if (!k) { set_last_error(R, 10038); return INVALID_SOCK; }
    gil_unlock();
    int c = wait_ready(k, POLLIN) ? accept(k->fd, (struct sockaddr *)&sa, &l) : -1;
    int e = errno;
    gil_lock();
    if (sock_leave(k)) { if (c >= 0) close(c); set_last_error(R, 10004); return INVALID_SOCK; }
    if (c < 0) { errno = e; set_last_error(R, wsa_err(errno, 0)); return INVALID_SOCK; }
    int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    sa_out(ARG(1), ARG(2), &sa);
    return sock_new(c);
}
SHIM(connect, 3) {
    struct sockaddr_in sa; sa_in(ARG(1), &sa);
    Sock *k = sock_enter(ARG(0));
    if (!k) { set_last_error(R, 10038); return SOCK_ERR; }
    LOG("connect fd %d -> %s:%u", k->fd, inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
    gil_unlock();
    int r = connect(k->fd, (struct sockaddr *)&sa, sizeof sa);
    int e = errno;
    gil_lock();
    if (sock_leave(k)) { set_last_error(R, 10004); return SOCK_ERR; }
    if (r < 0) { errno = e; return fail(R, 1); }
    return 0;
}
SHIM(send, 4) {
    Sock *k = sock_enter(ARG(0));
    if (!k) { set_last_error(R, 10038); return SOCK_ERR; }
    gil_unlock();
    ssize_t r = wait_ready(k, POLLOUT) ? send(k->fd, PTR(ARG(1)), ARG(2), (int)ARG(3)) : -1;
    int e = errno;
    gil_lock();
    if (sock_leave(k)) { set_last_error(R, 10004); return SOCK_ERR; }
    if (r < 0) { errno = e; return fail(R, 0); }
    return (uint32_t)r;
}
SHIM(recv, 4) {
    Sock *k = sock_enter(ARG(0));
    if (!k) { set_last_error(R, 10038); return SOCK_ERR; }
    gil_unlock();
    ssize_t r = wait_ready(k, POLLIN) ? recv(k->fd, PTR(ARG(1)), ARG(2), (int)ARG(3)) : -1;
    int e = errno;
    gil_lock();
    if (sock_leave(k)) { set_last_error(R, 10004); return SOCK_ERR; }
    if (r < 0) { errno = e; return fail(R, 0); }
    return (uint32_t)r;
}
// limited broadcast (255.255.255.255) gives EHOSTUNREACH on macOS: send to the directed broadcast of each
// broadcast-capable interface (which is also delivered to other instances on the same machine)
static ssize_t send_broadcast(int fd, const void *buf, size_t n, int flags, uint16_t port) {
    struct ifaddrs *ifs, *i; ssize_t best = -1; int any = 0;
    if (getifaddrs(&ifs) != 0) return -1;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_broadaddr) continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        struct sockaddr_in d = *(struct sockaddr_in *)i->ifa_broadaddr;
        d.sin_port = port;
        ssize_t r = sendto(fd, buf, n, flags, (struct sockaddr *)&d, sizeof d);
        if (r >= 0) best = r;
        any = 1;
    }
    freeifaddrs(ifs);
    if (!any) {   // no broadcast interface: at least reach local instances
        struct sockaddr_in d = {sizeof d, AF_INET, port, {htonl(INADDR_LOOPBACK)}, {0}};
        best = sendto(fd, buf, n, flags, (struct sockaddr *)&d, sizeof d);
    }
    return best;
}

SHIM(sendto, 6) {
    struct sockaddr_in sa; sa_in(ARG(4), &sa);
    int fd = fd_of(ARG(0));
    if (rt_verbose && sa.sin_addr.s_addr != htonl(INADDR_BROADCAST) && ARG(2) != 177) { static int u; if (u++ < 400) LOG("[%u t%x] sendto (unicast) fd %d -> %s:%u (%u bytes)", now_ms() % 100000, nthread(R)->tid, fd, inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), ARG(2)); }
    if (sa.sin_addr.s_addr == htonl(INADDR_BROADCAST)) {
        ssize_t r = send_broadcast(fd, PTR(ARG(1)), ARG(2), (int)ARG(3), sa.sin_port);
        if (r < 0) return fail(R, 0);
        return (uint32_t)r;
    }
    if (rt_verbose) { static int n; if (n++ < 40) LOG("sendto fd %d -> %s:%u (%u bytes)", fd, inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), ARG(2)); }
    ssize_t r = sendto(fd, PTR(ARG(1)), ARG(2), (int)ARG(3), (struct sockaddr *)&sa, sizeof sa);
    if (r < 0) return fail(R, 0);
    return (uint32_t)r;
}
SHIM(recvfrom, 6) {
    struct sockaddr_in sa; socklen_t l = sizeof sa;
    Sock *k = sock_enter(ARG(0));
    if (!k) { set_last_error(R, 10038); return SOCK_ERR; }
    int fd = k->fd;
    gil_unlock();
    ssize_t r = wait_ready(k, POLLIN) ? recvfrom(fd, PTR(ARG(1)), ARG(2), (int)ARG(3), (struct sockaddr *)&sa, &l) : -1;
    int e = errno;
    gil_lock();
    if (sock_leave(k)) { set_last_error(R, 10004); return SOCK_ERR; }   // WSAEINTR: closed by another thread
    if (r < 0) { errno = e; return fail(R, 0); }
    if (rt_verbose) { static int n; if (n++ < 10 || (r != 29 && r != 177)) LOG("[%u t%x] recvfrom fd %d <- %s:%u (%zd bytes)", now_ms() % 100000, nthread(R)->tid, fd, inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), r); }
    sa_out(ARG(4), ARG(5), &sa);
    return (uint32_t)r;
}
SHIM(setsockopt, 5) {
    int fd = fd_of(ARG(0));
    int level = (int)ARG(1), opt = (int)ARG(2);
    uint32_t len = ARG(4);
    if (level == 0xffff && opt == (int)~0x80u) return 0;   // SO_DONTLINGER (Windows only)
    if (len >= 4 && opt != 0x80) {
        int v = (int)rd32(ARG(3));
        LOG("setsockopt fd %d level %x option %x = %d", fd, level, opt, v);
        if (setsockopt(fd, level, opt, &v, sizeof v) < 0) LOG("setsockopt(%d,%d) failed: %s", level, opt, strerror(errno));
        return 0;
    }
    if (opt == 0x80 && len >= 4) {   // linger {u16 onoff, u16 secs}
        struct linger lg = {(int)rd16(ARG(3)), (int)rd16(ARG(3) + 2)};
        setsockopt(fd, level, SO_LINGER, &lg, sizeof lg);
    }
    return 0;
}
SHIM(getsockname, 3) {
    struct sockaddr_in sa; socklen_t l = sizeof sa;
    if (getsockname(fd_of(ARG(0)), (struct sockaddr *)&sa, &l) < 0) return fail(R, 0);
    sa_out(ARG(1), ARG(2), &sa);
    return 0;
}
SHIM(ioctlsocket, 3) {
    int fd = fd_of(ARG(0));
    uint32_t cmd = ARG(1), argp = ARG(2);
    LOG("ioctlsocket %x fd %d cmd %x", ARG(0), fd, cmd);
    if (cmd == 0x8004667E) {   // FIONBIO
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, rd32(argp) ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
        Sock *k = sock_of(ARG(0)); if (k) k->nonblock = rd32(argp) != 0;
        return 0;
    }
    if (cmd == 0x4004667F) {   // FIONREAD
        int n = 0; ioctl(fd, FIONREAD, &n); wr32(argp, (uint32_t)n);
        if (rt_verbose) { static int k; if (k++ < 10 || n > 0) LOG("FIONREAD fd %d -> %d", fd, n); }
        return 0;
    }
    set_last_error(R, 10045);
    return SOCK_ERR;
}

// Winsock fd_set: {u32 count; SOCKET array[64]}
static int fdset_in(uint32_t g, fd_set *s, int *maxfd) {
    FD_ZERO(s);
    if (!g) return 0;
    uint32_t n = rd32(g);
    for (uint32_t i = 0; i < n && i < 64; i++) {
        int fd = fd_of(rd32(g + 4 + 4 * i));
        if (fd >= 0 && fd < FD_SETSIZE) { FD_SET(fd, s); if (fd > *maxfd) *maxfd = fd; }
    }
    return 1;
}
static uint32_t fdset_out(uint32_t g, fd_set *s) {
    if (!g) return 0;
    uint32_t n = rd32(g), k = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        uint32_t sock = rd32(g + 4 + 4 * i);
        int fd = fd_of(sock);
        if (fd >= 0 && FD_ISSET(fd, s)) wr32(g + 4 + 4 * k++, sock);
    }
    wr32(g, k);
    return k;
}
SHIM(select, 5) {
    fd_set r, w, x; int maxfd = -1;
    int hr = fdset_in(ARG(1), &r, &maxfd), hw = fdset_in(ARG(2), &w, &maxfd), hx = fdset_in(ARG(3), &x, &maxfd);
    struct timeval tv, *ptv = NULL;
    if (ARG(4)) { tv.tv_sec = (int32_t)rd32(ARG(4)); tv.tv_usec = (int32_t)rd32(ARG(4) + 4); ptv = &tv; }
    gil_unlock();
    int n = select(maxfd + 1, hr ? &r : NULL, hw ? &w : NULL, hx ? &x : NULL, ptv);
    int e = errno;
    gil_lock();
    if (n < 0) { errno = e; return fail(R, 0); }
    if (rt_verbose) { static int k; if (k++ < 30 || n > 0) LOG("select maxfd=%d tmo=%s -> %d", maxfd, ptv ? "yes" : "infinite", n); }
    uint32_t total = fdset_out(ARG(1), &r) + fdset_out(ARG(2), &w) + fdset_out(ARG(3), &x);
    return total;
}
SHIM(__WSAFDIsSet, 2) {
    uint32_t s = ARG(0), g = ARG(1), n = rd32(g);
    for (uint32_t i = 0; i < n && i < 64; i++) if (rd32(g + 4 + 4 * i) == s) return 1;
    return 0;
}

SHIM(htons, 1) { return htons((uint16_t)ARG(0)); }
SHIM(ntohs, 1) { return ntohs((uint16_t)ARG(0)); }
SHIM(htonl, 1) { return htonl(ARG(0)); }
SHIM(ntohl, 1) { return ntohl(ARG(0)); }
SHIM(inet_addr, 1) { return (uint32_t)inet_addr(gstr(ARG(0))); }
SHIM(inet_ntoa, 1) {
    static uint32_t buf;
    if (!buf) buf = gmalloc(32);
    struct in_addr a = {ARG(0)};
    strcpy(gstr(buf), inet_ntoa(a));
    return buf;
}
// simple local name: the game resolves its own name to find its IP, and macOS mDNS can hang
#define LOCAL_NAME "poptb-mac"
SHIM(gethostname, 2) { strncpy(gstr(ARG(0)), LOCAL_NAME, ARG(1)); return 0; }

// IPv4 of active interfaces (no loopback). Prefers broadcast interfaces (Wi-Fi/Ethernet "en*")
// over VPNs (utun/ipsec): weanetr's game discovery uses broadcast on the local subnet.
static int local_addrs(uint32_t *out, int max) {
    struct ifaddrs *ifs, *i; int n = 0;
    if (getifaddrs(&ifs) != 0) return 0;
    for (int pass = 0; pass < 2; pass++)
        for (i = ifs; i && n < max; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            int good = (i->ifa_flags & IFF_BROADCAST) && !strncmp(i->ifa_name, "en", 2);
            if ((pass == 0) != good) continue;
            out[n++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        }
    freeifaddrs(ifs);
    if (!n) out[n++] = htonl(INADDR_LOOPBACK);
    return n;
}
static struct hostent *local_hostent(void) {
    static uint32_t addrs[8]; static char *list[9]; static struct hostent he;
    int n = local_addrs(addrs, 8);
    for (int i = 0; i < n; i++) list[i] = (char *)&addrs[i];
    list[n] = NULL;
    he.h_name = (char *)LOCAL_NAME; he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = list;
    return &he;
}
static struct hostent *resolve(const char *name) {
    if (!strcasecmp(name, LOCAL_NAME) || !name[0]) return local_hostent();
    return gethostbyname(name);
}

// Win32 HOSTENT (16 bytes) built in a guest buffer
static uint32_t build_hostent(uint32_t buf, uint32_t cap, const char *name, struct hostent *he) {
    uint32_t need = 16 + 8 + 4 + 4 * 9 + (uint32_t)strlen(name) + 1 + 4 * 8;
    if (cap < need) return 0;
    uint32_t p = buf + 16;
    uint32_t aliases = p; wr32(p, 0); p += 4;
    uint32_t list = p; int n = 0;
    for (; he->h_addr_list[n] && n < 8; n++) {}
    p += 4 * (uint32_t)(n + 1);
    for (int i = 0; i < n; i++) { memcpy(PTR(p), he->h_addr_list[i], 4); wr32(list + 4 * i, p); p += 4; }
    wr32(list + 4 * n, 0);
    uint32_t nm = p; strcpy(gstr(nm), name);
    wr32(buf, nm); wr32(buf + 4, aliases); wr16(buf + 8, 2); wr16(buf + 10, 4); wr32(buf + 12, list);
    return need;
}
SHIM(gethostbyname, 1) {
    static uint32_t buf;
    if (!buf) buf = gmalloc(1024);
    const char *name = gstr(ARG(0));
    gil_unlock();
    struct hostent *he = resolve(name);
    gil_lock();
    LOG("gethostbyname(%s) -> %s", name, he ? "ok" : "failed");
    if (!he) { set_last_error(R, 11001); return 0; }   // WSAHOST_NOT_FOUND
    return build_hostent(buf, 1024, name, he) ? buf : 0;
}
SHIM(WSAAsyncGetHostByName, 5) {
    // resolves immediately and notifies the window as Winsock would asynchronously
    static uint32_t next = 0x4000;
    uint32_t hwnd = ARG(0), msg = ARG(1), buf = ARG(3), cap = ARG(4);
    const char *name = gstr(ARG(2));
    uint32_t h = next++;
    gil_unlock();
    struct hostent *he = resolve(name);
    gil_lock();
    uint32_t err = 0, need = 0;
    if (!he) err = 11001;
    else if (!(need = build_hostent(buf, cap, name, he))) err = 10055;
    msgq_post(hwnd, msg, h, (err << 16) | (need & 0xffff));
    LOG("WSAAsyncGetHostByName(%s) -> error %u", name, err);
    return h;
}
SHIM(WSACancelAsyncRequest, 1) { return 0; }

// DirectPlay (lobby, IPX, modem) doesn't exist on macOS: only weanetr's own TCP/IP works
SHIM_AS(dplay_lobby_create, "#4", 5) { if (ARG(1)) wr32(ARG(1), 0); return 0x80004005; }
