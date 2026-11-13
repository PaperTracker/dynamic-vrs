#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using FactoryFunction = void*(__cdecl*)(const char* interfaceName, int* returnCode);

// IVRClientCore_003 from the OpenVR source (src/ivrclientcore.h); not in the public headers.
class IVRClientCore {
public:
    virtual int Init(int applicationType, const char* startupInfo) = 0;
    virtual void Cleanup() = 0;
    virtual int IsInterfaceVersionValid(const char* interfaceVersion) = 0;
    virtual void* GetGenericInterface(const char* nameAndVersion, int* error) = 0;
    virtual bool BIsHmdPresent() = 0;
    virtual const char* GetEnglishStringForHmdError(int error) = 0;
    virtual const char* GetIDForVRInitError(int error) = 0;
};

constexpr int VRApplication_Background = 3;
constexpr int VRInitError_None = 0;
constexpr int VRInitError_Init_NoServerForBackgroundApp = 121;

std::wstring modulePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())));
    return path;
}

// Any loaded module that exports the OpenVR client entry point: the proxy or SteamVR's client.
HMODULE findClientLibrary() {
    std::vector<HMODULE> modules(1024);
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                            &bytes)) {
        return nullptr;
    }
    modules.resize(std::min<std::size_t>(modules.size(), bytes / sizeof(HMODULE)));
    for (const HMODULE module : modules) {
        if (GetProcAddress(module, "VRClientCoreFactory")) {
            return module;
        }
    }
    return nullptr;
}

void printUsage() {
    std::wcerr << L"Usage: dynamic_vrs_proxy_probe.exe <vrclient_x64.dll> [--call-factory | --init-background]\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) {
        printUsage();
        return 2;
    }

    // --init-background initializes SteamVR's client as a background application, which never
    // starts SteamVR. "No server" or success means SteamVR accepts the installation; a broken
    // layout fails earlier, for example with Init_InstallationCorrupt (101).
    const bool initBackground = argc == 3 && std::wstring_view(argv[2]) == L"--init-background";
    const bool callFactory = initBackground || (argc == 3 && std::wstring_view(argv[2]) == L"--call-factory");
    if (argc == 3 && !callFactory) {
        printUsage();
        return 2;
    }

    const std::filesystem::path proxyPath = std::filesystem::absolute(argv[1]);
    HMODULE proxy =
        LoadLibraryExW(proxyPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!proxy) {
        std::wcerr << L"LoadLibraryExW failed for " << proxyPath << L" (error " << GetLastError() << L")\n";
        return 3;
    }

    bool missingExport = false;
    for (const char* name : {"HmdSystemFactory", "VRClientCoreFactory", "xrNegotiateLoaderRuntimeInterface"}) {
        const auto address = GetProcAddress(proxy, name);
        std::cout << name << "=" << address << "\n";
        missingExport = missingExport || address == nullptr;
    }
    if (missingExport) {
        return 4;
    }

    if (callFactory) {
        const auto factory = reinterpret_cast<FactoryFunction>(GetProcAddress(proxy, "VRClientCoreFactory"));
        int returnCode = 0;
        void* clientCore = factory("IVRClientCore_003", &returnCode);
        std::cout << "VRClientCoreFactory result=" << clientCore << " returnCode=" << returnCode << "\n";
        if (!clientCore) {
            return 5;
        }
        if (initBackground) {
            auto* core = static_cast<IVRClientCore*>(clientCore);
            const int initError = core->Init(VRApplication_Background, nullptr);
            const char* initErrorId = core->GetIDForVRInitError(initError);
            std::cout << "Init(Background) result=" << initError << " " << (initErrorId ? initErrorId : "") << "\n";
            if (initError == VRInitError_None) {
                core->Cleanup();
            }
            if (initError != VRInitError_None && initError != VRInitError_Init_NoServerForBackgroundApp) {
                return 6;
            }
        }
        // OpenVR unloads its client library in VR_Shutdown and expects a fresh one at the next VR_Init. If the
        // proxy left SteamVR's client loaded, that next VR_Init fails with Init_InstallationCorrupt (101).
        FreeLibrary(proxy);
        if (const HMODULE leftover = findClientLibrary()) {
            std::wcerr << L"SteamVR's client is still loaded after unloading the proxy: " << modulePath(leftover)
                       << L"\n";
            return 7;
        }
        std::cout << "unload=clean\n";
        return 0;
    }

    FreeLibrary(proxy);
    return 0;
}
