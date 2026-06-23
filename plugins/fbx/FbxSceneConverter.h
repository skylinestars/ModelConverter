#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"

#include <unordered_map>

namespace fbxsdk {
class FbxManager;
class FbxScene;
class FbxNode;
class FbxMesh;
class FbxSurfaceMaterial;
} // namespace fbxsdk

namespace mc {

// ============================================================
// FbxSceneConverter
// ============================================================
// 将 FbxScene 转换为 mc::Scene。
// Phase09 范围：静态模型 Node / Mesh / Material / Texture。
// 禁止：Animation / Skeleton / Skin / BlendShape。

class FbxSceneConverter
{
public:
    VoidResult Convert(fbxsdk::FbxManager* manager, fbxsdk::FbxScene* fbxScene, Scene& mcScene);

private:
    void ConvertNode(fbxsdk::FbxNode* fbxNode, Scene& mcScene,
                     mc::ObjectID parentId);

    ObjectID ConvertMesh(fbxsdk::FbxNode* fbxNode, Scene& mcScene);

    ObjectID GetOrCreateMaterial(fbxsdk::FbxSurfaceMaterial* fbxMat, Scene& mcScene);

    // FbxSurfaceMaterial 指针 -> mc ObjectID 缓存
    std::unordered_map<fbxsdk::FbxSurfaceMaterial*, mc::ObjectID> m_matCache;
};

} // namespace mc
