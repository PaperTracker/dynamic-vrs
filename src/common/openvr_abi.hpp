#pragma once

namespace dvr {
// The parts of the OpenVR ABI the Submit hooks need without the SDK headers.
using GetGenericInterfaceFunction = void*(__cdecl*)(const char* interfaceName, int* returnCode);

struct OpenVrTexture {
    void* handle;
    int type;
    int colorSpace;
};

struct OpenVrTextureBounds {
    float uMin;
    float vMin;
    float uMax;
    float vMax;
};

using CompositorSubmitFunction = int(__fastcall*)(void* compositor, int eye, const OpenVrTexture* texture,
                                                  const OpenVrTextureBounds* bounds, int submitFlags);
} // namespace dvr
