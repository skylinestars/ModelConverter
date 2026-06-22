#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>
#include <vector>

namespace mc {

// ============================================================
// Skin
// ============================================================
struct Skin
{
    ObjectID               id;  // Scene::skins 的索引键
    std::string            name;  // 调试用，可空
    ObjectID               skeletonId  = INVALID_ID;  // 引用 Scene::skeletons
    ObjectID               meshId      = INVALID_ID;  // 引用 Scene::meshes 

};

} // namespace mc