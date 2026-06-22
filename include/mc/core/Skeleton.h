#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>
#include <vector>

namespace mc {

// ============================================================
// Skeleton
// ============================================================

// 仅负责骨骼信息
struct Bone
{
    ObjectID    id;
    std::string name;
    ObjectID    parentBoneId     = INVALID_ID;  // 引用同 Skeleton 内另一个 Bone::id；INVALID_ID 表示根骨
    
    Matrix4     inverseBindPose; 
};

// 仅负责骨骼层级
struct Skeleton
{
    ObjectID          id;
    std::string       name;
    std::vector<Bone> bones;
};

} // namespace mc