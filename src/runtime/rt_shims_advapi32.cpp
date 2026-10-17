// ADVAPI32: the registry, plus the service and security calls the game probes.
//
// The registry is one small in-memory tree. All root keys (HKLM, HKCU, ...)
// share it: the game writes its settings under HKCU and reads its install
// keys from HKLM, and on a console there is only one user. The install keys
// are seeded with the layout rt_shims_files.cpp presents (the game lives in
// C:\, saves in C:\Save\). Values the game writes are persisted to
// <write_root>/registry.txt, rewritten only when a value actually changes,
// since an SD card write is slow and the game sets the same values repeatedly.
#include "runtime/rt_host.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace d2rt;

namespace rt {
namespace {

enum : uint32_t {
    ERROR_SUCCESS = 0,
    ERROR_FILE_NOT_FOUND = 2,
    ERROR_ACCESS_DENIED = 5,
    ERROR_INVALID_HANDLE = 6,
    ERROR_MORE_DATA = 234,
    ERROR_NO_MORE_ITEMS = 259,
    ERROR_FAILED_SERVICE_CONTROLLER_CONNECT = 1063,
};

constexpr uint32_t REG_SZ = 1, REG_DWORD = 4;

struct Value {
    std::string name;   // as first written
    uint32_t type;
    std::vector<uint8_t> data;
};

struct Key {
    std::vector<Value> values;   // in creation order (RegEnumValue)
};

std::map<std::string, Key> g_keys;          // lower-case path, '\'-separated, no root
std::map<uint32_t, std::string> g_handles;  // open key handle -> path
uint32_t g_next_handle = 0x83000010u;

std::string lower(std::string s)
{
    for (char &ch : s)
        ch = (char)std::tolower((unsigned char)ch);
    return s;
}

bool is_root(uint32_t h) { return h >= 0x80000000u && h <= 0x80000006u; }

bool key_path(uint32_t h, std::string &path)
{
    if (is_root(h)) {
        path.clear();
        return true;
    }
    auto it = g_handles.find(h);
    if (it == g_handles.end())
        return false;
    path = it->second;
    return true;
}

std::string join(const std::string &parent, const std::string &sub)
{
    std::string s = lower(sub);
    while (!s.empty() && s[0] == '\\')
        s.erase(0, 1);
    while (!s.empty() && s.back() == '\\')
        s.pop_back();
    if (parent.empty())
        return s;
    return s.empty() ? parent : parent + "\\" + s;
}

bool key_exists(const std::string &path)
{
    if (path.empty() || g_keys.count(path))
        return true;
    auto it = g_keys.lower_bound(path + "\\");   // a parent of an existing key exists too
    return it != g_keys.end() && it->first.compare(0, path.size() + 1, path + "\\") == 0;
}

Value *find_value(const std::string &path, const std::string &name)
{
    auto it = g_keys.find(path);
    if (it == g_keys.end())
        return nullptr;
    const std::string l = lower(name);
    for (Value &v : it->second.values)
        if (lower(v.name) == l)
            return &v;
    return nullptr;
}

uint32_t open_handle(const std::string &path)
{
    const uint32_t h = g_next_handle++;
    g_handles[h] = path;
    return h;
}

// ---- persistence: one line per value, "path|name|type|hex" ---------------------------

std::string store_path() { return options().write_root + "/registry.txt"; }

void save()
{
    FILE *f = std::fopen(store_path().c_str(), "wb");
    if (!f) {
        logf("registry: cannot write %s", store_path().c_str());
        return;
    }
    for (const auto &k : g_keys) {
        if (k.second.values.empty())
            std::fprintf(f, "%s|||\n", k.first.c_str());
        for (const Value &v : k.second.values) {
            std::fprintf(f, "%s|%s|%u|", k.first.c_str(), v.name.c_str(), v.type);
            for (uint8_t b : v.data)
                std::fprintf(f, "%02x", b);
            std::fputc('\n', f);
        }
    }
    std::fclose(f);
}

void set_value(const std::string &path, const std::string &name, uint32_t type, const void *data, size_t n)
{
    Key &k = g_keys[path];
    for (Value &v : k.values)
        if (lower(v.name) == lower(name)) {
            v.type = type;
            v.data.assign((const uint8_t *)data, (const uint8_t *)data + n);
            return;
        }
    k.values.push_back({name, type, std::vector<uint8_t>((const uint8_t *)data, (const uint8_t *)data + n)});
}

void load()
{
    FILE *f = std::fopen(store_path().c_str(), "rb");
    if (!f)
        return;
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        const size_t a = s.find('|'), b = s.find('|', a + 1), c = s.find('|', b + 1);
        if (c == std::string::npos)
            continue;
        const std::string path = s.substr(0, a), name = s.substr(a + 1, b - a - 1), hex = s.substr(c + 1);
        g_keys[path];
        if (b == a + 1 && c == b + 1)   // an empty key
            continue;
        std::vector<uint8_t> data;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            data.push_back((uint8_t)std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        set_value(path, name, (uint32_t)std::strtoul(s.substr(b + 1, c - b - 1).c_str(), nullptr, 10), data.data(),
                  data.size());
    }
    std::fclose(f);
}

void seed_sz(const std::string &path, const char *name, const char *s)
{
    if (!find_value(path, name))
        set_value(path, name, REG_SZ, s, std::strlen(s) + 1);
}

void seed_dword(const std::string &path, const char *name, uint32_t v)
{
    if (!find_value(path, name))
        set_value(path, name, REG_DWORD, &v, 4);
}

void seed()
{
    const std::string d2 = "software\\blizzard entertainment\\diablo ii";
    seed_sz(d2, "InstallPath", "C:\\");
    seed_sz(d2, "Save Path", "C:\\Save\\");
    seed_sz(d2, "NewSavePath", "C:\\Save\\");
    seed_sz(d2, "GamePath", "C:\\Game.exe");
    seed_dword(d2, "InstallType", 3);   // full install: every MPQ is local
    seed_dword(d2, "Render", 0);
    for (const auto &kv : options().registry_dwords)
        seed_dword(d2, kv.first.c_str(), kv.second);
}

// ---- shims ---------------------------------------------------------------------------

uint32_t open_key(Cpu &c, uint32_t parent, uint32_t sub_va, uint32_t out, bool create, uint32_t disp_out)
{
    std::string base;
    if (!key_path(parent, base))
        return ERROR_INVALID_HANDLE;
    const std::string path = join(base, read_cstr(c, sub_va));
    const bool existed = key_exists(path);
    if (!existed && !create)
        return ERROR_FILE_NOT_FOUND;
    if (!existed) {
        g_keys[path];
        save();
    }
    if (out)
        c.write_u32(out, open_handle(path));
    if (disp_out)
        c.write_u32(disp_out, existed ? 2u : 1u);   // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
    return ERROR_SUCCESS;
}

uint32_t query_value(Cpu &c, uint32_t h, const std::string &name, uint32_t ptype, uint32_t data, uint32_t pcb)
{
    std::string path;
    if (!key_path(h, path))
        return ERROR_INVALID_HANDLE;
    const Value *v = find_value(path, name);
    if (!v)
        return ERROR_FILE_NOT_FOUND;
    if (ptype)
        c.write_u32(ptype, v->type);
    const uint32_t n = (uint32_t)v->data.size();
    if (!pcb)
        return data ? ERROR_INVALID_HANDLE : ERROR_SUCCESS;
    const uint32_t cap = c.read_u32(pcb);
    c.write_u32(pcb, n);
    if (!data)
        return ERROR_SUCCESS;
    if (cap < n)
        return ERROR_MORE_DATA;
    if (n)
        c.write(data, v->data.data(), n);
    return ERROR_SUCCESS;
}

}  // namespace

void install_advapi32(Bridge &br)
{
    auto A = [&](const char *name, uint32_t argc, std::function<uint32_t(Cpu &)> fn) {
        Shim s;
        s.argc = argc;
        s.stdcall_cleanup = true;
        s.tag = std::string("ADVAPI32.dll!") + name;
        s.fn = std::move(fn);
        br.register_shim("ADVAPI32.dll", name, s);
    };
    g_keys.clear();
    g_handles.clear();
    g_next_handle = 0x83000010u;
    load();
    seed();

    A("RegOpenKeyExA", 5, [](Cpu &c) { return open_key(c, c.arg(0), c.arg(1), c.arg(4), false, 0); });
    A("RegOpenKeyA", 3, [](Cpu &c) { return open_key(c, c.arg(0), c.arg(1), c.arg(2), false, 0); });
    A("RegCreateKeyExA", 9, [](Cpu &c) { return open_key(c, c.arg(0), c.arg(1), c.arg(7), true, c.arg(8)); });
    A("RegCreateKeyA", 3, [](Cpu &c) { return open_key(c, c.arg(0), c.arg(1), c.arg(2), true, 0); });
    A("RegCloseKey", 1, [](Cpu &c) -> uint32_t {
        if (is_root(c.arg(0)))
            return ERROR_SUCCESS;
        return g_handles.erase(c.arg(0)) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
    });
    A("RegFlushKey", 1, [](Cpu &) { return (uint32_t)ERROR_SUCCESS; });   // every change is already saved
    A("RegQueryValueExA", 6, [](Cpu &c) {
        return query_value(c, c.arg(0), read_cstr(c, c.arg(1)), c.arg(3), c.arg(4), c.arg(5));
    });
    A("RegSetValueExA", 6, [](Cpu &c) -> uint32_t {
        std::string path;
        if (!key_path(c.arg(0), path))
            return ERROR_INVALID_HANDLE;
        const std::string name = read_cstr(c, c.arg(1));
        const uint32_t type = c.arg(3), n = c.arg(5);
        std::vector<uint8_t> data(n);
        if (n)
            c.read(c.arg(4), data.data(), n);
        const Value *old = find_value(path, name);
        if (old && old->type == type && old->data == data)
            return ERROR_SUCCESS;   // unchanged: no SD write
        set_value(path, name, type, data.data(), data.size());
        save();
        return ERROR_SUCCESS;
    });
    A("RegDeleteValueA", 2, [](Cpu &c) -> uint32_t {
        std::string path;
        if (!key_path(c.arg(0), path))
            return ERROR_INVALID_HANDLE;
        auto it = g_keys.find(path);
        const std::string l = lower(read_cstr(c, c.arg(1)));
        if (it != g_keys.end())
            for (size_t i = 0; i < it->second.values.size(); i++)
                if (lower(it->second.values[i].name) == l) {
                    it->second.values.erase(it->second.values.begin() + (long)i);
                    save();
                    return ERROR_SUCCESS;
                }
        return ERROR_FILE_NOT_FOUND;
    });
    A("RegDeleteKeyA", 2, [](Cpu &c) -> uint32_t {
        std::string base;
        if (!key_path(c.arg(0), base))
            return ERROR_INVALID_HANDLE;
        const std::string path = join(base, read_cstr(c, c.arg(1)));
        if (!g_keys.erase(path))
            return ERROR_FILE_NOT_FOUND;
        save();
        return ERROR_SUCCESS;
    });
    A("RegEnumValueA", 8, [](Cpu &c) -> uint32_t {
        std::string path;
        if (!key_path(c.arg(0), path))
            return ERROR_INVALID_HANDLE;
        auto it = g_keys.find(path);
        const uint32_t index = c.arg(1);
        if (it == g_keys.end() || index >= it->second.values.size())
            return ERROR_NO_MORE_ITEMS;
        const Value &v = it->second.values[index];
        const uint32_t name_buf = c.arg(2), pname_len = c.arg(3);
        const uint32_t cap = pname_len ? c.read_u32(pname_len) : 0;
        if (cap < v.name.size() + 1)
            return ERROR_MORE_DATA;
        c.write(name_buf, v.name.c_str(), (uint32_t)v.name.size() + 1);
        c.write_u32(pname_len, (uint32_t)v.name.size());
        return query_value(c, c.arg(0), v.name, c.arg(5), c.arg(6), c.arg(7));
    });

    // ---- security: one user, an administrator ----
    A("AllocateAndInitializeSid", 11, [](Cpu &c) {
        if (c.arg(10))
            c.write_u32(c.arg(10), 0x8A000010u);   // an opaque SID; FreeSid accepts anything
        return 1u;
    });

    // ---- services: there is no service manager ----
    auto denied = [](Cpu &c) {
        set_last_error(c, ERROR_ACCESS_DENIED);
        return 0u;
    };
    A("OpenSCManagerA", 3, denied);
    A("OpenServiceA", 3, denied);
    A("RegisterServiceCtrlHandlerA", 2, denied);
    A("SetServiceStatus", 2, denied);
    A("StartServiceCtrlDispatcherA", 1, [](Cpu &c) {
        set_last_error(c, ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
        return 0u;
    });
}

}  // namespace rt
