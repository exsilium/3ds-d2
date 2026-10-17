// WS2_32 / WSOCK32 for an offline console: Winsock starts, the host has a name
// and a loopback address, and every socket call fails with "network down".
// The game probes its TCP/IP support at startup and carries on without it
// (single player and the local menus need no network). Imported by ordinal,
// so each function is registered under its ordinal and its name.
//
// DSOUND lives here too for now: DirectSoundCreate reports "no driver" and
// the game runs silent until an ndsp-backed sink exists.
#include "runtime/rt_host.h"
#include "runtime/rt_layout.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_scratch.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace d2rt;

namespace rt {
namespace {

constexpr uint32_t kSocketError = 0xFFFFFFFFu;   // SOCKET_ERROR / INVALID_SOCKET
constexpr uint32_t WSAENOTSOCK = 10038, WSAENETDOWN = 10050, WSAEINVAL = 10022;

uint32_t g_hostent = 0;     // guest hostent for 127.0.0.1, built on first use
uint32_t g_ntoa_buf = 0;    // inet_ntoa's static buffer (16 bytes)

uint32_t bswap16(uint32_t v) { return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF); }
uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }

uint32_t fail(Cpu &c, uint32_t err)
{
    set_last_error(c, err);   // WSAGetLastError reads the thread's last error, as on Windows
    return kSocketError;
}

uint32_t loopback_hostent(Cpu &c)
{
    if (g_hostent)
        return g_hostent;
    // hostent {h_name, h_aliases, h_addrtype:16, h_length:16, h_addr_list}, then
    // the lists and the address.
    const uint32_t base = wx86_scratch_alloc(48);
    const uint32_t name = wx86_scratch_put_cstr(c, "localhost");
    const uint32_t addr = base + 16, list = base + 20, aliases = base + 28;
    c.write_u32(base + 0, name);
    c.write_u32(base + 4, aliases);
    c.write_u32(base + 8, 2 | (4u << 16));   // AF_INET, 4-byte addresses
    c.write_u32(base + 12, list);
    c.write_u32(addr, 0x0100007Fu);          // 127.0.0.1 in network order
    c.write_u32(list, addr);
    c.write_u32(list + 4, 0);
    c.write_u32(aliases, 0);
    g_hostent = base;
    return base;
}

}  // namespace

void install_net(Bridge &br)
{
    g_hostent = g_ntoa_buf = 0;
    struct Fn {
        uint32_t ordinal;
        const char *name;
        uint32_t argc;
        std::function<uint32_t(Cpu &)> fn;
    };
    const Fn fns[] = {
        {115, "WSAStartup", 2, [](Cpu &c) {
             const uint32_t d = c.arg(1);   // WSADATA: wVersion, wHighVersion, szDescription[257]...
             if (d) {
                 static const uint8_t zero[400] = {};
                 c.write(d, zero, sizeof zero);
                 c.write_u32(d, 0x02020202u);
                 c.write(d + 4, "3ds-d2 offline", 15);
             }
             return 0u;
         }},
        {116, "WSACleanup", 0, [](Cpu &) { return 0u; }},
        {111, "WSAGetLastError", 0, [](Cpu &c) { return c.read_u32(current_teb() + kTebLastError); }},
        {112, "WSASetLastError", 1, [](Cpu &c) {
             set_last_error(c, c.arg(0));
             return 0u;
         }},
        {57, "gethostname", 2, [](Cpu &c) -> uint32_t {
             if (c.arg(1) < 10)
                 return fail(c, 10014 /*WSAEFAULT*/);
             c.write(c.arg(0), "localhost", 10);
             return 0;
         }},
        {52, "gethostbyname", 1, [](Cpu &c) { return loopback_hostent(c); }},
        {51, "gethostbyaddr", 3, [](Cpu &c) { return loopback_hostent(c); }},
        {8, "htonl", 1, [](Cpu &c) { return bswap32(c.arg(0)); }},
        {14, "ntohl", 1, [](Cpu &c) { return bswap32(c.arg(0)); }},
        {9, "htons", 1, [](Cpu &c) { return bswap16(c.arg(0)); }},
        {15, "ntohs", 1, [](Cpu &c) { return bswap16(c.arg(0)); }},
        {11, "inet_addr", 1, [](Cpu &c) -> uint32_t {
             unsigned a, b, d, e;
             const std::string s = read_cstr(c, c.arg(0), 32);
             if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &d, &e) != 4 || a > 255 || b > 255 || d > 255 || e > 255)
                 return 0xFFFFFFFFu;   // INADDR_NONE
             return a | (b << 8) | (d << 16) | (e << 24);
         }},
        {12, "inet_ntoa", 1, [](Cpu &c) {
             if (!g_ntoa_buf)
                 g_ntoa_buf = wx86_scratch_alloc(16);
             const uint32_t v = c.arg(0);
             char s[16];
             std::snprintf(s, sizeof s, "%u.%u.%u.%u", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
             c.write(g_ntoa_buf, s, (uint32_t)std::strlen(s) + 1);
             return g_ntoa_buf;
         }},
        {23, "socket", 3, [](Cpu &c) { return fail(c, WSAENETDOWN); }},
        {3, "closesocket", 1, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {1, "accept", 3, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {2, "bind", 3, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {4, "connect", 3, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {5, "getpeername", 3, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {6, "getsockname", 3, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {7, "getsockopt", 5, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {10, "ioctlsocket", 3, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {13, "listen", 2, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {16, "recv", 4, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {17, "recvfrom", 6, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {18, "select", 5, [](Cpu &c) { return fail(c, WSAEINVAL); }},
        {19, "send", 4, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {20, "sendto", 6, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {21, "setsockopt", 5, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {22, "shutdown", 2, [](Cpu &c) { return fail(c, WSAENOTSOCK); }},
        {151, "__WSAFDIsSet", 2, [](Cpu &) { return 0u; }},
    };
    for (const char *dll : {"WS2_32.dll", "WSOCK32.dll"})
        for (const Fn &f : fns) {
            Shim s;
            s.argc = f.argc;
            s.stdcall_cleanup = true;
            s.tag = std::string(dll) + "!" + f.name;
            s.fn = f.fn;
            br.register_shim(dll, f.name, s);
            br.register_shim_ordinal(dll, f.ordinal, s);
        }

    // ---- DSOUND: no device yet ----
    Shim create;
    create.argc = 3;
    create.tag = "DSOUND.dll!DirectSoundCreate";
    create.fn = [](Cpu &c) {
        if (c.arg(1))
            c.write_u32(c.arg(1), 0);
        return 0x88780078u;   // DSERR_NODRIVER
    };
    br.register_shim("DSOUND.dll", "DirectSoundCreate", create);
    br.register_shim_ordinal("DSOUND.dll", 1, create);
    Shim enumerate;
    enumerate.argc = 2;
    enumerate.tag = "DSOUND.dll!DirectSoundEnumerateA";
    enumerate.fn = [](Cpu &) { return 0u; };   // DS_OK, no devices listed
    br.register_shim("DSOUND.dll", "DirectSoundEnumerateA", enumerate);
    br.register_shim_ordinal("DSOUND.dll", 2, enumerate);
}

}  // namespace rt
