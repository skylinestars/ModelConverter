#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"
#include <string>

namespace tinygltf { class Model; struct Mesh; struct Material; struct Node; }

namespace mc {

// ============================================================
// GltfSceneConverter
// ============================================================
// 将 tinygltf::Model 转换为 mc::Scene。
// Phase08 范围：Node / Mesh / Material / Texture（静态模型）。

class GltfSceneConverter
{
public:
    // baseDir：GLB/GLTF 所在目录，用于解析外置纹理路径
    VoidResult Convert(const tinygltf::Model& model,
                       const std::string& baseDir,
                       Scene& mcScene);

private:
    void ConvertTextures(const tinygltf::Model& model,
                         const std::string& baseDir,
                         Scene& mcScene);

    void ConvertMaterials(const tinygltf::Model& model, Scene& mcScene);

    void ConvertMeshes(const tinygltf::Model& model, Scene& mcScene);

    void ConvertNode(const tinygltf::Model& model,
                     int nodeIdx,
                     Scene& mcScene,
                     mc::ObjectID parentId,
                     const std::vector<mc::ObjectID>& meshIdMap);

    // tinygltf index -> mc ObjectID 映射（Convert 期间有效）
    std::vector<mc::ObjectID> m_texIdMap;
    std::vector<mc::ObjectID> m_matIdMap;
};

} // namespace mc
