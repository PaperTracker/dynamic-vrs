#pragma once

#include <memory>

struct ID3D11Texture2D;

namespace dvr::layer::diagnostics {

struct DebugBlurRegion {
    float centerX;
    float centerY;
    float radiusX;
    float radiusY;
};

struct DebugBlurBounds {
    float left = 0.0F;
    float top = 0.0F;
    float right = 1.0F;
    float bottom = 1.0F;
};

struct DebugBlurParameters {
    DebugBlurRegion eyes[2];
    float mipLevel = 7.0F;
    bool radiusRings = false;
    float middleRadiusX[2]{};
    float middleRadiusY[2]{};
    // The source holds one eye per image (its own texture, or its slice of a texture array) instead of
    // both eyes side by side.
    bool singleEye = false;
    DebugBlurBounds bounds;
};

// Diagnostic presentation only. The returned texture belongs to the renderer;
// the game's source texture and graphics pipeline state are preserved. For a single-eye source the
// result is that eye's image as a plain texture. Only the selected bounds contain
// the current result; copy back that same rectangle before the next render call.
class DebugBlurRenderer {
public:
    DebugBlurRenderer();
    ~DebugBlurRenderer();
    ID3D11Texture2D* render(ID3D11Texture2D* source, int eye, const DebugBlurParameters& parameters);
    // Copy into the application's color image before OpenVR Submit or OpenXR image release.
    bool copyTo(ID3D11Texture2D* result, ID3D11Texture2D* destination, unsigned int slice,
                const DebugBlurBounds& bounds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvr::layer::diagnostics
