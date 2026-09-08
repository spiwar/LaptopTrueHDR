// LaptopTrueHDR — automatic HDR calibration for laptop internal displays.
//
// The HDR content brightness (SDR white level) write uses the undocumented
// DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL DisplayConfigSetDeviceInfo type
// (same mechanism the Settings slider uses). The base peak comes from the
// panel's EDID via WinRT DisplayMonitor; the adjusted peak is what Windows
// reports through DXGI_OUTPUT_DESC1.MaxLuminance.

#include <Windows.h>
#include <dxgi1_6.h>
#include <wbemidl.h>
#include <powrprof.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <filesystem>

// WinRT headers for accessing raw hardware metrics
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Display.h>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(linker, "/subsystem:windows /entry:wmainCRTStartup")

using namespace winrt::Windows::Devices::Display;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Undocumented DISPLAYCONFIG_DEVICE_INFO_TYPE that writes the SDR white level.
constexpr auto DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL = (DISPLAYCONFIG_DEVICE_INFO_TYPE)0xFFFFFFEE;

typedef struct _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    unsigned int SDRWhiteLevel;
    unsigned char finalValue;
} _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL;

static const wchar_t* kAppName = L"LaptopTrueHDR";
static const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName = L"LaptopTrueHDR";
static const wchar_t* kMutexName = L"Local\\LaptopTrueHDR_Watcher";
static const wchar_t* kWatchWndClass = L"LaptopTrueHDR_WatchWnd";

constexpr UINT WM_APP_BRIGHTNESS = WM_APP + 1;  // WMI brightness event fired
constexpr UINT WM_APP_WMI_FAILED = WM_APP + 2;  // event subscription broke -> poll
constexpr UINT WM_APP_REAPPLY    = WM_APP + 3;  // resume / topology change

constexpr int kDebounceMs = 2000;      
constexpr int kPollFallbackMs = 5000;  
constexpr int kMinNits = 80;           
constexpr int kMaxNits = 400;
const int kAnchorPcts[] = { 10, 30, 50, 70, 90 };

// Exit codes
enum ExitCode {
    ExitOk = 0,
    ExitGeneric = 1,
    ExitProfileInvalid = 2,
    ExitHdrOff = 3,
    ExitWriteFailed = 4,
    ExitDisplayError = 5,
};

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

// The white level is carried in thousandths of 80 nits (1000 == 80 nits). Both
// directions round: truncating drops the .5 on odd nits and the slider value
// stops surviving a write/read round trip.
static int LevelToNits(unsigned int level) { return (int)((level * 80 + 500) / 1000); }
static unsigned int NitsToLevel(int nits) { return (unsigned int)((nits * 1000 + 40) / 80); }

static bool SelfCheck() {
    for (int n = kMinNits; n <= kMaxNits; n++)
        if (LevelToNits(NitsToLevel(n)) != n) return false;
    return true;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring NowString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

static unsigned long long Fnv1a64(const unsigned char* data, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= data[i]; h *= 1099511628211ULL; }
    return h;
}

static std::wstring HashToHex(unsigned long long h) {
    wchar_t buf[20];
    swprintf(buf, 20, L"%016llX", h);
    return buf;
}

static std::wstring ProfileDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? buf : L"C:\\Windows\\Temp";
    return base + L"\\" + kAppName;
}

static std::wstring ProfilePath() { return ProfileDir() + L"\\profile.json"; }
static std::wstring LogPath() { return ProfileDir() + L"\\watcher.log"; }

// ---------------------------------------------------------------------------
// Console attach (interactive modes only; --watch stays windowless)
// ---------------------------------------------------------------------------

static void AttachStdio() {
    // If stdout already goes somewhere useful (console the shell gave us via
    // handles, or a pipe/file redirect), leave it alone.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    // Launched by double-click or from a shell that passed no handles.
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
}

static void Pause() {
    std::wcout << L"\nDone. Press ENTER to exit.";
    std::wcout.flush();
    std::cin.get();
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

static void AppendLog(const std::wstring& msg) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(ProfileDir(), ec);

    // Cap the log: when it grows past 64 KiB, keep only the last 16 KiB.
    try {
        std::ifstream in(LogPath(), std::ios::binary | std::ios::ate);
        if (in) {
            std::streamoff size = in.tellg();
            if (size > 65536) {
                in.seekg(-16384, std::ios::end);
                std::string tail((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                in.close();
                std::ofstream out(LogPath(), std::ios::binary | std::ios::trunc);
                out << tail;
            }
        }
    } catch (...) {}

    std::ofstream out(LogPath(), std::ios::binary | std::ios::app);
    if (out) out << ToUtf8(L"[" + NowString() + L"] " + msg + L"\n");
}

// ---------------------------------------------------------------------------
// Profile (JSON)
// ---------------------------------------------------------------------------

struct Anchor {
    int pct = 0;
    int nits = 0; 
};

struct Profile {
    int version = 1;
    std::wstring deviceName;
    std::wstring edidHash;
    float basePeakNits = 0.0f;
    std::wstring calibratedAt;
    std::vector<Anchor> anchors;
};

static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c == L'\n') out += L"\\n";
        else if (c >= 0x20 || c == L'\t') out += c;
    }
    return out;
}

static bool SaveProfile(const Profile& p, const std::wstring& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::wstring j = L"{\n";
    j += L"  \"version\": " + std::to_wstring(p.version) + L",\n";
    j += L"  \"deviceName\": \"" + JsonEscape(p.deviceName) + L"\",\n";
    j += L"  \"edidHash\": \"" + JsonEscape(p.edidHash) + L"\",\n";
    wchar_t buf[64];
    swprintf(buf, 64, L"%.1f", p.basePeakNits);
    j += L"  \"basePeakNits\": " + std::wstring(buf) + L",\n";
    j += L"  \"calibratedAt\": \"" + JsonEscape(p.calibratedAt) + L"\",\n";
    j += L"  \"anchors\": [\n";
    for (size_t i = 0; i < p.anchors.size(); i++) {
        wchar_t row[128];
        swprintf(row, 128, L"    {\"pct\": %d, \"nits\": %d}%s\n",
                 p.anchors[i].pct, p.anchors[i].nits, i + 1 < p.anchors.size() ? L"," : L"");
        j += row;
    }
    j += L"  ]\n}\n";

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << ToUtf8(j);
    return (bool)out;
}

// Minimal JSON reader
struct JsonCursor {
    const wchar_t* p = nullptr;
    const wchar_t* end = nullptr;

    void SkipWs() { while (p < end && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')) p++; }
    bool Eat(wchar_t c) { SkipWs(); if (p < end && *p == c) { p++; return true; } return false; }
    bool Peek(wchar_t c) { SkipWs(); return p < end && *p == c; }

    bool ParseString(std::wstring* out) {
        if (!Eat(L'"')) return false;
        out->clear();
        while (p < end && *p != L'"') {
            if (*p == L'\\' && p + 1 < end) {
                p++;
                switch (*p) {
                    case L'"': case L'\\': case L'/': out->push_back(*p); break;
                    case L'n': out->push_back(L'\n'); break;
                    case L't': out->push_back(L'\t'); break;
                    default: out->push_back(*p); break;
                }
            } else {
                out->push_back(*p);
            }
            p++;
        }
        return Eat(L'"');
    }

    bool ParseNumber(double* out) {
        SkipWs();
        wchar_t* endp = nullptr;
        double v = wcstod(p, &endp);
        if (endp == p) return false;
        p = endp;
        *out = v;
        return true;
    }

    void SkipValue() {
        SkipWs();
        if (p >= end) return;
        if (*p == L'"') { std::wstring tmp; ParseString(&tmp); return; }
        if (*p == L'{' || *p == L'[') {
            wchar_t open = *p, close = (open == L'{') ? L'}' : L']';
            int depth = 0;
            while (p < end) {
                if (*p == L'"') { std::wstring tmp; ParseString(&tmp); continue; }
                if (*p == open) depth++;
                else if (*p == close) { depth--; p++; if (depth == 0) return; continue; }
                p++;
            }
            return;
        }
        while (p < end && *p != L',' && *p != L'}' && *p != L']') p++;
    }
};

static bool LoadProfile(const std::wstring& path, Profile* out, std::wstring* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = L"Profile not found at " + path + L". Run calibration first."; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    std::wstring text = FromUtf8(ss.str());

    JsonCursor c{text.data(), text.data() + text.size()};
    if (!c.Eat(L'{')) { if (error) *error = L"Profile is not valid JSON: " + path; return false; }

    Profile p;
    bool haveHash = false, haveAnchors = false;
    while (!c.Eat(L'}')) {
        std::wstring key;
        if (!c.ParseString(&key) || !c.Eat(L':')) { if (error) *error = L"Profile is corrupt: " + path; return false; }
        if (key == L"version") { double v; c.ParseNumber(&v); p.version = (int)v; }
        else if (key == L"deviceName") { c.ParseString(&p.deviceName); }
        else if (key == L"edidHash") { c.ParseString(&p.edidHash); haveHash = true; }
        else if (key == L"basePeakNits") { double v; c.ParseNumber(&v); p.basePeakNits = (float)v; }
        else if (key == L"calibratedAt") { c.ParseString(&p.calibratedAt); }
        else if (key == L"anchors") {
            if (!c.Eat(L'[')) { if (error) *error = L"Profile is corrupt: anchors not a list."; return false; }
            while (!c.Eat(L']')) {
                Anchor a;
                if (!c.Eat(L'{')) { if (error) *error = L"Profile is corrupt: bad anchor."; return false; }
                while (!c.Eat(L'}')) {
                    std::wstring k2;
                    if (!c.ParseString(&k2) || !c.Eat(L':')) { if (error) *error = L"Profile is corrupt: bad anchor."; return false; }
                    double v;
                    if (k2 == L"pct") { c.ParseNumber(&v); a.pct = (int)v; }
                    else if (k2 == L"nits") { c.ParseNumber(&v); a.nits = (int)v; }
                    else c.SkipValue();
                    if (c.Peek(L',')) c.Eat(L',');
                }
                p.anchors.push_back(a);
                if (c.Peek(L',')) c.Eat(L',');
            }
            haveAnchors = true;
        }
        else c.SkipValue();
        if (c.Peek(L',')) c.Eat(L',');
    }

    if (p.version != 1) { if (error) *error = L"Profile version " + std::to_wstring(p.version) + L" is not supported; re-run calibration."; return false; }
    if (!haveHash || p.edidHash.empty()) { if (error) *error = L"Profile has no panel identity; re-run calibration."; return false; }
    if (!haveAnchors || p.anchors.empty()) { if (error) *error = L"Profile has no calibration points; re-run calibration."; return false; }
    std::sort(p.anchors.begin(), p.anchors.end(), [](const Anchor& a, const Anchor& b) { return a.pct < b.pct; });
    *out = p;
    return true;
}

// Linear interpolation between the surrounding anchors, clamped at the ends.
static int InterpolateHdrNits(const std::vector<Anchor>& anchors, int pct) {
    if (anchors.empty()) return 0;
    if (pct <= anchors.front().pct) return anchors.front().nits;
    if (pct >= anchors.back().pct) return anchors.back().nits;
    for (size_t i = 1; i < anchors.size(); i++) {
        if (pct <= anchors[i].pct) {
            const Anchor& a = anchors[i - 1];
            const Anchor& b = anchors[i];
            double t = (pct - a.pct) / double(b.pct - a.pct);
            return (int)std::lround(a.nits + t * (b.nits - a.nits));
        }
    }
    return anchors.back().nits;
}

// ---------------------------------------------------------------------------
// DXGI
// ---------------------------------------------------------------------------

static bool GetFreshDXGIDesc(const wchar_t* preferGdiName, DXGI_OUTPUT_DESC1* outDesc) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

    DXGI_OUTPUT_DESC1 chosen = {};
    bool haveChosen = false;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6))) {
                DXGI_OUTPUT_DESC1 desc = {};
                if (SUCCEEDED(output6->GetDesc1(&desc))) {
                    bool isPreferred = preferGdiName && desc.DeviceName[0] && preferGdiName == desc.DeviceName;
                    bool isHdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    if (isPreferred || !haveChosen) {
                        bool better = isPreferred ||
                                      (!haveChosen && (isHdr || chosen.MaxLuminance <= 0.0f));
                        if (better) {
                            chosen = desc;
                            haveChosen = true;
                            if (isPreferred) { output6->Release(); output->Release(); adapter->Release(); factory->Release(); *outDesc = chosen; return true; }
                        }
                    }
                }
                output6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();

    if (haveChosen) *outDesc = chosen;
    return haveChosen;
}

// Poll until DXGI reports the same nonzero peak twice in a row so a single
// stale read after a slider change can't poison a probe.
static float MeasureStableMaxLuminance(const wchar_t* preferGdiName) {
    const DWORD start = GetTickCount();
    float prev = 0.0f, lastNonZero = 0.0f;
    for (;;) {
        DXGI_OUTPUT_DESC1 desc = {};
        float v = 0.0f;
        if (GetFreshDXGIDesc(preferGdiName, &desc)) v = desc.MaxLuminance;
        if (v > lastNonZero) lastNonZero = v;
        if (v > 0.0f && v == prev) return v;
        prev = v;
        if (GetTickCount() - start > 2500) return lastNonZero;
        Sleep(120);
    }
}

// ---------------------------------------------------------------------------
// Display paths (CCD)
// ---------------------------------------------------------------------------

static bool QueryActivePaths(std::vector<DISPLAYCONFIG_PATH_INFO>* paths,
                             std::vector<DISPLAYCONFIG_MODE_INFO>* modes) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    paths->assign(pathCount, {});
    modes->assign(modeCount, {});
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths->data(), &modeCount, modes->data(), nullptr) != ERROR_SUCCESS)
        return false;
    paths->resize(pathCount);
    modes->resize(modeCount);
    return !paths->empty();
}

// Bind a GDI device name (e.g. \\.\DISPLAY1) to its active display path
static bool FindPathForGdiDevice(const std::wstring& gdiName, LUID* adapterId, UINT32* targetId) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActivePaths(&paths, &modes)) return false;

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (gdiName == source.viewGdiDeviceName) {
            *adapterId = path.targetInfo.adapterId;
            *targetId = path.targetInfo.id;
            return true;
        }
    }
    return false;
}

static std::wstring GetTargetFriendlyName(LUID adapterId, UINT32 targetId) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = adapterId;
    target.header.id = targetId;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
        return target.monitorFriendlyDeviceName;
    return L"";
}

// ---------------------------------------------------------------------------
// EDID: base peak + panel identity
// ---------------------------------------------------------------------------

static std::wstring MonitorEnumPathFromDeviceId(const std::wstring& deviceId) {
    if (deviceId.rfind(L"\\\\?\\", 0) == 0) {
        std::wstring body = deviceId.substr(4);
        size_t guid = body.rfind(L"#");
        size_t guid2 = body.find(L"#{");
        std::wstring core = (guid2 != std::wstring::npos) ? body.substr(0, guid2) : body;
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (;;) {
            size_t pos = core.find(L'#', start);
            if (pos == std::wstring::npos) { parts.push_back(core.substr(start)); break; }
            parts.push_back(core.substr(start, pos - start));
            start = pos + 1;
        }
        if (parts.size() >= 3) return parts[0] + L"\\" + parts[1] + L"\\" + parts[2];
        return L"";
    }
    return deviceId;
}

static std::wstring ComputeEdidHash(const DXGI_OUTPUT_DESC1& desc) {
    DISPLAY_DEVICEW device = {};
    device.cb = sizeof(device);
    for (UINT i = 0; EnumDisplayDevicesW(desc.DeviceName, i, &device, EDD_GET_DEVICE_INTERFACE_NAME); i++) {
        if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;

        std::wstring enumPath = MonitorEnumPathFromDeviceId(device.DeviceID);
        if (!enumPath.empty()) {
            std::wstring regKey = L"SYSTEM\\CurrentControlSet\\Enum\\" + enumPath + L"\\Device Parameters";
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regKey.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
                DWORD size = 0;
                if (RegGetValueW(HKEY_LOCAL_MACHINE, regKey.c_str(), L"EDID", RRF_RT_REG_BINARY,
                                 nullptr, nullptr, &size) == ERROR_SUCCESS && size > 0 && size < 4096) {
                    std::vector<unsigned char> edid(size);
                    DWORD got = size;
                    if (RegGetValueW(HKEY_LOCAL_MACHINE, regKey.c_str(), L"EDID", RRF_RT_REG_BINARY,
                                     nullptr, edid.data(), &got) == ERROR_SUCCESS && got > 0) {
                        RegCloseKey(key);
                        return HashToHex(Fnv1a64(edid.data(), got));
                    }
                }
                RegCloseKey(key);
            }
        }
        std::string bytes = ToUtf8(device.DeviceID);
        return HashToHex(Fnv1a64((const unsigned char*)bytes.data(), bytes.size()));
    }
    return L"";
}

static bool GetBaseLuminanceFromEdid(const DXGI_OUTPUT_DESC1& desc, float* outNits, std::wstring* error) {
    DISPLAY_DEVICEW device = {};
    device.cb = sizeof(device);
    try {
        for (UINT deviceIndex = 0;
             EnumDisplayDevicesW(desc.DeviceName, deviceIndex, &device, EDD_GET_DEVICE_INTERFACE_NAME);
             deviceIndex++) {
            if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
            DisplayMonitor monitor = DisplayMonitor::FromInterfaceIdAsync(winrt::to_hstring(device.DeviceID)).get();
            if (monitor) {
                *outNits = monitor.MaxLuminanceInNits();
                if (*outNits > 0.0f) return true;
            }
        }
    } catch (winrt::hresult_error const& e) {
        if (error) *error = L"EDID lookup failed: " + std::wstring(e.message().c_str());
        return false;
    } catch (...) {
        if (error) *error = L"EDID lookup failed with an unexpected error.";
        return false;
    }
    if (error) *error = L"No active display device reported an EDID peak luminance.";
    return false;
}

// ---------------------------------------------------------------------------
// Display context: everything one calibration/apply pass needs
// ---------------------------------------------------------------------------

struct DisplayContext {
    DXGI_OUTPUT_DESC1 desc = {};
    float baseLevel = 0.0f;   // EDID hardware peak in nits
    LUID adapterId = {};
    UINT32 targetId = 0;      // target the SDR white level GET/SET applies to
    std::wstring edidHash;
    std::wstring monitorName;
};

static bool IsHdrActive(const DXGI_OUTPUT_DESC1& desc) {
    return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

static int InitDisplayContext(DisplayContext* ctx, bool quiet) {
    DXGI_OUTPUT_DESC1 desc = {};
    if (!GetFreshDXGIDesc(nullptr, &desc) || desc.MaxLuminance <= 0.0f) {
        if (!quiet) std::wcout << L"[-] Unable to query the display through DXGI.\n";
        return ExitDisplayError;
    }
    ctx->desc = desc;

    std::wstring edidError;
    if (!GetBaseLuminanceFromEdid(desc, &ctx->baseLevel, &edidError)) {
        if (!quiet) std::wcout << L"[-] Unable to proceed: " << edidError << L"\n";
        return ExitDisplayError;
    }

    if (!FindPathForGdiDevice(desc.DeviceName, &ctx->adapterId, &ctx->targetId)) {
        if (!quiet) std::wcout << L"[-] Could not match the measured display to an active display path.\n";
        return ExitDisplayError;
    }

    ctx->edidHash = ComputeEdidHash(desc);
    ctx->monitorName = GetTargetFriendlyName(ctx->adapterId, ctx->targetId);
    return ExitOk;
}

// ---------------------------------------------------------------------------
// SDR white level read / write
// ---------------------------------------------------------------------------

static bool GetCurrentSliderNits(LUID adapterId, UINT32 targetId, int* nits) {
    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(whiteLevel);
    whiteLevel.header.adapterId = adapterId;
    whiteLevel.header.id = targetId;

    if (DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS) return false;
    *nits = LevelToNits(whiteLevel.SDRWhiteLevel);
    return true;
}

static LONG WriteSDRWhiteLevel(LUID adapterId, UINT32 targetId, int nits) {
    _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL sdrWhiteParams = {};
    sdrWhiteParams.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL;
    sdrWhiteParams.header.size = sizeof(sdrWhiteParams);
    sdrWhiteParams.header.adapterId = adapterId;
    sdrWhiteParams.header.id = targetId;
    sdrWhiteParams.SDRWhiteLevel = NitsToLevel(nits);
    sdrWhiteParams.finalValue = 1;

    return DisplayConfigSetDeviceInfo(&sdrWhiteParams.header);
}

static bool WriteAndVerify(const DisplayContext& ctx, int nits, int* appliedNits) {
    for (int attempt = 0; attempt < 2; attempt++) {
        LONG hr = WriteSDRWhiteLevel(ctx.adapterId, ctx.targetId, nits);
        if (hr != ERROR_SUCCESS) {
            std::wcout << L"  [!] Slider write failed (error " << hr << L")\n";
            continue;
        }
        int read = 0;
        if (GetCurrentSliderNits(ctx.adapterId, ctx.targetId, &read) && std::abs(read - nits) <= 1) {
            if (appliedNits) *appliedNits = read;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// System brightness via WMI (root\WMI)
// ---------------------------------------------------------------------------

class BrightnessWmi {
public:
    bool Ensure() {
        if (services_) return true;
        IWbemLocator* locator = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, (void**)&locator)))
            return false;
        IWbemServices* services = nullptr;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
        SysFreeString(ns);
        locator->Release();
        if (FAILED(hr)) return false;
        if (FAILED(CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                     RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                                     nullptr, EOAC_NONE))) {
            services->Release();
            return false;
        }
        services_ = services;
        return true;
    }

    static bool VariantToInt(const VARIANT& v, int* out) {
        switch (v.vt) {
            case VT_I2: *out = v.iVal; return true;
            case VT_I4: *out = v.lVal; return true;
            case VT_UI1: *out = v.bVal; return true;
            case VT_UI4: *out = (int)v.ulVal; return true;
            default: return false;
        }
    }

    bool GetBrightnessPct(int* pct) {
        if (!Ensure()) return false;
        IEnumWbemClassObject* en = QueryObjects(L"WQL", L"SELECT CurrentBrightness FROM WmiMonitorBrightness", false);
        if (!en) return false;
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        bool ok = false;
        if (en->Next(WBEM_INFINITE, 1, &obj, &got) == S_OK && got == 1) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) {
                ok = VariantToInt(v, pct);
                VariantClear(&v);
            }
            obj->Release();
        }
        en->Release();
        return ok;
    }

    bool SetBrightnessPct(int pct) {
        if (!Ensure()) return false;

        IEnumWbemClassObject* en = QueryObjects(L"WQL", L"SELECT __PATH FROM WmiMonitorBrightnessMethods", false);
        if (!en) return false;
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        std::wstring instancePath;
        if (en->Next(WBEM_INFINITE, 1, &obj, &got) == S_OK && got == 1) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"__PATH", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
                instancePath = v.bstrVal;
            VariantClear(&v);
            obj->Release();
        }
        en->Release();
        if (instancePath.empty()) return false;

        IWbemClassObject* cls = nullptr;
        if (FAILED(services_->GetObject(BSTR(L"WmiMonitorBrightnessMethods"), 0, nullptr, &cls, nullptr)))
            return false;
        IWbemClassObject* inDef = nullptr;
        HRESULT hr = cls->GetMethod(BSTR(L"WmiSetBrightness"), 0, &inDef, nullptr);
        cls->Release();
        if (FAILED(hr)) return false;

        IWbemClassObject* inInst = nullptr;
        if (FAILED(inDef->SpawnInstance(0, &inInst))) { inDef->Release(); return false; }
        inDef->Release();

        VARIANT t, b;
        VariantInit(&t); VariantInit(&b);
        t.vt = VT_I4; t.lVal = 0;
        b.vt = VT_I4; b.lVal = pct;
        hr = inInst->Put(BSTR(L"Timeout"), 0, &t, 0);
        if (SUCCEEDED(hr)) hr = inInst->Put(BSTR(L"Brightness"), 0, &b, 0);
        if (SUCCEEDED(hr)) {
            IWbemClassObject* outParams = nullptr;
            hr = services_->ExecMethod(BSTR(instancePath.c_str()), BSTR(L"WmiSetBrightness"),
                                       0, nullptr, inInst, &outParams, nullptr);
            if (outParams) outParams->Release();
        }
        inInst->Release();
        return SUCCEEDED(hr);
    }

    IEnumWbemClassObject* SubscribeBrightnessEvent() {
        if (!Ensure()) return nullptr;
        return QueryObjects(L"WQL", L"SELECT * FROM WmiMonitorBrightnessEvent", true);
    }

private:
    IEnumWbemClassObject* QueryObjects(const wchar_t* lang, const wchar_t* query, bool notification) {
        BSTR langB = SysAllocString(lang);
        BSTR queryB = SysAllocString(query);
        IEnumWbemClassObject* en = nullptr;
        HRESULT hr = notification
            ? services_->ExecNotificationQuery(langB, queryB,
                                               WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en)
            : services_->ExecQuery(langB, queryB,
                                   WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(langB);
        SysFreeString(queryB);
        return SUCCEEDED(hr) ? en : nullptr;
    }

    IWbemServices* services_ = nullptr;
};

// ---------------------------------------------------------------------------
// System brightness backends
// ---------------------------------------------------------------------------
//
// Two ways to read/set the laptop's system brightness:
//  1. WMI (root\WMI, WmiMonitorBrightness) — works on most laptops, and its
//     event class gives the watcher instant change notifications. On hybrid
//     GPU laptops the provider is often not wired up (0x8004100C).
//  2. Power policy (powrprof) — the power-plan "display brightness" value,
//     which is what the native brightness slider writes. Works even where the
//     WMI provider is broken. Requires PowerSetActiveScheme re-application to
//     take effect immediately. No change events exist, so the watcher polls.

static const GUID kGuidVideoSubgroup = {0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
static const GUID kGuidVideoBrightness = {0xfbd9aa66, 0x9553, 0x4097, {0xba, 0x44, 0xed, 0x6e, 0x9d, 0x65, 0xea, 0xb8}};

static bool PowerGetBrightnessPct(int* pct) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    SYSTEM_POWER_STATUS sps;
    GetSystemPowerStatus(&sps);
    DWORD idx = 0;
    DWORD hr = (sps.ACLineStatus == 1)
        ? PowerReadACValueIndex(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, &idx)
        : PowerReadDCValueIndex(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, &idx);
    LocalFree(scheme);
    if (hr != ERROR_SUCCESS || idx > 100) return false;
    *pct = (int)idx;
    return true;
}

static bool PowerSetBrightnessPct(int pct) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
    SYSTEM_POWER_STATUS sps;
    GetSystemPowerStatus(&sps);
    DWORD hr = (sps.ACLineStatus == 1)
        ? PowerWriteACValueIndex(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, (DWORD)pct)
        : PowerWriteDCValueIndex(nullptr, scheme, &kGuidVideoSubgroup, &kGuidVideoBrightness, (DWORD)pct);
    if (hr == ERROR_SUCCESS)
        hr = PowerSetActiveScheme(nullptr, scheme);  // flush: applies the new value now
    LocalFree(scheme);
    return hr == ERROR_SUCCESS;
}

class Brightness {
public:
    bool GetPct(int* pct) {
        if (wmiOk_ && wmi_.GetBrightnessPct(pct)) return true;
        wmiOk_ = false;
        return PowerGetBrightnessPct(pct);
    }
    bool SetPct(int pct) {
        if (wmiOk_ && wmi_.SetBrightnessPct(pct)) return true;
        wmiOk_ = false;
        return PowerSetBrightnessPct(pct);
    }
    IEnumWbemClassObject* SubscribeBrightnessEvent() { return wmi_.SubscribeBrightnessEvent(); }

private:
    BrightnessWmi wmi_;
    bool wmiOk_ = true;
};

static bool WaitBrightness(Brightness& brightness, int targetPct) {
    for (int i = 0; i < 20; i++) {
        int v = -1;
        if (brightness.GetPct(&v) && v == targetPct) return true;
        Sleep(100);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Calibration search
// ---------------------------------------------------------------------------

struct BisectResult {
    bool failed = false;
    int bestNits = 0;
    float measured = 0.0f;   // adjusted peak at bestNits
    const wchar_t* note = nullptr;  // floor/ceiling notice, if any
};

static BisectResult BisectAtCurrentBrightness(const DisplayContext& ctx) {
    BisectResult r;
    const wchar_t* gdi = ctx.desc.DeviceName;
    float base = ctx.baseLevel;

    auto probe = [&](int nits) {
        int applied = 0;
        if (!WriteAndVerify(ctx, nits, &applied)) return 0.0f;
        Sleep(150);
        float measured = MeasureStableMaxLuminance(gdi);
        std::wcout << L"  Testing Slider: " << nits
                   << L" Nits | Adjusted Level: " << measured
                   << L" Nits | Factor: " << (measured > 0.0f ? base / measured : 0.0f) << L"\n";
        return measured;
    };

    int lo = kMinNits, hi = kMaxNits;
    float loVal = probe(lo), hiVal = probe(hi);
    if (loVal <= 0.0f || hiVal <= 0.0f) {
        r.failed = true;
        return r;
    }

    if (loVal >= base) {
        r.bestNits = lo;
        r.measured = loVal;
        r.note = L"already at or above the target at the minimum slider value";
        return r;
    }
    if (hiVal < base) {
        r.bestNits = hi;
        r.measured = hiVal;
        r.note = L"slider ceiling reached before matching the hardware peak";
        return r;
    }

    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        float measured = probe(mid);
        if (measured <= 0.0f) {
            r.failed = true;
            return r;
        }
        if (measured < base) { lo = mid; loVal = measured; }
        else                 { hi = mid; hiVal = measured; }
    }
    r.bestNits = (std::abs(loVal - base) <= std::abs(hiVal - base)) ? lo : hi;
    r.measured = (r.bestNits == lo) ? loVal : hiVal;
    return r;
}

// ---------------------------------------------------------------------------
// Modes: map / quick / apply
// ---------------------------------------------------------------------------

static void PrintHeader(const DisplayContext& ctx) {
    int current = 0;
    GetCurrentSliderNits(ctx.adapterId, ctx.targetId, &current);
    std::wcout << L"[Base Level Found] (Hardware Peak): " << ctx.baseLevel << L" Nits\n";
    std::wcout << L"[Panel]                            : " << ctx.monitorName << L"\n";
    std::wcout << L"[Current HDR Content Brightness]   : " << current << L" Nits\n";
    std::wcout << L"[*] Initializing calibration...\n\n";
}

static int CheckHdrForCalibration(const DisplayContext& ctx) {
    if (IsHdrActive(ctx.desc)) return ExitOk;
    std::wcout << L"[-] HDR is not enabled on the internal display.\n";
    std::wcout << L"    Turn HDR on in Settings > System > Display and run this again.\n";
    return ExitHdrOff;
}

// --quick: calibrate for the current system brightness only, no profile.
static int RunQuick() {
    DisplayContext ctx;
    int rc = InitDisplayContext(&ctx, false);
    if (rc) return rc;
    rc = CheckHdrForCalibration(ctx);
    if (rc) return rc;

    PrintHeader(ctx);

    BisectResult r = BisectAtCurrentBrightness(ctx);
    if (r.failed) {
        std::wcout << L"\n[-] DXGI stopped reporting a luminance value. Aborting.\n";
        return ExitGeneric;
    }
    if (r.note) std::wcout << L"\n[!] " << r.note << L".\n";
    else std::wcout << L"\n[*] Optimal value identified for current system brightness.\n";

    int applied = 0;
    if (!WriteAndVerify(ctx, r.bestNits, &applied)) {
        std::wcout << L"[-] Failed to set the final slider value.\n";
        return ExitWriteFailed;
    }
    Sleep(150);
    float finalAdjusted = MeasureStableMaxLuminance(ctx.desc.DeviceName);

    std::wcout << L"\n[*] Alignment Step Complete.\n";
    std::wcout << L"    -> Selected Slider Setting: " << applied << L" Nits\n";
    std::wcout << L"    -> Base Level: " << ctx.baseLevel << L" Nits\n";
    std::wcout << L"    -> Final Adjusted Level: " << finalAdjusted << L" Nits\n";
    std::wcout << L"    -> Final Slider Factor: "
               << (finalAdjusted > 0.0f ? ctx.baseLevel / finalAdjusted : 0.0f) << L"\n";
    std::wcout << L"    In games that ask for a peak brightness, use the FINAL ADJUSTED VALUE above.\n";
    return ExitOk;
}

// --map (default): calibrate the whole brightness range and save the profile.
static int RunMap() {
    DisplayContext ctx;
    int rc = InitDisplayContext(&ctx, false);
    if (rc) return rc;
    rc = CheckHdrForCalibration(ctx);
    if (rc) return rc;

    Brightness brightness;
    int originalPct = 0;
    if (!brightness.GetPct(&originalPct)) {
        std::wcout << L"[-] Could not read the system brightness (tried WMI and the power-policy backlight).\n";
        std::wcout << L"    Mapping the brightness range requires working brightness controls.\n";
        return ExitGeneric;
    }

    PrintHeader(ctx);
    std::wcout << L"[Current System Brightness]        : " << originalPct << L"%\n";
    std::wcout << L"[*] Calibrating at system brightness anchors: 10/30/50/70/90%.\n\n";

    Profile profile;
    profile.deviceName = ctx.monitorName;
    profile.edidHash = ctx.edidHash;
    profile.basePeakNits = ctx.baseLevel;
    profile.calibratedAt = NowString();

    bool brightnessChanged = false;
    for (int pct : kAnchorPcts) {
        if (!brightness.SetPct(pct) || !WaitBrightness(brightness, pct)) {
            std::wcout << L"[-] Could not set system brightness to " << pct << L"%. Aborting.\n";
            rc = ExitGeneric;
            break;
        }
        brightnessChanged = true;
        Sleep(300);  // panel transition
        std::wcout << L"[System Brightness " << pct << L"%]\n";
        BisectResult r = BisectAtCurrentBrightness(ctx);
        if (r.failed) {
            std::wcout << L"[-] Measurement failed at " << pct << L"% brightness. Aborting.\n";
            rc = ExitGeneric;
            break;
        }
        profile.anchors.push_back({pct, r.bestNits});
        std::wcout << L"  -> HDR content brightness: " << r.bestNits << L" nits"
                   << L" (adjusted peak " << r.measured << L")";
        if (r.note) std::wcout << L" [" << r.note << L"]";
        std::wcout << L"\n\n";
    }

    if (brightnessChanged) {
        brightness.SetPct(originalPct);
        WaitBrightness(brightness, originalPct);
    }
    if (rc) return rc;

    if (!SaveProfile(profile, ProfilePath())) {
        std::wcout << L"[-] Failed to write the profile to " << ProfilePath() << L"\n";
        return ExitGeneric;
    }

    // Leave the system at the best pair for the user's current brightness.
    int nits = InterpolateHdrNits(profile.anchors, originalPct);
    int applied = 0;
    if (WriteAndVerify(ctx, nits, &applied)) {
        std::wcout << L"[*] Applied HDR content brightness for current system brightness ("
                   << originalPct << L"%): " << applied << L" nits\n";
    } else {
        std::wcout << L"[!] Profile saved, but applying the value for the current brightness failed.\n";
        std::wcout << L"    Run --apply to retry.\n";
        return ExitWriteFailed;
    }

    std::wcout << L"\n[+] Profile saved: " << ProfilePath() << L"\n";
    std::wcout << L"    Your accurate combo: system brightness " << originalPct
               << L"% + HDR content brightness " << applied << L" nits.\n";
    std::wcout << L"    Install the watcher (hdr-laptop-calibration.exe --install) to keep HDR\n";
    std::wcout << L"    aligned automatically whenever the system brightness changes.\n";
    return ExitOk;
}

// --apply: load the profile and apply the value for the current brightness.
static int RunApply() {
    Profile profile;
    std::wstring error;
    if (!LoadProfile(ProfilePath(), &profile, &error)) {
        std::wcout << L"[-] " << error << L"\n";
        return ExitProfileInvalid;
    }

    DisplayContext ctx;
    int rc = InitDisplayContext(&ctx, false);
    if (rc) return rc;

    if (ctx.edidHash != profile.edidHash) {
        std::wcout << L"[-] Profile was calibrated for a different panel (EDID mismatch).\n";
        std::wcout << L"    Run calibration again.\n";
        return ExitProfileInvalid;
    }
    if (!IsHdrActive(ctx.desc)) {
        std::wcout << L"[*] HDR is off — nothing applied.\n";
        return ExitOk;
    }

    Brightness brightness;
    int pct = 0;
    if (!brightness.GetPct(&pct)) {
        std::wcout << L"[-] Could not read the system brightness.\n";
        return ExitGeneric;
    }

    int nits = InterpolateHdrNits(profile.anchors, pct);
    int applied = 0;
    if (!WriteAndVerify(ctx, nits, &applied)) {
        std::wcout << L"[-] Failed to set the HDR content brightness.\n";
        return ExitWriteFailed;
    }
    std::wcout << L"[+] System brightness " << pct << L"% -> HDR content brightness "
               << applied << L" nits (panel peak " << profile.basePeakNits << L" nits).\n";
    return ExitOk;
}

// ---------------------------------------------------------------------------
// Watcher
// ---------------------------------------------------------------------------

struct WatchState {
    Profile profile;
    DisplayContext ctx;
    Brightness brightness;      // used on the main (message) thread
    HWND hwnd = nullptr;
    int lastBrightnessPct = -1;
};

static WatchState* g_watch = nullptr;

// Applies the profile value for the current system brightness. Runs on the
// message thread; all failures are logged, never fatal.
static void ApplyFromProfile(const wchar_t* reason) {
    WatchState& s = *g_watch;

    DXGI_OUTPUT_DESC1 desc = {};
    if (GetFreshDXGIDesc(s.ctx.desc.DeviceName, &desc) && !IsHdrActive(desc)) {
        AppendLog(std::wstring(L"[apply] skipped (") + reason + L"): HDR is off.");
        return;
    }

    int pct = 0;
    if (!s.brightness.GetPct(&pct)) {
        AppendLog(L"[apply] failed: could not read system brightness.");
        return;
    }

    int nits = InterpolateHdrNits(s.profile.anchors, pct);
    int applied = 0;
    if (!WriteAndVerify(s.ctx, nits, &applied)) {
        AppendLog(L"[apply] FAILED: slider write did not land (target " + std::to_wstring(nits) + L" nits).");
        return;
    }
    s.lastBrightnessPct = pct;
    AppendLog(L"[apply] system brightness " + std::to_wstring(pct) + L"% -> HDR content brightness "
              + std::to_wstring(applied) + L" nits (verified) [" + reason + L"]");
}

// Rebuild the display context after resume/topology change and re-apply once.
static void RevalidateAndApply() {
    WatchState& s = *g_watch;
    DisplayContext fresh;
    if (InitDisplayContext(&fresh, true) != ExitOk) {
        AppendLog(L"[watch] display context unavailable after topology/resume change; keeping old binding.");
        return;
    }
    if (fresh.edidHash != s.profile.edidHash) {
        AppendLog(L"[watch] panel identity changed (EDID mismatch). Exiting.");
        PostQuitMessage(ExitProfileInvalid);
        return;
    }
    s.ctx = fresh;
    ApplyFromProfile(L"resume/topology");
}

static DWORD WINAPI WatchEventThread(LPVOID) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        PostMessage(g_watch->hwnd, WM_APP_WMI_FAILED, 0, 0);
        return 0;
    }

    BrightnessWmi wmi;
    IEnumWbemClassObject* en = wmi.SubscribeBrightnessEvent();
    if (!en) {
        PostMessage(g_watch->hwnd, WM_APP_WMI_FAILED, 0, 0);
        CoUninitialize();
        return 0;
    }

    for (;;) {
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        if (en->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0) {
            PostMessage(g_watch->hwnd, WM_APP_WMI_FAILED, 0, 0);
            break;
        }
        if (obj) obj->Release();
        PostMessage(g_watch->hwnd, WM_APP_BRIGHTNESS, 0, 0);
    }
    en->Release();
    CoUninitialize();
    return 0;
}

static LRESULT CALLBACK WatchWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WatchState& s = *g_watch;
    switch (msg) {
        case WM_APP_BRIGHTNESS:
            // Restart the debounce timer: apply only after the keys go quiet.
            SetTimer(hwnd, 1, kDebounceMs, nullptr);
            return 0;
        case WM_APP_WMI_FAILED:
            AppendLog(L"[watch] brightness events unavailable; relying on the periodic poll.");
            return 0;
        case WM_APP_REAPPLY:
            RevalidateAndApply();
            return 0;
        case WM_TIMER:
            if (wParam == 1) {
                KillTimer(hwnd, 1);
                ApplyFromProfile(L"brightness change");
            } else if (wParam == 2) {
                int pct = -1;
                if (s.brightness.GetPct(&pct) && pct != s.lastBrightnessPct)
                    SetTimer(hwnd, 1, kDebounceMs, nullptr);
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
                PostMessage(hwnd, WM_APP_REAPPLY, 0, 0);
            return TRUE;
        case WM_DISPLAYCHANGE:
            PostMessage(hwnd, WM_APP_REAPPLY, 0, 0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static int RunWatch() {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        AppendLog(L"[watch] another instance is already running; exiting.");
        return ExitOk;
    }

    std::wstring error;
    Profile profile;
    if (!LoadProfile(ProfilePath(), &profile, &error)) {
        AppendLog(L"[watch] " + error + L" Exiting.");
        return ExitProfileInvalid;
    }

    DisplayContext ctx;
    if (InitDisplayContext(&ctx, true) != ExitOk) {
        AppendLog(L"[watch] could not initialize the display context. Exiting.");
        return ExitDisplayError;
    }
    if (ctx.edidHash != profile.edidHash) {
        AppendLog(L"[watch] profile is calibrated for a different panel; run calibration. Exiting.");
        return ExitProfileInvalid;
    }

    static WatchState state;
    state.profile = profile;
    state.ctx = ctx;
    g_watch = &state;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WatchWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWatchWndClass;
    RegisterClassW(&wc);

    state.hwnd = CreateWindowExW(0, kWatchWndClass, L"LaptopTrueHDR watcher", 0,
                                 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!state.hwnd) {
        AppendLog(L"[watch] could not create the message window. Exiting.");
        return ExitGeneric;
    }

    HANDLE thread = CreateThread(nullptr, 0, WatchEventThread, nullptr, 0, nullptr);
    if (!thread) PostMessage(state.hwnd, WM_APP_WMI_FAILED, 0, 0);

    SetTimer(state.hwnd, 2, kPollFallbackMs, nullptr);

    AppendLog(L"[watch] started. Panel peak " + std::to_wstring((int)(profile.basePeakNits + 0.5f))
              + L" nits, " + std::to_wstring(profile.anchors.size()) + L" calibration anchors.");

    ApplyFromProfile(L"startup");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}

// ---------------------------------------------------------------------------
// Autostart + status + help
// ---------------------------------------------------------------------------

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L"";
}

static bool IsWatcherRunning();

static int RunInstall() {
    std::wstring exe = ExePath();
    if (exe.empty()) {
        std::wcout << L"[-] Could not determine the executable path.\n";
        return ExitGeneric;
    }
    std::wstring value = L"\"" + exe + L"\" --watch";
    LONG hr = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValueName, REG_SZ,
                              value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    if (hr != ERROR_SUCCESS) {
        std::wcout << L"[-] Failed to write the Run key (error " << hr << L").\n";
        return ExitGeneric;
    }
    std::wcout << L"[+] Watcher installed. It will start at logon:\n    " << value << L"\n";

    if (IsWatcherRunning()) {
        std::wcout << L"[*] Watcher is already running.\n";
        return ExitOk;
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmdline = L"\"" + exe + L"\" --watch";
    if (CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::wcout << L"[+] Watcher started and watching.\n";
    } else {
        std::wcout << L"[!] Could not start the watcher now (error " << GetLastError() << L").\n";
        std::wcout << L"    It will start at your next logon.\n";
    }
    return ExitOk;
}

static int RunUninstall() {
    LONG hr = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValueName);
    if (hr == ERROR_FILE_NOT_FOUND) {
        std::wcout << L"[*] Watcher was not installed.\n";
        return ExitOk;
    }
    if (hr != ERROR_SUCCESS) {
        std::wcout << L"[-] Failed to delete the Run key (error " << hr << L").\n";
        return ExitGeneric;
    }
    std::wcout << L"[+] Watcher autostart removed. A running watcher keeps running until you log off\n";
    std::wcout << L"    (or kill hdr-laptop-calibration.exe in Task Manager).\n";
    return ExitOk;
}

static bool IsWatcherRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!m) return false;
    CloseHandle(m);
    return true;
}

static bool GetRunKeyValue(std::wstring* value) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    LONG hr = RegGetValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValueName, RRF_RT_REG_SZ,
                           nullptr, buf, &size);
    if (hr != ERROR_SUCCESS) return false;
    *value = buf;
    return true;
}

static std::wstring LastAppliedFromLog() {
    std::ifstream in(LogPath(), std::ios::binary);
    if (!in) return L"";
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring wide = FromUtf8(text);
    std::wstring last;
    size_t pos = 0;
    while ((pos = wide.find(L"[apply]", pos)) != std::wstring::npos) {
        size_t end = wide.find(L'\n', pos);
        last = wide.substr(pos, (end == std::wstring::npos ? wide.size() : end) - pos);
        pos = end;
    }
    return last;
}

static int RunStatus() {
    std::wstring error;
    Profile profile;
    std::wstring path = ProfilePath();
    if (LoadProfile(path, &profile, &error)) {
        std::wcout << L"Profile    : VALID (" << profile.calibratedAt << L", panel peak "
                   << profile.basePeakNits << L" nits, " << profile.anchors.size() << L" anchors)\n";
        std::wcout << L"             " << path << L"\n";
        for (const Anchor& a : profile.anchors)
            std::wcout << L"             system " << a.pct << L"% -> HDR " << a.nits << L" nits\n";
    } else {
        std::wcout << L"Profile    : INVALID — " << error << L"\n";
    }

    DisplayContext ctx;
    if (InitDisplayContext(&ctx, true) == ExitOk) {
        std::wcout << L"Panel      : " << ctx.monitorName << L" (EDID hash " << ctx.edidHash << L")";
        if (LoadProfile(path, &profile, &error))
            std::wcout << (ctx.edidHash == profile.edidHash ? L" — matches profile" : L" — MISMATCH with profile");
        std::wcout << L"\n";
    } else {
        std::wcout << L"Panel      : unavailable\n";
    }

    std::wstring runValue;
    if (GetRunKeyValue(&runValue)) {
        std::wcout << L"Autostart  : installed — " << runValue << L"\n";
    } else {
        std::wcout << L"Autostart  : not installed (use --install)\n";
    }

    std::wcout << L"Watcher    : " << (IsWatcherRunning() ? L"running" : L"not running") << L"\n";

    std::wstring last = LastAppliedFromLog();
    if (!last.empty()) std::wcout << L"Last apply : " << last << L"\n";
    return ExitOk;
}

static int RunHelp() {
    std::wcout << L"LaptopTrueHDR — automatic HDR calibration for laptop internal displays.\n\n";
    std::wcout << L"Usage: hdr-laptop-calibration.exe [mode]\n\n";
    std::wcout << L"  (no args) / --map   Calibrate the whole brightness range (10-90% anchors),\n";
    std::wcout << L"                      save the profile, apply the value for the current\n";
    std::wcout << L"                      system brightness. Takes about a minute; your screen\n";
    std::wcout << L"                      will step through brightness levels and return.\n";
    std::wcout << L"  --quick             Calibrate only for the current system brightness.\n";
    std::wcout << L"  --apply             Apply the saved profile for the current brightness.\n";
    std::wcout << L"  --watch             Run the background watcher (re-applies HDR brightness\n";
    std::wcout << L"                      when system brightness changes).\n";
    std::wcout << L"  --install           Start the watcher now and automatically at logon.\n";
    std::wcout << L"  --uninstall         Remove the autostart entry.\n";
    std::wcout << L"  --status            Show profile, panel, autostart and watcher state.\n";
    return ExitOk;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
    _wsetlocale(LC_ALL, L"");
    std::wcout << std::unitbuf;  // keep wcout/wcerr ordering sane when piped

    std::wstring mode = L"map";
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg.rfind(L"--", 0) == 0) mode = arg.substr(2);
        else mode = arg;
    }

    if (mode != L"watch") AttachStdio();

    if (!SelfCheck()) {
        std::wcout << L"[-] Internal unit-conversion self-check failed.\n";
        return ExitGeneric;
    }

    // COM for the main thread. WMI needs impersonation; WinRT reuses the MTA.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);  // RPC_E_TOO_LATE is fine
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    int rc = ExitGeneric;
    if (mode == L"map")          rc = RunMap();
    else if (mode == L"quick")   rc = RunQuick();
    else if (mode == L"apply")   rc = RunApply();
    else if (mode == L"watch")   rc = RunWatch();
    else if (mode == L"install") rc = RunInstall();
    else if (mode == L"uninstall") rc = RunUninstall();
    else if (mode == L"status")  rc = RunStatus();
    else if (mode == L"help" || mode == L"h") rc = RunHelp();
    else {
        std::wcout << L"[-] Unknown mode '" << mode << L"'.\n\n";
        RunHelp();
        return ExitGeneric;
    }

    if (mode == L"map" || mode == L"quick") Pause();
    return rc;
}
