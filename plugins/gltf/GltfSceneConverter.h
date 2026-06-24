#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace tinygltf { class Model; struct Mesh; struct Material; struct Node; }

namespace mc {

// ============================================================
// GltfSceneConverter
// ============================================================
// 将 tinygltf::Model 转换为 mc::Scene。

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

    void ConvertSkins(const tinygltf::Model& model, Scene& mcScene);
    void ApplySkinWeights(Scene& mcScene);
    void ConvertAnimations(const tinygltf::Model& model, Scene& mcScene);

    // 临时存储：mesh mcId → (GLTF joints[per-vertex], GLTF weights[per-vertex])
    struct RawSkinData {
        int skinIdx;
        std::vector<std::vector<uint16_t>> joints;   // [vertex][influence]
        std::vector<std::vector<float>>   weights;
    };
    std::unordered_map<ObjectID, RawSkinData> m_skinDataMap;
    std::vector<std::pair<ObjectID, int>>     m_meshSkinMap; // (meshId, gltfSkinIdx)

    // tinygltf index -> mc ObjectID 映射（Convert 期间有效）
    std::vector<mc::ObjectID> m_texIdMap;
    std::vector<mc::ObjectID> m_matIdMap;
    std::vector<mc::ObjectID> m_nodeIdMap;  // gltf node index -> mc ObjectID
};

} // namespace mc
