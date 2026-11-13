#include "view_geometry.hpp"

#include <array>
#include <cmath>

namespace dvr::layer::openxr {
namespace {

using Rotation = std::array<std::array<float, 3>, 3>;

bool rotationFromQuaternion(XrQuaternionf q, Rotation& matrix) {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!std::isfinite(length) || length < 0.00001F) return false;
    q.x /= length;
    q.y /= length;
    q.z /= length;
    q.w /= length;
    matrix = {{{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
               {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
               {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)}}};
    return true;
}

bool rectEquals(const XrRect2Di& rect, int x, int width, int height) {
    return rect.offset.x == x && rect.offset.y == 0 && rect.extent.width == width && rect.extent.height == height;
}

} // namespace

// The inverse head rotation turns the located eye pose into an eye-to-head transform.
bool eyeProjectionFromView(const XrView& view, const XrPosef& head, EyeProjectionData& eye) {
    Rotation eyeRotation{}, headRotation{};
    if (!rotationFromQuaternion(view.pose.orientation, eyeRotation) ||
        !rotationFromQuaternion(head.orientation, headRotation))
        return false;
    eye.tangentLeft = std::tan(view.fov.angleLeft);
    eye.tangentRight = std::tan(view.fov.angleRight);
    eye.tangentTop = -std::tan(view.fov.angleUp);
    eye.tangentBottom = -std::tan(view.fov.angleDown);
    const float width = eye.tangentRight - eye.tangentLeft;
    const float height = eye.tangentBottom - eye.tangentTop;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return false;
    eye.projection.m[0][0] = 2 / width;
    eye.projection.m[0][2] = (eye.tangentRight + eye.tangentLeft) / width;
    eye.projection.m[1][1] = 2 / height;
    eye.projection.m[1][2] = -(eye.tangentBottom + eye.tangentTop) / height;
    eye.projection.m[2][2] = eye.projection.m[3][2] = -1;
    const float delta[] = {view.pose.position.x - head.position.x, view.pose.position.y - head.position.y,
                           view.pose.position.z - head.position.z};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            for (int k = 0; k < 3; ++k) eye.eyeToHead.m[row][column] += headRotation[k][row] * eyeRotation[k][column];
        for (int k = 0; k < 3; ++k) eye.eyeToHead.m[row][3] += headRotation[k][row] * delta[k];
        if (!std::isfinite(eye.eyeToHead.m[row][3])) return false;
    }
    eye.ready = true;
    return true;
}

EyeLayout classifyEyeLayout(const XrSwapchainCreateInfo& left, const XrSwapchainSubImage& leftSubImage,
                            const XrSwapchainCreateInfo& right, const XrSwapchainSubImage& rightSubImage,
                            bool sameSwapchain) {
    const int width = static_cast<int>(left.width), height = static_cast<int>(left.height);
    const bool firstArraySlices = leftSubImage.imageArrayIndex == 0 && rightSubImage.imageArrayIndex == 1;
    const bool bothFirstSlice = leftSubImage.imageArrayIndex == 0 && rightSubImage.imageArrayIndex == 0;
    if (sameSwapchain && left.arraySize == 2 && firstArraySlices &&
        rectEquals(leftSubImage.imageRect, 0, width, height) && rectEquals(rightSubImage.imageRect, 0, width, height))
        return EyeLayout::Array;
    if (sameSwapchain && left.arraySize == 1 && bothFirstSlice && width % 2 == 0 &&
        rectEquals(leftSubImage.imageRect, 0, width / 2, height) &&
        rectEquals(rightSubImage.imageRect, width / 2, width / 2, height))
        return EyeLayout::SideBySide;
    if (!sameSwapchain && left.arraySize == 1 && right.arraySize == 1 && bothFirstSlice && left.width == right.width &&
        left.height == right.height && rectEquals(leftSubImage.imageRect, 0, width, height) &&
        rectEquals(rightSubImage.imageRect, 0, width, height))
        return EyeLayout::PerEye;
    return EyeLayout::Unknown;
}

} // namespace dvr::layer::openxr
