#pragma once
#include <openxr/openxr.h>

#include "layer/vrs/eye_projection.hpp"
#include "layer/vrs/eye_viewport.hpp"

// Pure geometry for the OpenXR layer: what a located view means for gaze projection, and how an
// application packs its two eyes into swapchain images. No runtime calls, so it stays independent of the layer's state.

namespace dvr::layer::openxr {

// The eye's projection and its transform into head space, from a located view and the view-space head pose
// of the same display time. False when either pose or the field of view is unusable.
bool eyeProjectionFromView(const XrView& view, const XrPosef& head, EyeProjectionData& eye);

// How a projection layer's two subimages sit in their swapchain images. Unknown when the packing is one this
// layer cannot give a shading rate image: it needs a full-size image per eye, two array slices or the
// halves of one image. left and right describe the swapchains of the two subimages.
EyeLayout classifyEyeLayout(const XrSwapchainCreateInfo& left, const XrSwapchainSubImage& leftSubImage,
                            const XrSwapchainCreateInfo& right, const XrSwapchainSubImage& rightSubImage,
                            bool sameSwapchain);

} // namespace dvr::layer::openxr
