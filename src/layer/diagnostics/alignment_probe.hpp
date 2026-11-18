#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "layer/vrs/eye_viewport.hpp"

namespace dvr::layer::diagnostics {
struct AlignmentScore {
    float identity = -2, flipped = -2, best = -2, scaleY = 1;
    int shiftRows = 0;
    bool flipY = false;
};
struct TargetAlignmentResult {
    uintptr_t texture = 0;
    uint32_t width = 0, height = 0, samples = 0, format = 0, viewportCount = 0;
    std::array<float, 8> viewports{};
    bool sameAsSubmit = false, captured = false;
    std::array<AlignmentScore, 2> eyes;
};
// One-shot read-only diagnostic. Retains at most four targets for one frame,
// samples 64x64 luminance per eye, and returns numbers only (no stored images).
class TargetAlignmentProbe {
public:
    TargetAlignmentProbe();
    ~TargetAlignmentProbe();
    void reset();
    bool truncated() const;
    void observe(ID3D11Texture2D*, uint32_t viewFormat, uint32_t viewportCount, const std::array<float, 8>&);
    std::vector<TargetAlignmentResult> capture(ID3D11Texture2D* submitted, EyeLayout layout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dvr::layer::diagnostics
