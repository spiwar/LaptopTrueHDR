#include <Windows.h>
#include <dxgi1_6.h>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>

// WinRT headers for accessing raw hardware metrics
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Enumeration.h>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "runtimeobject.lib") 
#pragma comment(lib, "ole32.lib")     
#pragma comment(lib, "oleaut32.lib")  

using namespace winrt::Windows::Devices::Display;
using namespace winrt::Windows::Devices::Enumeration;

enum DISPLAYCONFIG_DEVICE_INFO_TYPE_INTERNAL {
    DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL = 0xFFFFFFEE,
};

typedef struct _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    unsigned int SDRWhiteLevel;
    unsigned char finalValue;
} _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL;

int GetCurrentSliderNits(LUID adapterId, UINT32 targetId) {
    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(whiteLevel);
    whiteLevel.header.adapterId = adapterId;
    whiteLevel.header.id = targetId;

    if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS) {
        return (whiteLevel.SDRWhiteLevel * 80) / 1000;
    }
    return 80; 
}

LONG WriteSDRWhiteLevel(LUID adapterId, UINT32 targetId, int nits) {
    _DISPLAYCONFIG_SET_SDR_WHITE_LEVEL sdrWhiteParams = {};
    sdrWhiteParams.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)DISPLAYCONFIG_DEVICE_INFO_SET_SDR_WHITE_LEVEL;
    sdrWhiteParams.header.size = sizeof(sdrWhiteParams);
    sdrWhiteParams.header.adapterId = adapterId;
    sdrWhiteParams.header.id = targetId;
    sdrWhiteParams.SDRWhiteLevel = (nits * 1000) / 80;
    sdrWhiteParams.finalValue = 1;

    return DisplayConfigSetDeviceInfo(&sdrWhiteParams.header);
}

float GetFreshDXGIMaxLuminance() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return 0.0f;
    
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(0, &adapter) == DXGI_ERROR_NOT_FOUND) {
        factory->Release();
        return 0.0f;
    }
    
    IDXGIOutput* output = nullptr;
    if (adapter->EnumOutputs(0, &output) == DXGI_ERROR_NOT_FOUND) {
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

    float baseLevel = 0.0f;
    try {
        IDXGIFactory1* initFactory = nullptr;
        CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&initFactory);
        IDXGIAdapter1* initAdapter = nullptr;
        initFactory->EnumAdapters1(0, &initAdapter);
        IDXGIOutput* initOutput = nullptr;
        initAdapter->EnumOutputs(0, &initOutput);
        IDXGIOutput6* initOutput6 = nullptr;
        initOutput->QueryInterface(__uuidof(IDXGIOutput6), (void**)&initOutput6);
        
        DXGI_OUTPUT_DESC1 initDesc = {};
        initOutput6->GetDesc1(&initDesc);

        DISPLAY_DEVICEW device = {};
        device.cb = sizeof(device);

        for (UINT deviceIndex = 0; EnumDisplayDevicesW(initDesc.DeviceName, deviceIndex, &device, EDD_GET_DEVICE_INTERFACE_NAME); deviceIndex++) {
            if (device.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                winrt::hstring hstr = winrt::to_hstring(device.DeviceID);
                DisplayMonitor foundMonitor = DisplayMonitor::FromInterfaceIdAsync(hstr).get();
                if (foundMonitor) {
                    baseLevel = foundMonitor.MaxLuminanceInNits();
                    break;
                }
            }
        }
        initOutput6->Release();
        initOutput->Release();
        initAdapter->Release();
        initFactory->Release();
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

    LUID adapterId = paths[0].targetInfo.adapterId;
    UINT32 targetId = paths[0].targetInfo.id;

    const int initialNits = GetCurrentSliderNits(adapterId, targetId);
    int testNits = initialNits;
    int bestNits = initialNits;

    std::wcout << L"[Base Level Found] (Hardware Peak): " << baseLevel << L" Nits\n";
    std::wcout << L"[Current System State]           : " << initialNits << L" Nits\n";
    std::wcout << L"[*] Initializing calibration...\n\n";

    float adjustedLevel = GetFreshDXGIMaxLuminance();
    float currentFactor = baseLevel / adjustedLevel;
    float currentDelta = std::abs(adjustedLevel - baseLevel);
    float bestDelta = currentDelta;

    int direction = (currentFactor > 1.0f) ? 1 : -1;
    int worseStepCount = 0;
    bool directionFlipped = false;
    
    // Limits stride sizes globally after an overshoot to prevent loop bouncing
    int maxStepLimit = 100; 

    while (testNits >= 80 && testNits <= 400) {
        float factorDelta = std::abs(currentFactor - 1.0f);
        
        // Coarse-To-Fine Adaptive Stride Engine with Macro-Jumping
        int stepSize = 40; // Macro-jump for extreme distance optimization
        if (factorDelta <= 0.015f)      stepSize = 1;  // Micro-crawl precision
        else if (factorDelta <= 0.04f)  stepSize = 2;  // High precision adjustment
        else if (factorDelta <= 0.12f)  stepSize = 5;  // Intermediate approach
        else if (factorDelta <= 0.30f)  stepSize = 20; // Fast traversal

        // Enforce the calculation ceiling to scale down step sizes if we have overshot
        if (stepSize > maxStepLimit) {
            stepSize = maxStepLimit;
        }

        int nextNits = testNits + (direction * stepSize);
        if (nextNits < 80)  nextNits = 80;
        if (nextNits > 400) nextNits = 400;

        if (nextNits == testNits) {
            std::wcout << L"[-] 80 Nit limit reached. Increase your system brightness\n";
            break;
        }

        WriteSDRWhiteLevel(adapterId, targetId, nextNits);
        Sleep(250); 

        float nextAdjusted = GetFreshDXGIMaxLuminance();
        float nextFactor = baseLevel / nextAdjusted;
        float nextDelta = std::abs(nextAdjusted - baseLevel);

        std::wcout << L"  Testing Slider: " << nextNits 
                   << L" Nits | Adjusted Level: " << nextAdjusted 
                   << L" Nits | Factor: " << nextFactor 
                   << L" | Stride: " << stepSize << L" Nits\n";

        if (nextDelta < bestDelta) {
            bestDelta = nextDelta;
            bestNits = nextNits;
            testNits = nextNits;
            currentFactor = nextFactor;
            currentDelta = nextDelta;
            worseStepCount = 0;
        } else {
            if (testNits == initialNits && !directionFlipped) {
                std::wcout << L"  [!] Switching search direction due to factor degradation.\n";
                direction = -direction;
                directionFlipped = true;
                continue;
            } else {
                if (stepSize > 1) {
                    // Overshot boundary recovery: flip direction and halve the step ceiling
                    direction = -direction;
                    maxStepLimit = stepSize / 2; 
                    if (maxStepLimit < 1) maxStepLimit = 1;
                    worseStepCount = 0;
                } else {
                    worseStepCount++;
                    if (worseStepCount >= 2) {
                        std::wcout << L"\n[*] Optimal value identified for current system brightness.\n";
                        break;
                    }
                    testNits = nextNits;
                }
            }
        }
    }

    std::wcout << L"\n[*] Alignment Step Complete.\n";
    std::wcout << L"    -> Selected Slider Setting: " << bestNits << L" Nits\n";
    
    WriteSDRWhiteLevel(adapterId, targetId, bestNits);
    Sleep(250);

    float finalAdjusted = GetFreshDXGIMaxLuminance();
    std::wcout << L"    -> Base Level: " << baseLevel << L" Nits\n";
    std::wcout << L"    -> Final Adjusted Level: " << finalAdjusted << L" Nits\n";
    std::wcout << L"    -> Final Slider Factor: " << (baseLevel / finalAdjusted) << L"\n\n";
}

int main() {
    _wsetlocale(LC_ALL, L"");
    AlignLevelsToMatch();
    std::wcout << L"Done. Press ENTER to exit.";
    std::cin.get();
    return 0;
}