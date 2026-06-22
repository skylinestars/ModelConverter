#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>
#include <vector>

namespace mc {

// ============================================================
// PointInstancer
// ============================================================
struct PointInstancer
{
    ObjectID                  id;
    std::string               name;
    ObjectID                  prototypeNodeId = INVALID_ID;  // 引用哪个原型 Node（其下子树被整体实例化）

    // 每个实例的变换（逐实例）
    std::vector<Vec3>         positions;    // 每个实例的平移
    std::vector<Quaternion>   orientations; // 每个实例的旋转
    std::vector<Vec3>         scales;       // 每个实例的缩放（可选）

    // USD 支持的多原型：可选 protoIndices[i] 指定第 i 个实例选哪个 prototype
    // 第一版不支持：prototypeNodeId 唯一；多原型扩展为 std::vector<ObjectID> prototypeNodeIds
    // + std::vector<int32_t> protoIndices
};

} // namespace mc