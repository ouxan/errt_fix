// dinput8.dll proxy. Forwards DirectInput to the system DLL and installs the hooks in Elden Ring.
#include "common.h"

#include <psapi.h>

#include <cstdarg>
#include <cwctype>
#include <share.h>

namespace errt {

Config g_cfg;
std::wstring g_outDir;

static std::wstring s_dllDir;
static LARGE_INTEGER s_freq{}, s_start{};
static uintptr_t s_modBase = 0, s_modEnd = 0;

bool InOurModule(const void* p) {
    const uintptr_t a = (uintptr_t)p;
    return s_modBase && a >= s_modBase && a < s_modEnd;
}

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double TicksToMs(int64_t ticks) { return (double)ticks * 1000.0 / (double)s_freq.QuadPart; }
double Seconds() { return (double)(Now() - s_start.QuadPart) / (double)s_freq.QuadPart; }

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    char buf[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

// ---------------------------------------------------------------- logging
static std::mutex s_logMtx;
static FILE* s_events = nullptr;
static uint64_t s_logLines = 0;

static void LogOpen() {
    if (!CreateDirectoryW(g_outDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        // Fall back to %LOCALAPPDATA% when the game folder is not writable.
        wchar_t local[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
            g_outDir = std::wstring(local) + L"\\errt_fix\\";
            CreateDirectoryW(g_outDir.c_str(), nullptr);
        }
    }
    s_events = _wfsopen((g_outDir + L"events.log").c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(s_logMtx);
    if (!s_events || s_logLines > 20000) return;
    ++s_logLines;
    fprintf(s_events, "[%9.3fs f=%llu] %s\n", Seconds(),
            (unsigned long long)g_frame.load(std::memory_order_relaxed), buf);
    fflush(s_events);  // few lines per session; flushed so the log survives a crash
}

void RawAlert(const char* fmt, ...) {
    char line[1024];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u  ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    line[n++] = '\r';
    line[n++] = '\n';
    HANDLE f = CreateFileW((s_dllDir + L"errt_fix_alert.txt").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    SetFilePointer(f, 0, nullptr, FILE_END);
    WriteFile(f, line, (DWORD)n, &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

// ---------------------------------------------------------------- config
static void LoadConfig() {
    const std::wstring ini = s_dllDir + L"errt_fix.ini";
    auto I = [&](const wchar_t* key, int def) {
        return (int)GetPrivateProfileIntW(L"errt_fix", key, def, ini.c_str());
    };
    g_cfg.rtCache = I(L"rt_cache", 1) != 0;
    g_cfg.rtCacheMax = I(L"rt_cache_max", 2000);
    g_cfg.rtScan = I(L"rt_scan", 0) != 0;
    g_cfg.rtScanSeconds = I(L"rt_scan_seconds", 30);
    g_cfg.refitThinning = I(L"refit_thinning", 1) != 0;
    g_cfg.refitFrames = I(L"refit_frames", 2);
    g_cfg.psoReuse = I(L"pso_reuse", 1) != 0;
    g_cfg.psoReuseMinMs = I(L"pso_reuse_min_ms", 2);
    if (g_cfg.refitFrames < 2) g_cfg.refitFrames = 2;
}

static DWORD WINAPI InitThread(LPVOID) {
    LogOpen();
    Log("errt_fix %s: rt_cache=%d (max %d) rt_scan=%d refit_thinning=%d (every %d frames) pso_reuse=%d", kVersion,
        (int)g_cfg.rtCache, g_cfg.rtCacheMax, (int)g_cfg.rtScan, (int)g_cfg.refitThinning, g_cfg.refitFrames,
        (int)g_cfg.psoReuse);
    const bool ok = InstallHooks();
    Log(ok ? "hooks installed" : "HOOK INSTALL FAILED - the fixes are not active");
    RtScanStart();
    return 0;
}

}  // namespace errt

// ---------------------------------------------------------------- dinput8 proxy
static HMODULE RealDinput() {
    static HMODULE h = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        wchar_t p[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) {
            errt::RawAlert("GetSystemDirectoryW failed (%u) - dinput8 forwarding disabled", n);
            return nullptr;
        }
        wcscat_s(p, L"\\dinput8.dll");
        HMODULE m = LoadLibraryW(p);
        // Forwarding to this module would recurse indefinitely.
        if (m && errt::InOurModule((void*)m)) {
            errt::RawAlert("dinput8 proxy resolved to errt_fix itself (%p) - forwarding disabled", (void*)m);
            return nullptr;
        }
        if (!m) errt::RawAlert("could not load the real dinput8.dll (error %lu)", GetLastError());
        h = m;
    }
    return h;
}
static FARPROC RealProc(const char* name) {
    HMODULE h = RealDinput();
    if (!h) return nullptr;
    FARPROC p = GetProcAddress(h, name);
    if (p && errt::InOurModule((void*)p)) {
        errt::RawAlert("dinput8 export %s resolves back into errt_fix - refusing to call it", name);
        return nullptr;
    }
    return p;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using F = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto f = (F)RealProc("DirectInput8Create");
    return f ? f(inst, ver, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using F = HRESULT(WINAPI*)();
    auto f = (F)RealProc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using F = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto f = (F)RealProc("DllGetClassObject");
    return f ? f(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    using F = HRESULT(WINAPI*)();
    auto f = (F)RealProc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    using F = HRESULT(WINAPI*)();
    auto f = (F)RealProc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
LPCVOID WINAPI Proxy_GetdfDIJoystick() {
    using F = LPCVOID(WINAPI*)();
    auto f = (F)RealProc("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    QueryPerformanceFrequency(&errt::s_freq);
    QueryPerformanceCounter(&errt::s_start);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(h, path, MAX_PATH);
    std::wstring dir(path);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    errt::s_dllDir = dir;
    errt::g_outDir = dir + L"errt_fix\\";

    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) {
        errt::s_modBase = (uintptr_t)mi.lpBaseOfDll;
        errt::s_modEnd = errt::s_modBase + mi.SizeOfImage;
    }

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring e(exe);
    e = e.substr(e.find_last_of(L"\\/") + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);

    errt::LoadConfig();
    // Substring match, so renamed or wrapped executables are recognized.
    const bool target = e.find(L"eldenring") != std::wstring::npos;
    if (target) {
        HANDLE t = CreateThread(nullptr, 0, errt::InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
