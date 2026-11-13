#include <windows.h>
#include <d3d11.h>

#include "common/ini_file.hpp"
#include "common/log.hpp"
#include "common/openvr_abi.hpp"
#include "common/paths.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace {

using dvr::CompositorSubmitFunction;
using dvr::GetGenericInterfaceFunction;
using dvr::logMessage;
using dvr::modulePath;
using dvr::OpenVrTexture;
using dvr::OpenVrTextureBounds;
using dvr::wideToUtf8;

using FactoryFunction = void*(__cdecl*)(const char* interfaceName, int* returnCode);
using XrNegotiateFunction = std::int32_t(__stdcall*)(const void* loaderInfo, void* runtimeRequest);

HMODULE g_proxyModule = nullptr;
HMODULE g_realVrClient = nullptr;
HMODULE g_nativeLayerLibrary = nullptr;
std::atomic<CompositorSubmitFunction> g_originalCompositorSubmit020 = nullptr;
std::atomic<CompositorSubmitFunction> g_originalCompositorSubmit028 = nullptr;
std::atomic<unsigned int> g_submitProbeCount = 0;
std::wstring g_spoofedRealVrClientPathW;
std::string g_spoofedRealVrClientPathA;

std::once_flag g_realLoadOnce;

std::filesystem::path proxyDirectory() { return modulePath(g_proxyModule).parent_path(); }

template <typename Character>
DWORD copySpoofedModulePath(const std::basic_string<Character>& source, Character* destination, DWORD destinationSize) {
    if (!destination || destinationSize == 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    const auto sourceLength = source.size();
    if (sourceLength >= destinationSize) {
        const auto copyLength = static_cast<std::size_t>(destinationSize - 1);
        std::char_traits<Character>::copy(destination, source.data(), copyLength);
        destination[copyLength] = Character{};
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return destinationSize;
    }

    std::char_traits<Character>::copy(destination, source.data(), sourceLength);
    destination[sourceLength] = Character{};
    SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(sourceLength);
}

DWORD WINAPI spoofedGetModuleFileNameW(HMODULE module, LPWSTR destination, DWORD destinationSize) {
    if (module == g_realVrClient && !g_spoofedRealVrClientPathW.empty()) {
        return copySpoofedModulePath(g_spoofedRealVrClientPathW, destination, destinationSize);
    }
    return GetModuleFileNameW(module, destination, destinationSize);
}

DWORD WINAPI spoofedGetModuleFileNameA(HMODULE module, LPSTR destination, DWORD destinationSize) {
    if (module == g_realVrClient && !g_spoofedRealVrClientPathA.empty()) {
        return copySpoofedModulePath(g_spoofedRealVrClientPathA, destination, destinationSize);
    }
    return GetModuleFileNameA(module, destination, destinationSize);
}

bool patchImportedFunction(HMODULE module, const char* functionName, void* replacement, void** original) {
    if (!module || !functionName || !replacement || !original) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    auto* imageBase = reinterpret_cast<unsigned char*>(module);
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return false;
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        SetLastError(ERROR_BAD_EXE_FORMAT);
        return false;
    }

    const auto& importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDirectory.VirtualAddress == 0 || importDirectory.Size == 0) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(imageBase + importDirectory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0) {
            continue;
        }

        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(imageBase + descriptor->OriginalFirstThunk);
        auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(imageBase + descriptor->FirstThunk);
        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++addressThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) {
                continue;
            }

            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(imageBase + nameThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), functionName) != 0) {
                continue;
            }

            auto* slot = reinterpret_cast<void**>(&addressThunk->u1.Function);
            DWORD oldProtection = 0;
            if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
                return false;
            }
            *original = *slot;
            *slot = replacement;
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
            DWORD ignoredProtection = 0;
            VirtualProtect(slot, sizeof(*slot), oldProtection, &ignoredProtection);
            SetLastError(ERROR_SUCCESS);
            return true;
        }
    }

    SetLastError(ERROR_PROC_NOT_FOUND);
    return false;
}

bool installRealVrClientPathCompatibility() {
    const auto proxyPath = modulePath(g_proxyModule).wstring();
    g_spoofedRealVrClientPathW = proxyPath;

    const int ansiLength = WideCharToMultiByte(CP_ACP, 0, proxyPath.c_str(), static_cast<int>(proxyPath.size()),
                                               nullptr, 0, nullptr, nullptr);
    if (ansiLength <= 0) {
        logMessage("stage=real-path-compat result=ansi-conversion-failed win32Error=%lu", GetLastError());
        return false;
    }
    g_spoofedRealVrClientPathA.resize(static_cast<std::size_t>(ansiLength));
    WideCharToMultiByte(CP_ACP, 0, proxyPath.c_str(), static_cast<int>(proxyPath.size()),
                        g_spoofedRealVrClientPathA.data(), ansiLength, nullptr, nullptr);

    void* originalW = nullptr;
    SetLastError(ERROR_SUCCESS);
    const bool patchedW = patchImportedFunction(g_realVrClient, "GetModuleFileNameW",
                                                reinterpret_cast<void*>(&spoofedGetModuleFileNameW), &originalW);
    const DWORD errorW = patchedW ? ERROR_SUCCESS : GetLastError();

    void* originalA = nullptr;
    SetLastError(ERROR_SUCCESS);
    const bool patchedA = patchImportedFunction(g_realVrClient, "GetModuleFileNameA",
                                                reinterpret_cast<void*>(&spoofedGetModuleFileNameA), &originalA);
    const DWORD errorA = patchedA ? ERROR_SUCCESS : GetLastError();

    logMessage("stage=real-path-compat spoof=\"%s\" widePatched=%d wideOriginal=%p "
               "wideError=%lu ansiPatched=%d ansiOriginal=%p ansiError=%lu",
               wideToUtf8(proxyPath).c_str(), patchedW ? 1 : 0, originalW, errorW, patchedA ? 1 : 0, originalA, errorA);
    return patchedW && patchedA;
}

struct LoaderConfig {
    bool enabled = false;
    bool spoofRealVrClientPath = false;
    bool enableSubmitProbe = false;
    std::filesystem::path realVrClientPath;
    std::filesystem::path nativeLayerPath;
};

LoaderConfig loadConfig() {
    LoaderConfig config;
    const auto root = proxyDirectory();
    const dvr::IniFile ini(root / L"dynamic-vrs.ini");

    config.enabled = ini.integer(L"enabled", 0) != 0;
    config.spoofRealVrClientPath = ini.integer(L"spoofRealVrClientPath", 0) != 0;
    config.enableSubmitProbe = ini.integer(L"enableSubmitProbe", 0) != 0;

    config.realVrClientPath = std::filesystem::path(ini.text(L"realVrClientPath", L"real_vrclient_x64.dll"));
    if (config.realVrClientPath.is_relative()) {
        config.realVrClientPath = root / config.realVrClientPath;
    }

    auto payloadDirectory = std::filesystem::path(ini.text(L"payloadDirectory", L"dynamic-vrs_payload"));
    if (payloadDirectory.is_relative()) {
        payloadDirectory = root / payloadDirectory;
    }

    config.nativeLayerPath = payloadDirectory / ini.text(L"nativeLayerLibrary", L"DynamicVRSLayer64.dll");
    return config;
}

HMODULE loadPayloadLibrary(const std::filesystem::path& path, const char* stage) {
    SetLastError(ERROR_SUCCESS);
    HMODULE module =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD error = module ? ERROR_SUCCESS : GetLastError();
    logMessage("stage=%s path=\"%s\" handle=%p win32Error=%lu", stage, wideToUtf8(path.wstring()).c_str(), module,
               error);
    return module;
}

void logCompositorSubmitEntrypoints() {
    HMODULE openVr = GetModuleHandleW(L"openvr_api.dll");
    if (!openVr) {
        logMessage("stage=compositor-entrypoints result=openvr-missing");
        return;
    }
    const auto getInterface =
        reinterpret_cast<GetGenericInterfaceFunction>(GetProcAddress(openVr, "VR_GetGenericInterface"));
    if (!getInterface) {
        logMessage("stage=compositor-entrypoints result=export-missing");
        return;
    }

    for (int version = 5; version <= 29; ++version) {
        char interfaceName[64]{};
        snprintf(interfaceName, sizeof(interfaceName), "IVRCompositor_%03d", version);
        int interfaceError = 0;
        void* object = getInterface(interfaceName, &interfaceError);
        void* submit = nullptr;
        unsigned char bytes[8]{};
        SIZE_T bytesRead = 0;
        if (object) {
            auto** vtable = *reinterpret_cast<void***>(object);
            submit = vtable[5];
            ReadProcessMemory(GetCurrentProcess(), submit, bytes, sizeof(bytes), &bytesRead);
        }
        logMessage("stage=compositor-entrypoint interface=%s error=%d submit=%p "
                   "bytes=%02X%02X%02X%02X%02X%02X%02X%02X bytesRead=%zu",
                   interfaceName, interfaceError, submit, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                   bytes[6], bytes[7], static_cast<std::size_t>(bytesRead));
    }
}

int compositorSubmitProbe(CompositorSubmitFunction original, void* compositor, int eye, const OpenVrTexture* texture,
                          const OpenVrTextureBounds* bounds, int submitFlags) {
    const unsigned int count = g_submitProbeCount.fetch_add(1, std::memory_order_relaxed);
    if (count < 24) {
        D3D11_TEXTURE2D_DESC desc{};
        bool hasD3d11Desc = false;
        if (texture && texture->type == 0 && texture->handle) {
            auto* d3dTexture = static_cast<ID3D11Texture2D*>(texture->handle);
            d3dTexture->GetDesc(&desc);
            hasD3d11Desc = true;
        }
        logMessage("stage=compositor-submit count=%u eye=%d texture=%p handle=%p type=%d "
                   "colorSpace=%d flags=%d bounds=%s%.4f,%.4f,%.4f,%.4f "
                   "d3d11=%d width=%u height=%u array=%u mips=%u samples=%u "
                   "format=%u bind=0x%X misc=0x%X",
                   count, eye, texture, texture ? texture->handle : nullptr, texture ? texture->type : -1,
                   texture ? texture->colorSpace : -1, submitFlags, bounds ? "" : "<null>",
                   bounds ? bounds->uMin : 0.0f, bounds ? bounds->vMin : 0.0f, bounds ? bounds->uMax : 0.0f,
                   bounds ? bounds->vMax : 0.0f, hasD3d11Desc ? 1 : 0, desc.Width, desc.Height, desc.ArraySize,
                   desc.MipLevels, desc.SampleDesc.Count, static_cast<unsigned int>(desc.Format), desc.BindFlags,
                   desc.MiscFlags);
    }
    return original ? original(compositor, eye, texture, bounds, submitFlags) : 1;
}

int __fastcall compositorSubmitProbe020(void* compositor, int eye, const OpenVrTexture* texture,
                                        const OpenVrTextureBounds* bounds, int submitFlags) {
    return compositorSubmitProbe(g_originalCompositorSubmit020.load(std::memory_order_acquire), compositor, eye,
                                 texture, bounds, submitFlags);
}

int __fastcall compositorSubmitProbe028(void* compositor, int eye, const OpenVrTexture* texture,
                                        const OpenVrTextureBounds* bounds, int submitFlags) {
    return compositorSubmitProbe(g_originalCompositorSubmit028.load(std::memory_order_acquire), compositor, eye,
                                 texture, bounds, submitFlags);
}

void installCompositorSubmitProbe() {
    HMODULE openVr = GetModuleHandleW(L"openvr_api.dll");
    const auto getInterface =
        openVr ? reinterpret_cast<GetGenericInterfaceFunction>(GetProcAddress(openVr, "VR_GetGenericInterface"))
               : nullptr;
    if (!getInterface) {
        logMessage("stage=compositor-submit-probe result=openvr-missing");
        return;
    }

    struct Probe {
        const char* name;
        CompositorSubmitFunction replacement;
        std::atomic<CompositorSubmitFunction>* original;
    };
    for (const auto& probe : {Probe{"IVRCompositor_020", &compositorSubmitProbe020, &g_originalCompositorSubmit020},
                              Probe{"IVRCompositor_028", &compositorSubmitProbe028, &g_originalCompositorSubmit028}}) {
        const char* interfaceName = probe.name;
        int interfaceError = 0;
        void* object = getInterface(interfaceName, &interfaceError);
        if (!object) {
            logMessage("stage=compositor-submit-probe interface=%s result=interface-missing "
                       "error=%d",
                       interfaceName, interfaceError);
            continue;
        }
        auto** vtable = *reinterpret_cast<void***>(object);
        void** submitSlot = &vtable[5];
        const auto original = reinterpret_cast<CompositorSubmitFunction>(*submitSlot);
        // Different interface names can alias the same vtable slot.
        if (original == &compositorSubmitProbe020 || original == &compositorSubmitProbe028) continue;

        DWORD oldProtection = 0;
        if (!VirtualProtect(submitSlot, sizeof(*submitSlot), PAGE_READWRITE, &oldProtection)) {
            logMessage("stage=compositor-submit-probe interface=%s result=protect-failed "
                       "win32Error=%lu",
                       interfaceName, GetLastError());
            continue;
        }
        probe.original->store(original, std::memory_order_release);
        InterlockedExchangePointer(submitSlot, reinterpret_cast<void*>(probe.replacement));
        FlushInstructionCache(GetCurrentProcess(), submitSlot, sizeof(*submitSlot));
        DWORD ignoredProtection = 0;
        VirtualProtect(submitSlot, sizeof(*submitSlot), oldProtection, &ignoredProtection);
        logMessage("stage=compositor-submit-probe interface=%s result=installed original=%p "
                   "replacement=%p",
                   interfaceName, reinterpret_cast<void*>(original), reinterpret_cast<void*>(probe.replacement));
    }
}

// Loads the layer into this process, once. Called when a scene application gets its compositor, which is
// after VR_Init succeeded and before the first frame.
void loadLayer() {
    static std::atomic<bool> started = false;
    // Also stops the recursion when the layer asks OpenVR for the compositor itself.
    if (started.exchange(true)) {
        return;
    }

    const LoaderConfig config = loadConfig();
    logMessage("stage=payload-config enabled=%d submitProbe=%d", config.enabled ? 1 : 0,
               config.enableSubmitProbe ? 1 : 0);
    if (!config.enabled) {
        logMessage("stage=payload-skip reason=disabled");
        return;
    }
    if (!std::filesystem::is_regular_file(config.nativeLayerPath)) {
        logMessage("stage=layer-preflight result=missing path=\"%s\"",
                   wideToUtf8(config.nativeLayerPath.wstring()).c_str());
        return;
    }
    g_nativeLayerLibrary = loadPayloadLibrary(config.nativeLayerPath, "layer-load");
    if (!g_nativeLayerLibrary) {
        return;
    }
    // The layer hooks SteamVR's client, so this proxy and that client stay loaded for the rest of the process.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&loadLayer), &pinned);
    using InitializeNativeLayerFunction = int(__cdecl*)(const wchar_t*);
    const auto initialize = reinterpret_cast<InitializeNativeLayerFunction>(
        GetProcAddress(g_nativeLayerLibrary, "DynamicVRSLayer_InitializeWithConfig"));
    logMessage("stage=layer-export name=DynamicVRSLayer_InitializeWithConfig address=%p", initialize);
    if (!initialize) {
        return;
    }
    if (config.enableSubmitProbe) {
        logCompositorSubmitEntrypoints();
        installCompositorSubmitProbe();
    }
    const int initialized = initialize((proxyDirectory() / L"dynamic-vrs.ini").c_str());
    logMessage("stage=payload-complete initialized=%d", initialized);
}

// IVRClientCore_003 from the OpenVR source (src/ivrclientcore.h); not in the public headers. IVRClientCore_002,
// which games built against OpenVR before 1.0.10 ask for, differs only in Init taking no startup info, so
// one forwarder serves both: the extra argument is ignored by a 002 client core.
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

constexpr int VRApplication_Scene = 1;

// Handed to the application in place of SteamVR's client core. It forwards every call and loads the layer
// when a scene application (a game, as opposed to Steam, SteamVR's own programs, overlays and utilities)
// asks for the compositor.
class ClientCoreForwarder final : public IVRClientCore {
public:
    void attach(IVRClientCore* real) {
        real_ = real;
        applicationType_ = -1;
    }

    int Init(int applicationType, const char* startupInfo) override {
        const int error = real_->Init(applicationType, startupInfo);
        applicationType_ = error == 0 ? applicationType : -1;
        logMessage("stage=client-init applicationType=%d error=%d", applicationType, error);
        return error;
    }
    void Cleanup() override {
        applicationType_ = -1;
        real_->Cleanup();
    }
    int IsInterfaceVersionValid(const char* interfaceVersion) override {
        return real_->IsInterfaceVersionValid(interfaceVersion);
    }
    void* GetGenericInterface(const char* nameAndVersion, int* error) override {
        void* result = real_->GetGenericInterface(nameAndVersion, error);
        if (result && applicationType_ == VRApplication_Scene && nameAndVersion &&
            std::strncmp(nameAndVersion, "IVRCompositor_", 14) == 0) {
            loadLayer();
        }
        return result;
    }
    bool BIsHmdPresent() override { return real_->BIsHmdPresent(); }
    const char* GetEnglishStringForHmdError(int error) override { return real_->GetEnglishStringForHmdError(error); }
    const char* GetIDForVRInitError(int error) override { return real_->GetIDForVRInitError(error); }

private:
    IVRClientCore* real_ = nullptr;
    int applicationType_ = -1;
};

ClientCoreForwarder g_clientCore;

void loadRealVrClient() {
    std::call_once(g_realLoadOnce, [] {
        const auto realPath = loadConfig().realVrClientPath;
        logMessage("stage=real-load-start proxy=\"%s\" real=\"%s\" process=\"%s\"",
                   wideToUtf8(modulePath(g_proxyModule).wstring()).c_str(), wideToUtf8(realPath.wstring()).c_str(),
                   wideToUtf8(modulePath(nullptr).wstring()).c_str());

        if (!std::filesystem::is_regular_file(realPath)) {
            logMessage("stage=real-preflight result=missing");
            return;
        }

        SetLastError(ERROR_SUCCESS);
        g_realVrClient = LoadLibraryExW(realPath.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const DWORD error = g_realVrClient ? ERROR_SUCCESS : GetLastError();
        logMessage("stage=real-load-complete handle=%p win32Error=%lu", g_realVrClient, error);
        if (g_realVrClient == g_proxyModule) {
            logMessage("stage=real-load-complete result=self-resolution real=proxy");
            g_realVrClient = nullptr;
            return;
        }
        if (!g_realVrClient) {
            return;
        }

        const LoaderConfig config = loadConfig();
        if (config.spoofRealVrClientPath && !installRealVrClientPathCompatibility()) {
            logMessage("stage=real-path-compat result=required-hook-failed");
            g_realVrClient = nullptr;
            return;
        }

        for (const char* exportName :
             {"HmdSystemFactory", "VRClientCoreFactory", "xrNegotiateLoaderRuntimeInterface"}) {
            logMessage("stage=real-export name=%s address=%p", exportName, GetProcAddress(g_realVrClient, exportName));
        }
    });
}

template <typename Function> Function resolveRealExport(const char* name) {
    loadRealVrClient();
    if (!g_realVrClient) {
        return nullptr;
    }
    const auto function = reinterpret_cast<Function>(GetProcAddress(g_realVrClient, name));
    if (!function) {
        logMessage("stage=forward-resolve name=%s result=missing win32Error=%lu", name, GetLastError());
    }
    return function;
}

void setFactoryFailure(int* returnCode) {
    if (returnCode) {
        *returnCode = 1; // VRInitError_Unknown; keeps this proxy independent of OpenVR headers.
    }
}

void* callFactory(const char* exportName, const char* interfaceName, int* returnCode) {
    const auto function = resolveRealExport<FactoryFunction>(exportName);
    if (!function) {
        setFactoryFailure(returnCode);
        return nullptr;
    }

    logMessage("stage=forward-call name=%s interface=\"%s\"", exportName, interfaceName ? interfaceName : "<null>");
    void* result = function(interfaceName, returnCode);
    logMessage("stage=forward-return name=%s result=%p returnCode=%d", exportName, result,
               returnCode ? *returnCode : 0);
    if (result && interfaceName &&
        (std::strcmp(interfaceName, "IVRClientCore_003") == 0 ||
         std::strcmp(interfaceName, "IVRClientCore_002") == 0)) {
        g_clientCore.attach(static_cast<IVRClientCore*>(result));
        return &g_clientCore;
    }
    return result;
}

} // namespace

extern "C" void* __cdecl HmdSystemFactory(const char* interfaceName, int* returnCode) {
    return callFactory("HmdSystemFactory", interfaceName, returnCode);
}

extern "C" void* __cdecl VRClientCoreFactory(const char* interfaceName, int* returnCode) {
    return callFactory("VRClientCoreFactory", interfaceName, returnCode);
}

extern "C" std::int32_t __stdcall xrNegotiateLoaderRuntimeInterface(const void* loaderInfo, void* runtimeRequest) {
    const auto function = resolveRealExport<XrNegotiateFunction>("xrNegotiateLoaderRuntimeInterface");
    if (!function) {
        return -1;
    }
    logMessage("stage=forward-call name=xrNegotiateLoaderRuntimeInterface");
    return function(loaderInfo, runtimeRequest);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxyModule = module;
        dvr::setLogComponent(L"proxy");
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && !reserved && g_realVrClient) {
        // OpenVR unloads its client library in VR_Shutdown and loads a fresh one for the next VR_Init. SteamVR's
        // client depends on that: initialized a second time without being reloaded, it fails with
        // Init_InstallationCorrupt (101). Steam does exactly this when a game starts SteamVR, so the real
        // client has to go when the proxy goes. Once the layer is loaded the proxy is pinned and never gets
        // here.
        FreeLibrary(g_realVrClient);
        g_realVrClient = nullptr;
    }
    return TRUE;
}
