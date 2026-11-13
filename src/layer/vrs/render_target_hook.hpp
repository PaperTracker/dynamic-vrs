#pragma once
#include <d3d11_4.h>

namespace dvr::layer {

class RenderTargetHookBypass {
public:
    RenderTargetHookBypass() { ++depth_; }
    ~RenderTargetHookBypass() { --depth_; }
    RenderTargetHookBypass(const RenderTargetHookBypass&) = delete;
    RenderTargetHookBypass& operator=(const RenderTargetHookBypass&) = delete;
    static bool active() { return depth_ != 0; }

private:
    inline static thread_local unsigned int depth_ = 0;
};

// Match the runtime's lock order: a game can hold its D3D multithread lock while
// calling our OM hook. Submit-side work must acquire that lock before g_vrsMutex.
class ContextThreadGuard {
public:
    explicit ContextThreadGuard(ID3D11DeviceContext* context) { enter(context); }
    explicit ContextThreadGuard(ID3D11Texture2D* texture) {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        if (texture) texture->GetDevice(&device);
        if (device) device->GetImmediateContext(&context);
        enter(context);
        if (context) context->Release();
        if (device) device->Release();
    }
    ~ContextThreadGuard() {
        if (multithread_) {
            multithread_->Leave();
            multithread_->Release();
        }
    }
    ContextThreadGuard(const ContextThreadGuard&) = delete;
    ContextThreadGuard& operator=(const ContextThreadGuard&) = delete;

private:
    void enter(ID3D11DeviceContext* context) {
        if (context &&
            SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread_))))
            multithread_->Enter();
    }
    ID3D11Multithread* multithread_ = nullptr;
};

// Hooks render-target and viewport changes so the rate image follows the actual
// eye render area. Re-arms after runtime dispatch changes and preserves foreign hooks.
bool installContextRenderTargetHook(ID3D11DeviceContext* context);

} // namespace dvr::layer
