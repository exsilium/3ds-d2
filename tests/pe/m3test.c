/* M3 fixture: a self-authored PE32 that exercises what plan §5 M3 asks of
 * the runtime — allocation, files, time, events, a second guest thread, TLS
 * through the TEB, a message loop and guest callbacks — and exits through
 * ExitProcess. No C runtime: entry is `start`, output goes through WriteFile.
 *
 * Output: one "CHECK <name> ok|FAIL" line per check, then "RESULT m3 <n>/<m>".
 * The exit code is the number of failed checks. */
#include <windows.h>

static HANDLE g_out;
static int g_pass, g_total;
static volatile LONG g_counter;
static HANDLE g_event;
static DWORD g_tls;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { DWORD n; WriteFile(g_out, s, (DWORD)slen(s), &n, 0); }
static void say_num(unsigned v)
{
    char b[12]; int i = 11; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    say(b + i);
}
static void check(const char *name, int ok)
{
    g_total++; g_pass += ok != 0;
    say("CHECK "); say(name); say(ok ? " ok\n" : " FAIL\n");
}

static DWORD WINAPI worker(LPVOID param)
{
    TlsSetValue(g_tls, (LPVOID)0x2222);
    check("thread.param", (int)(INT_PTR)param == 7);
    check("tls.worker", TlsGetValue(g_tls) == (LPVOID)0x2222);
    InterlockedExchangeAdd(&g_counter, 7);
    SetEvent(g_event);
    return 42;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_USER + 1) {
        InterlockedExchangeAdd(&g_counter, (LONG)w);
        return 5;
    }
    return DefWindowProcA(h, m, w, l);
}

void __stdcall start(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("M3 test begin\n");

    /* allocation */
    char *v = (char *)VirtualAlloc(0, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check("virtualalloc", v != 0);
    if (v) { v[0] = 1; v[65535] = 2; }
    check("virtualalloc.rw", v && v[0] == 1 && v[65535] == 2);
    check("virtualfree", v && VirtualFree(v, 0, MEM_RELEASE));
    int *p = (int *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1000);
    check("heapalloc.zeroed", p && p[0] == 0 && p[249] == 0);
    check("heapfree", p && HeapFree(GetProcessHeap(), 0, p));

    /* files */
    HANDLE f = CreateFileA("m3test.tmp", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    check("file.create", f != INVALID_HANDLE_VALUE);
    DWORD n = 0;
    check("file.write", WriteFile(f, "hello3ds", 8, &n, 0) && n == 8);
    check("file.close", CloseHandle(f));
    f = CreateFileA("C:\\m3test.tmp", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    char buf[16] = {0};
    check("file.read", f != INVALID_HANDLE_VALUE && ReadFile(f, buf, 16, &n, 0) && n == 8 &&
                       buf[0] == 'h' && buf[7] == 's');
    check("file.size", GetFileSize(f, 0) == 8);
    CloseHandle(f);
    f = CreateFileA("does-not-exist.tmp", GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
    check("file.missing", f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);

    /* time */
    DWORD t0 = GetTickCount();
    Sleep(30);
    DWORD dt = GetTickCount() - t0;
    say("INFO time.sleep(30) measured_ms="); say_num(dt); say("\n");
    check("time.sleep", dt >= 20 && dt < 5000);
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    Sleep(5);
    QueryPerformanceCounter(&q1);
    check("time.qpc", qf.QuadPart > 0 && q1.QuadPart > q0.QuadPart);

    /* TLS and the TEB */
    g_tls = TlsAlloc();
    check("tls.alloc", g_tls != TLS_OUT_OF_INDEXES);
    TlsSetValue(g_tls, (LPVOID)0x1111);
    check("tls.main", TlsGetValue(g_tls) == (LPVOID)0x1111);
    DWORD teb = 0, slot = 0;
    __asm__ volatile("movl %%fs:0x18, %0" : "=r"(teb));
    check("teb.self", teb != 0);
    if (teb && g_tls < 64)
        slot = *(DWORD *)(teb + 0xE10 + 4 * g_tls);
    check("tls.in_teb", slot == 0x1111);

    /* events and a second guest thread */
    g_event = CreateEventA(0, FALSE, FALSE, 0);
    check("event.create", g_event != 0);
    DWORD tid = 0;
    HANDLE th = CreateThread(0, 0, worker, (LPVOID)7, 0, &tid);
    check("thread.create", th != 0 && tid != 0);
    check("event.wait", WaitForSingleObject(g_event, 5000) == WAIT_OBJECT_0);
    check("thread.join", WaitForSingleObject(th, 5000) == WAIT_OBJECT_0);
    DWORD code = 0;
    check("thread.exitcode", GetExitCodeThread(th, &code) && code == 42);
    check("tls.isolated", TlsGetValue(g_tls) == (LPVOID)0x1111);
    CloseHandle(th);

    /* window procedure callbacks and a message loop */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = "m3class";
    check("window.register", RegisterClassA(&wc) != 0);
    HWND w = CreateWindowExA(0, "M3Class", "m3", 0, 0, 0, 10, 10, 0, 0, 0, 0);
    check("window.create", w != 0);
    check("sendmessage.callback", SendMessageA(w, WM_USER + 1, 100, 0) == 5);
    PostMessageA(w, WM_USER + 1, 1000, 0);
    PostQuitMessage(9);
    MSG msg;
    int dispatched = 0;
    while (GetMessageA(&msg, 0, 0, 0) > 0)
        if (msg.message == WM_USER + 1) { DispatchMessageA(&msg); dispatched++; }
    check("messageloop.quit", msg.message == WM_QUIT && msg.wParam == 9);
    check("messageloop.dispatch", dispatched == 1 && g_counter == 7 + 100 + 1000);

    say("RESULT m3 "); say_num((unsigned)g_pass); say("/"); say_num((unsigned)g_total); say("\n");
    ExitProcess((UINT)(g_total - g_pass));
}
