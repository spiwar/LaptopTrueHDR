#include <Windows.h>
#include <dxgi1_6.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>

// WinRT headers for accessing raw hardware metrics
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Display.h>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using namespace winrt::Windows::Devices::Display;

// Undocumented DISPLAYCONFIG_DEVICE_INFO_TYPE that writes the SDR white level.
constexpr auto DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL = (DISPLAYCONFIG_DEVICE_INFO_TYPE)0xFFFFFFEE;

typedef struct _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    unsigned int SDRWhiteLevel;
    unsigned char finalValue;
} _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL;

// The white level is carried in thousandths of 80 nits (1000 == 80 nits). Both
// directions round: truncating drops the .5 on odd nits and the slider value
// stops surviving a write/read round trip.
static int LevelToNits(unsigned int level) { return (int)((level * 80 + 500) / 1000); }
static unsigned int NitsToLevel(int nits) { return (unsigned int)((nits * 1000 + 40) / 80); }

static void SelfCheck() {
    for (int n = 80; n <= 400; n++) assert(LevelToNits(NitsToLevel(n)) == n);
}

int GetCurrentSliderNits(LUID adapterId, UINT32 targetId) {
    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(whiteLevel);
    whiteLevel.header.adapterId = adapterId;
    whiteLevel.header.id = targetId;

    if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS) {
        return LevelToNits(whiteLevel.SDRWhiteLevel);
    }
    return 80;
}

LONG WriteSDRWhiteLevel(LUID adapterId, UINT32 targetId, int nits) {
    _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL sdrWhiteParams = {};
    sdrWhiteParams.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL;
    sdrWhiteParams.header.size = sizeof(sdrWhiteParams);
    sdrWhiteParams.header.adapterId = adapterId;
    sdrWhiteParams.header.id = targetId;
    sdrWhiteParams.SDRWhiteLevel = NitsToLevel(nits);
    sdrWhiteParams.finalValue = 1;

    return DisplayConfigSetDeviceInfo(&sdrWhiteParams.header);
}

// Returns adapter 0 / output 0's max luminance, 0.0f on failure. When outDesc is
// given it also receives the full descriptor.
float GetFreshDXGIMaxLuminance(DXGI_OUTPUT_DESC1* outDesc = nullptr) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return 0.0f;

    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) {
        factory->Release();
        return 0.0f;
    }

    IDXGIOutput* output = nullptr;
    if (FAILED(adapter->EnumOutputs(0, &output))) {
        adapter->Release();
        factory->Release();
        return 0.0f;
    }

    IDXGIOutput6* output6 = nullptr;
    float maxLumi = 0.0f;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6))) {
        DXGI_OUTPUT_DESC1 desc = {};
        if (SUCCEEDED(output6->GetDesc1(&desc))) {
            maxLumi = desc.MaxLuminance;
            if (outDesc) *outDesc = desc;
        }
        output6->Release();
    }

    output->Release();
    adapter->Release();
    factory->Release();
    return maxLumi;
}

void AlignLevelsToMatch() {
    winrt::init_apartment();

    DXGI_OUTPUT_DESC1 outputDesc = {};
    if (GetFreshDXGIMaxLuminance(&outputDesc) <= 0.0f) {
        std::wcerr << L"[-] Unable to query the display through DXGI.\n";
        return;
    }

    float baseLevel = 0.0f;
    try {
        DISPLAY_DEVICEW device = {};
        device.cb = sizeof(device);

        for (UINT deviceIndex = 0; EnumDisplayDevicesW(outputDesc.DeviceName, deviceIndex, &device, EDD_GET_DEVICE_INTERFACE_NAME); deviceIndex++) {
            if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
            DisplayMonitor monitor = DisplayMonitor::FromInterfaceIdAsync(winrt::to_hstring(device.DeviceID)).get();
            if (monitor) {
                baseLevel = monitor.MaxLuminanceInNits();
                break;
            }
        }
    } catch (...) {}

    if (baseLevel <= 0.0f) {
        std::wcerr << L"[-] Unable to proceed: Base hardware level reported as 0 nits.\n";
        return;
    }

    UINT32 pathCount = 0, modeCount = 0;
    GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (paths.empty()) {
        std::wcerr << L"[-] No active display paths returned.\n";
        return;
    }

    LUID adapterId = paths[0].targetInfo.adapterId;
    UINT32 targetId = paths[0].targetInfo.id;

    std::wcout << L"[Base Level Found] (Hardware Peak): " << baseLevel << L" Nits\n";
    std::wcout << L"[Current System State]           : " << GetCurrentSliderNits(adapterId, targetId) << L" Nits\n";
    std::wcout << L"[*] Initializing calibration...\n\n";

    auto probe = [&](int nits) {
        WriteSDRWhiteLevel(adapterId, targetId, nits);
        Sleep(250);
        float measured = GetFreshDXGIMaxLuminance();
        std::wcout << L"  Testing Slider: " << nits
                   << L" Nits | Adjusted Level: " << measured
                   << L" Nits | Factor: " << (measured > 0.0f ? baseLevel / measured : 0.0f) << L"\n";
        return measured;
    };

    // The adjusted level rises with the slider, so bisect on (adjusted < base).
    // ~10 probes over the whole 80..400 range instead of a stride walk.
    int lo = 80, hi = 400;
    float loVal = probe(lo), hiVal = probe(hi);
    if (loVal <= 0.0f || hiVal <= 0.0f) {
        std::wcerr << L"\n[-] DXGI stopped reporting a luminance value. Aborting.\n";
        return;
    }

    int bestNits;
    if (loVal >= baseLevel) {
        bestNits = lo;
        std::wcout << L"\n[!] Already at or above the target at 80 nits. Increase your system brightness.\n";
    } else if (hiVal < baseLevel) {
        bestNits = hi;
        std::wcout << L"\n[!] 400 nit ceiling reached before matching the hardware peak.\n";
    } else {
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            float measured = probe(mid);
            if (measured <= 0.0f) break;
            if (measured < baseLevel) { lo = mid; loVal = measured; }
            else                      { hi = mid; hiVal = measured; }
        }
        bestNits = (std::abs(loVal - baseLevel) <= std::abs(hiVal - baseLevel)) ? lo : hi;
        std::wcout << L"\n[*] Optimal value identified for current system brightness.\n";
    }

    std::wcout << L"\n[*] Alignment Step Complete.\n";
    std::wcout << L"    -> Selected Slider Setting: " << bestNits << L" Nits\n";

    WriteSDRWhiteLevel(adapterId, targetId, bestNits);
    Sleep(250);

    float finalAdjusted = GetFreshDXGIMaxLuminance();
    std::wcout << L"    -> Base Level: " << baseLevel << L" Nits\n";
    std::wcout << L"    -> Final Adjusted Level: " << finalAdjusted << L" Nits\n";
    std::wcout << L"    -> Final Slider Factor: " << (finalAdjusted > 0.0f ? baseLevel / finalAdjusted : 0.0f) << L"\n\n";
}

int main() {
    _wsetlocale(LC_ALL, L"");
    SelfCheck();
    AlignLevelsToMatch();
    std::wcout << L"Done. Press ENTER to exit.";
    std::cin.get();
    return 0;
}
