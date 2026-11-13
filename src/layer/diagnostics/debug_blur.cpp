#include "debug_blur.hpp"

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace dvr::layer::diagnostics {
using Microsoft::WRL::ComPtr;
namespace {

constexpr char kBlurShader[] = R"(
Texture2D<float4> sourceImage : register(t0);
RWTexture2D<float4> resultImage : register(u0);
SamplerState linearClamp : register(s0);
cbuffer Parameters : register(b0) {
    float4 leftRegion;
    float4 rightRegion;
    float4 sizeAndMip;
    float4 middleRadii;
    float4 ringStyle;
    float4 imageBounds;
};
float ringCoverage(float2 delta, float2 radii, float2 pixelStep) {
    float2 d = delta / radii;
    float gradient = length(2.0 * delta / (radii * radii) * pixelStep);
    float distancePixels = abs(dot(d, d) - 1.0) / max(gradient, 0.000001);
    return saturate(ringStyle.y * 0.5 + 0.5 - distancePixels);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint2 size = uint2(sizeAndMip.xy);
    id.xy += uint2(round(imageBounds.xy * float2(size)));
    if (any(id.xy >= uint2(round(imageBounds.zw * float2(size))))) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    bool left = ringStyle.w < 0.5;
    float2 local = (uv - imageBounds.xy) / (imageBounds.zw - imageBounds.xy);
    float4 region = left ? leftRegion : rightRegion;
    float2 d = (local - region.xy) / region.zw;
    float4 original = sourceImage.Load(int3(id.xy, 0));
    if (ringStyle.x > 0.5) {
        float2 step = 1.0 / ((imageBounds.zw - imageBounds.xy) * float2(size));
        float2 delta = local - region.xy;
        float inner = ringCoverage(delta, region.zw, step);
        float middle = ringCoverage(delta, left ? middleRadii.xy : middleRadii.zw, step);
        float3 color = lerp(original.rgb, float3(1.0, 0.75, 0.05), middle * 0.9);
        color = lerp(color, float3(0.1, 1.0, 0.2), inner * 0.9);
        resultImage[id.xy] = float4(color, original.a);
    } else if (dot(d, d) <= 1.0) {
        resultImage[id.xy] = original;
    } else {
        // Keep the blur footprint within this eye, including at the stereo seam.
        float2 margin = 1.5 * exp2(sizeAndMip.z) / float2(size);
        uv = clamp(uv, imageBounds.xy + margin, imageBounds.zw - margin);
        float4 blurred = sourceImage.SampleLevel(linearClamp, uv, sizeAndMip.z);
        resultImage[id.xy] = float4(blurred.rgb, original.a);
    }
}
)";

constexpr char kCopyShader[] = R"(
Texture2D<float4> sourceImage : register(t0);
float4 vertexMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 pixelMain(float4 position : SV_Position) : SV_Target {
    float4 color = sourceImage.Load(int3(uint2(position.xy), 0));
#ifdef SRGB_TARGET
    // The result stores encoded bytes. Cancel the sRGB RTV's automatic encoding.
    color.rgb = lerp(pow((color.rgb + 0.055) / 1.055, 2.4), color.rgb / 12.92, color.rgb <= 0.04045);
#endif
    return color;
}
)";

struct ComputeState {
    ComPtr<ID3D11DeviceContext1> extended;
    ComPtr<ID3D11ComputeShader> shader;
    std::array<ID3D11ClassInstance*, 256> instances{};
    UINT instanceCount = 256;
    ComPtr<ID3D11ShaderResourceView> resource;
    ComPtr<ID3D11UnorderedAccessView> unordered;
    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0;
    UINT constantCount = 0;
    ComPtr<ID3D11SamplerState> sampler;

    explicit ComputeState(ID3D11DeviceContext* context) {
        context->CSGetShader(&shader, instances.data(), &instanceCount);
        context->CSGetShaderResources(0, 1, &resource);
        context->CSGetUnorderedAccessViews(0, 1, &unordered);
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&extended))))
            extended->CSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
        else
            context->CSGetConstantBuffers(0, 1, &constants);
        context->CSGetSamplers(0, 1, &sampler);
    }

    void restore(ID3D11DeviceContext* context) {
        ID3D11ShaderResourceView* nullResource = nullptr;
        ID3D11UnorderedAccessView* nullUnordered = nullptr;
        context->CSSetShaderResources(0, 1, &nullResource);
        context->CSSetUnorderedAccessViews(0, 1, &nullUnordered, nullptr);
        const UINT preserveCounter = ~0U;
        context->CSSetUnorderedAccessViews(0, 1, unordered.GetAddressOf(), &preserveCounter);
        context->CSSetShaderResources(0, 1, resource.GetAddressOf());
        if (extended)
            extended->CSSetConstantBuffers1(0, 1, constants.GetAddressOf(), &firstConstant, &constantCount);
        else
            context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->CSSetSamplers(0, 1, sampler.GetAddressOf());
        context->CSSetShader(shader.Get(), instances.data(), instanceCount);
    }

    ~ComputeState() {
        for (auto* instance : instances) {
            if (instance) instance->Release();
        }
    }
};

bool textureRegion(const D3D11_TEXTURE2D_DESC& texture, const DebugBlurBounds& bounds, D3D11_BOX& box) {
    if (!std::isfinite(bounds.left) || !std::isfinite(bounds.top) || !std::isfinite(bounds.right) ||
        !std::isfinite(bounds.bottom) || bounds.left < 0 || bounds.top < 0 || bounds.right > 1 || bounds.bottom > 1 ||
        bounds.left >= bounds.right || bounds.top >= bounds.bottom)
        return false;
    box = {static_cast<UINT>(std::round(bounds.left * texture.Width)),
           static_cast<UINT>(std::round(bounds.top * texture.Height)),
           0,
           static_cast<UINT>(std::round(bounds.right * texture.Width)),
           static_cast<UINT>(std::round(bounds.bottom * texture.Height)),
           1};
    return box.right >= box.left + 4 && box.bottom >= box.top + 4;
}

} // namespace

struct DebugBlurRenderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> pyramid;
    ComPtr<ID3D11ShaderResourceView> inputView;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
    bool radiusRings = false;
    bool singleEye = false;
    ComPtr<ID3D11DeviceContext1> copyContext;
    ComPtr<ID3DDeviceContextState> copyState;
    ComPtr<ID3D11VertexShader> copyVertex;
    ComPtr<ID3D11PixelShader> copyPixel;
    ComPtr<ID3D11PixelShader> copySrgbPixel;

    bool initializeCopy() {
        if (copyState) return true;
        ComPtr<ID3D11Device1> owner;
        if (FAILED(device.As(&owner)) || FAILED(context.As(&copyContext))) return false;
        const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        ComPtr<ID3DBlob> vertex, pixel, srgb;
        const D3D_SHADER_MACRO macros[] = {{"SRGB_TARGET", "1"}, {nullptr, nullptr}};
        if (FAILED(D3DCompile(kCopyShader, sizeof(kCopyShader), nullptr, nullptr, nullptr, "vertexMain", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, nullptr)) ||
            FAILED(D3DCompile(kCopyShader, sizeof(kCopyShader), nullptr, nullptr, nullptr, "pixelMain", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, nullptr)) ||
            FAILED(D3DCompile(kCopyShader, sizeof(kCopyShader), nullptr, macros, nullptr, "pixelMain", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &srgb, nullptr)) ||
            FAILED(
                owner->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &copyVertex)) ||
            FAILED(owner->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &copyPixel)) ||
            FAILED(owner->CreatePixelShader(srgb->GetBufferPointer(), srgb->GetBufferSize(), nullptr, &copySrgbPixel)))
            return false;
        return SUCCEEDED(owner->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                         nullptr, &copyState));
    }

    bool initialize(ID3D11Device* owner, const D3D11_TEXTURE2D_DESC& source, DXGI_FORMAT format, bool rings,
                    bool single) {
        *this = Impl{};
        ComPtr<ID3DBlob> compiled;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(kBlurShader, sizeof(kBlurShader), "dynamic_vrs_debug_blur", nullptr, nullptr, "main",
                              "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled, &errors)) ||
            FAILED(owner->CreateComputeShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr,
                                              &shader))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = source.Width;
        texture.Height = source.Height;
        texture.ArraySize = 1;
        texture.MipLevels = rings ? 1 : 0;
        // Copy within the source's format family. The SRV reads BGRA in logical RGBA order.
        texture.Format = format;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texture.MiscFlags = rings ? 0 : D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(owner->CreateTexture2D(&texture, nullptr, &pyramid)) ||
            FAILED(owner->CreateShaderResourceView(pyramid.Get(), nullptr, &inputView))) {
            return false;
        }
        texture.MipLevels = 1;
        // BGRA has no typed UAV support. The compute shader writes a regular RGBA result.
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        texture.MiscFlags = 0;
        if (FAILED(owner->CreateTexture2D(&texture, nullptr, &output)) ||
            FAILED(owner->CreateUnorderedAccessView(output.Get(), nullptr, &outputView))) {
            return false;
        }
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 96;
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(owner->CreateSamplerState(&sampling, &sampler)) ||
            FAILED(owner->CreateBuffer(&buffer, nullptr, &constants))) {
            return false;
        }
        owner->GetImmediateContext(&context);
        device = owner;
        width = source.Width;
        height = source.Height;
        inputFormat = format;
        radiusRings = rings;
        singleEye = single;
        return context != nullptr;
    }
};

DebugBlurRenderer::DebugBlurRenderer() : impl_(std::make_unique<Impl>()) {}
DebugBlurRenderer::~DebugBlurRenderer() = default;

ID3D11Texture2D* DebugBlurRenderer::render(ID3D11Texture2D* source, int eye, const DebugBlurParameters& parameters) {
    if (!source || (eye != 0 && eye != 1)) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const bool single = parameters.singleEye;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default: return nullptr;
    }
    if (desc.SampleDesc.Count != 1 || (!single && desc.ArraySize != 1) || desc.ArraySize > 2 || desc.Width < 256 ||
        desc.Height < 128) {
        return nullptr;
    }
    D3D11_BOX region{};
    if (!textureRegion(desc, parameters.bounds, region)) return nullptr;
    for (const auto& eyeRegion : parameters.eyes) {
        if (!std::isfinite(eyeRegion.centerX) || !std::isfinite(eyeRegion.centerY) ||
            !std::isfinite(eyeRegion.radiusX) || !std::isfinite(eyeRegion.radiusY) || eyeRegion.radiusX <= 0 ||
            eyeRegion.radiusY <= 0)
            return nullptr;
    }
    if (!std::isfinite(parameters.mipLevel)) return nullptr;
    if (parameters.radiusRings) {
        for (int index = 0; index < 2; ++index) {
            if (!std::isfinite(parameters.middleRadiusX[index]) || !std::isfinite(parameters.middleRadiusY[index]) ||
                parameters.middleRadiusX[index] <= 0 || parameters.middleRadiusY[index] <= 0)
                return nullptr;
        }
    }
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    auto& state = *impl_;
    if (state.device.Get() != device.Get() || state.width != desc.Width || state.height != desc.Height ||
        state.inputFormat != inputFormat || state.radiusRings != parameters.radiusRings || state.singleEye != single) {
        if (!state.initialize(device.Get(), desc, inputFormat, parameters.radiusRings, single)) return nullptr;
    }
    auto* context = state.context.Get();
    ComputeState saved(context);
    // An eye array holds the left eye in slice 0 and the right eye in slice 1.
    const UINT slice = single && desc.ArraySize == 2 ? static_cast<UINT>(eye) : 0U;
    context->CopySubresourceRegion(state.pyramid.Get(), 0, region.left, region.top, 0, source,
                                   D3D11CalcSubresource(0, slice, desc.MipLevels), &region);
    if (!parameters.radiusRings) context->GenerateMips(state.inputView.Get());
    const float eyeWidth = static_cast<float>(region.right - region.left);
    const float eyeHeight = static_cast<float>(region.bottom - region.top);
    const float maxMip = std::max(0.0F, std::floor(std::log2(std::min(eyeWidth, eyeHeight))) - 2);
    const float mip = std::clamp(parameters.mipLevel, 0.0F, maxMip);
    const float values[24] = {parameters.eyes[0].centerX,
                              parameters.eyes[0].centerY,
                              parameters.eyes[0].radiusX,
                              parameters.eyes[0].radiusY,
                              parameters.eyes[1].centerX,
                              parameters.eyes[1].centerY,
                              parameters.eyes[1].radiusX,
                              parameters.eyes[1].radiusY,
                              static_cast<float>(state.width),
                              static_cast<float>(state.height),
                              mip,
                              0,
                              parameters.middleRadiusX[0],
                              parameters.middleRadiusY[0],
                              parameters.middleRadiusX[1],
                              parameters.middleRadiusY[1],
                              parameters.radiusRings ? 1.0F : 0.0F,
                              std::max(1.5F, eyeWidth * 0.0015F),
                              single ? 1.0F : 0.0F,
                              static_cast<float>(eye),
                              parameters.bounds.left,
                              parameters.bounds.top,
                              parameters.bounds.right,
                              parameters.bounds.bottom};
    context->UpdateSubresource(state.constants.Get(), 0, nullptr, values, 0, 0);
    context->CSSetShader(state.shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, state.inputView.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, state.outputView.GetAddressOf(), nullptr);
    context->CSSetConstantBuffers(0, 1, state.constants.GetAddressOf());
    context->CSSetSamplers(0, 1, state.sampler.GetAddressOf());
    context->Dispatch((region.right - region.left + 7) / 8, (region.bottom - region.top + 7) / 8, 1);
    saved.restore(context);
    return state.output.Get();
}

bool DebugBlurRenderer::copyTo(ID3D11Texture2D* result, ID3D11Texture2D* destination, unsigned int slice,
                               const DebugBlurBounds& bounds) {
    auto& state = *impl_;
    if (!result || !destination || !state.device) return false;
    D3D11_TEXTURE2D_DESC desc{}, output{};
    destination->GetDesc(&desc);
    result->GetDesc(&output);
    if (slice >= desc.ArraySize || desc.SampleDesc.Count != 1 || desc.Width != output.Width ||
        desc.Height != output.Height)
        return false;
    D3D11_BOX region{};
    if (!textureRegion(desc, bounds, region)) return false;
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        state.context->CopySubresourceRegion(destination, D3D11CalcSubresource(0, slice, desc.MipLevels), region.left,
                                             region.top, 0, result, 0, &region);
        return true;
    }
    if (!(desc.BindFlags & D3D11_BIND_RENDER_TARGET) || !state.initializeCopy()) return false;
    const bool srgb = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    view.ViewDimension = desc.ArraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D;
    view.Texture2DArray.FirstArraySlice = slice;
    view.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> source;
    if (FAILED(state.device->CreateRenderTargetView(destination, &view, &target)) ||
        FAILED(state.device->CreateShaderResourceView(result, nullptr, &source)))
        return false;
    ComPtr<ID3DDeviceContextState> previous;
    state.copyContext->SwapDeviceContextState(state.copyState.Get(), &previous);
    state.context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    state.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    state.context->VSSetShader(state.copyVertex.Get(), nullptr, 0);
    state.context->PSSetShader(srgb ? state.copySrgbPixel.Get() : state.copyPixel.Get(), nullptr, 0);
    state.context->PSSetShaderResources(0, 1, source.GetAddressOf());
    const D3D11_VIEWPORT viewport{static_cast<float>(region.left),
                                  static_cast<float>(region.top),
                                  static_cast<float>(region.right - region.left),
                                  static_cast<float>(region.bottom - region.top),
                                  0,
                                  1};
    state.context->RSSetViewports(1, &viewport);
    state.context->Draw(3, 0);
    state.copyContext->SwapDeviceContextState(previous.Get(), nullptr);
    return true;
}

} // namespace dvr::layer::diagnostics
