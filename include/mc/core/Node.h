#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>
#include <vector>

namespace mc {

// ============================================================
// Node
// ============================================================
enum class NodeType { Empty, Mesh, Bone, Camera, Light, Helper };

// 仅负责场景树
struct Node
{
    ObjectID              id                = INVALID_ID;
    ObjectID              parent            = INVALID_ID;  // INVALID_ID 表示根
    std::vector<ObjectID> children;

    std::vector<ObjectID> meshIds;                         // 引用 Scene::meshes（支持一个 Node 挂多个 Mesh）

    // NodeType::Camera / Light 时引用对应对象
    ObjectID              cameraId          = INVALID_ID;  // 引用 Scene::cameras
    ObjectID              lightId           = INVALID_ID;  // 引用 Scene::lights
    
    // 预留：实例化相关（USD PointInstancer / FBX Instance / GLTF EXT_mesh_gpu_instancing）
    // 注意：此处 instanceOfNodeId 表示"本 Node 是某个原型的实例"（即 Node 自身是叶子实例）
    // PointInstancer::prototypeNodeId 是"Instancer 引用的模板"，两者职责不同
    ObjectID              instanceOfNodeId  = INVALID_ID;  // 原型节点；INVALID_ID 表示非实例

    std::string           name;
    NodeType              type              = NodeType::Empty;
    Matrix4               localMatrix;  // 相对父节点的变换矩阵
};

} // namespace mc