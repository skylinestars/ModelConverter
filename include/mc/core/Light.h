#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>

namespace mc {

// ============================================================
// Light
// ============================================================
enum class LightType { Point, Directional, Spot, Ambient };

struct Light
{
    ObjectID    id;
    std::string name;
    LightType   type       = LightType::Point;
    Vec3        color      = Vec3(1, 1, 1);
    float       intensity  = 1.0f;

    // Point / Spot
    float       range      = 0.0f;    // 0 = 无限

    // Spot
    float       innerConeAngle = 0.0f;
    float       outerConeAngle = 0.5f;
};

} // namespace mc