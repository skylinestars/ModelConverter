#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"

struct aiScene;
struct aiMesh;
struct aiMaterial;
struct aiNode;

namespace mc {

// ============================================================
// AssimpSceneConverter
// ============================================================
// 将 Assimp 的 aiScene 转换为 mc::Scene。
// Phase06 范围：仅支持静态网格（无 Animation/Skin/Morph）。

class AssimpSceneConverter
{
public:
    // 主入口：将 aiScene 的数据写入 mcScene
    VoidResult Convert(const aiScene* aiSrc, Scene& mcScene);

private:
    void ConvertMeshes(const aiScene* aiSrc, Scene& mcScene);
    void ConvertMaterials(const aiScene* aiSrc, Scene& mcScene);
    void ConvertNode(const aiNode* aiNode, Scene& mcScene,
                     mc::ObjectID parentId,
                     const std::vector<mc::ObjectID>& meshIdMap);

    // Assimp 材质索引 -> mc ObjectID 映射（Convert 期间有效）
    std::vector<mc::ObjectID> m_matIdMap;
};

} // namespace mc
