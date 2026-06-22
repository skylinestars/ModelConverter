#pragma once

#include "mc/common/Types.h"
#include <string>

namespace mc {

// ============================================================
// Camera
// ============================================================
enum class CameraType { Perspective, Orthographic };

struct Camera
{
    ObjectID    id;
    std::string name;
    CameraType  type        = CameraType::Perspective;

    // Perspective
    float       yfov        = 1.047f;    // 垂直 FOV（弧度）~60 度
    float       aspectRatio = 1.0f;
    float       znear       = 0.1f;
    float       zfar        = 100.0f;

     // Orthographic（type == Orthographic 时才用）
    float       xmag        = 1.0f;
    float       ymag        = 1.0f;
};

} // namespace mc