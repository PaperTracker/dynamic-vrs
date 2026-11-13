#include "alignment_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <wrl/client.h>

namespace dvr::layer::diagnostics {
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned Grid = 64;
using Luminance = std::vector<float>;
float halfFloat(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0    ? std::ldexp(float(m), -24)
                    : e == 31 ? INFINITY
                              : std::ldexp(1.0F + float(m) / 1024.0F, e - 15);
    return h & 0x8000 ? -v : v;
}
Luminance sample(ID3D11Texture2D* source, DXGI_FORMAT viewFormat) {
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (d.Width < 128 || d.Height < Grid || d.Width % 2 || d.ArraySize != 1) return {};
    auto format = d.Format;
    if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        format = viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? viewFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == DXGI_FORMAT_B8G8R8A8_TYPELESS) format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format == DXGI_FORMAT_R16G16B16A16_TYPELESS) format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (format == DXGI_FORMAT_R32G32B32A32_TYPELESS) format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    unsigned bytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16 : 4;
    if (!bgra && bytes == 4 && format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        return {};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    source->GetDevice(&device);
    device->GetImmediateContext(&context);
    const unsigned width = d.Width, height = d.Height;
    d.Format = format;
    d.MipLevels = 1;
    d.SampleDesc = {1, 0};
    d.BindFlags = d.MiscFlags = d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> resolved;
    D3D11_TEXTURE2D_DESC original{};
    source->GetDesc(&original);
    ID3D11Texture2D* input = source;
    if (original.SampleDesc.Count > 1) {
        if (FAILED(device->CreateTexture2D(&d, nullptr, &resolved))) return {};
        context->ResolveSubresource(resolved.Get(), 0, source, 0, format);
        input = resolved.Get();
    }
    d.Height = Grid;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    std::vector<uint8_t> zero(static_cast<size_t>(width) * Grid * bytes, 0);
    D3D11_SUBRESOURCE_DATA initial{zero.data(), width * bytes, 0};
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&d, &initial, &staging))) return {};
    for (unsigned row = 0; row < Grid; ++row) {
        const unsigned y = (row * (height - 1) + (Grid - 1) / 2) / (Grid - 1);
        D3D11_BOX box{0, y, 0, width, y + 1, 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, row, 0, input, 0, &box);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    Luminance out(2 * Grid * Grid);
    bool finite = true;
    for (unsigned eye = 0; eye < 2; ++eye)
        for (unsigned y = 0; y < Grid; ++y)
            for (unsigned x = 0; x < Grid; ++x) {
                const unsigned px = eye * (width / 2) + (x * (width / 2 - 1) + (Grid - 1) / 2) / (Grid - 1);
                const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + px * bytes;
                float rgb[4]{};
                if (bytes == 16)
                    std::memcpy(rgb, pixel, 16);
                else if (bytes == 8) {
                    uint16_t h[4];
                    std::memcpy(h, pixel, 8);
                    for (int c = 0; c < 3; ++c) rgb[c] = halfFloat(h[c]);
                } else {
                    for (int c = 0; c < 3; ++c) rgb[c] = float(pixel[bgra ? 2 - c : c]) / 255.0F;
                }
                const float value = .2126F * rgb[0] + .7152F * rgb[1] + .0722F * rgb[2];
                finite = finite && std::isfinite(value);
                out[(eye * Grid + y) * Grid + x] = value;
            }
    context->Unmap(staging.Get(), 0);
    return finite ? out : Luminance{};
}
float correlation(const Luminance& source, const Luminance& final, unsigned eye, bool flip, int shift, float scale) {
    double a = 0, b = 0, aa = 0, bb = 0, ab = 0, n = 0;
    for (unsigned y = 0; y < Grid; ++y) {
        const int sy =
            static_cast<int>(std::lround(((flip ? float(Grid - 1 - y) : float(y)) - 31.5F) * scale + 31.5F + shift));
        if (sy < 0 || sy >= int(Grid)) continue;
        for (unsigned x = 0; x < Grid; ++x) {
            const double p = source[(eye * Grid + sy) * Grid + x], q = final[(eye * Grid + y) * Grid + x];
            a += p;
            b += q;
            aa += p * p;
            bb += q * q;
            ab += p * q;
            ++n;
        }
    }
    const double va = aa - a * a / n, vb = bb - b * b / n;
    if (n < Grid * Grid / 2 || va / n < .000001 || vb / n < .000001)
        return -2; // Flat/insufficient image: no conclusion.
    return static_cast<float>((ab - a * b / n) / std::sqrt(va * vb));
}
} // namespace

struct TargetAlignmentProbe::Impl {
    struct Candidate {
        ComPtr<ID3D11Texture2D> texture;
        TargetAlignmentResult result;
    };
    std::vector<Candidate> candidates;
    bool truncated = false;
};
TargetAlignmentProbe::TargetAlignmentProbe() : impl_(std::make_unique<Impl>()) {}
TargetAlignmentProbe::~TargetAlignmentProbe() = default;
void TargetAlignmentProbe::reset() {
    impl_->candidates.clear();
    impl_->truncated = false;
}
bool TargetAlignmentProbe::truncated() const { return impl_->truncated; }
void TargetAlignmentProbe::observe(ID3D11Texture2D* texture, uint32_t format, uint32_t count,
                                   const std::array<float, 8>& viewports) {
    for (auto& c : impl_->candidates)
        if (c.texture.Get() == texture) {
            c.result.format = format;
            c.result.viewportCount = count;
            c.result.viewports = viewports;
            return;
        }
    if (impl_->candidates.size() >= 4) {
        impl_->truncated = true;
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    Impl::Candidate c;
    c.texture = texture;
    c.result.texture = reinterpret_cast<uintptr_t>(texture);
    c.result.width = d.Width;
    c.result.height = d.Height;
    c.result.samples = d.SampleDesc.Count;
    c.result.format = format;
    c.result.viewportCount = count;
    c.result.viewports = viewports;
    impl_->candidates.push_back(std::move(c));
}
std::vector<TargetAlignmentResult> TargetAlignmentProbe::capture(ID3D11Texture2D* submitted) {
    std::vector<TargetAlignmentResult> results;
    ComPtr<ID3D11Device> finalDevice;
    submitted->GetDevice(&finalDevice);
    const auto final = sample(submitted, DXGI_FORMAT_UNKNOWN);
    for (auto& candidate : impl_->candidates) {
        auto result = candidate.result;
        ComPtr<ID3D11Device> candidateDevice;
        candidate.texture->GetDevice(&candidateDevice);
        if (candidateDevice.Get() != finalDevice.Get()) {
            results.push_back(result);
            continue;
        }
        result.sameAsSubmit = candidate.texture.Get() == submitted;
        const auto input =
            result.sameAsSubmit ? final : sample(candidate.texture.Get(), static_cast<DXGI_FORMAT>(result.format));
        result.captured = !input.empty() && !final.empty();
        if (result.captured)
            for (unsigned eye = 0; eye < 2; ++eye) {
                auto& score = result.eyes[eye];
                score.identity = correlation(input, final, eye, false, 0, 1);
                score.flipped = correlation(input, final, eye, true, 0, 1);
                score.best = score.identity;
                if (result.sameAsSubmit || (score.identity <= -1.5F && score.flipped <= -1.5F)) continue;
                for (bool flip : {false, true})
                    for (float scale : {1.F, .75F, .875F, 1.125F, 1.25F})
                        for (int shift = -16; shift <= 16; ++shift) {
                            const float value = correlation(input, final, eye, flip, shift, scale);
                            if (value > score.best + .0001F) {
                                score.best = value;
                                score.flipY = flip;
                                score.shiftRows = shift;
                                score.scaleY = scale;
                            }
                        }
            }
        results.push_back(result);
    }
    reset();
    return results;
}
} // namespace dvr::layer::diagnostics
