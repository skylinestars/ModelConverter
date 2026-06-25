#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"

#include <unordered_map>
#include <vector>

namespace fbxsdk {
class FbxManager;
class FbxScene;
class FbxNode;
class FbxMesh;
class FbxSurfaceMaterial;
class FbxFileTexture;
class FbxAnimStack;
class FbxAnimLayer;
} // namespace fbxsdk

namespace mc {

// ============================================================
// FbxSceneConverter
// ============================================================
// 将 FbxScene 转换为 mc::Scene。

class FbxSceneConverter
{
public:
    VoidResult Convert(fbxsdk::FbxManager* manager, fbxsdk::FbxScene* fbxScene, Scene& mcScene);

private:
    void ConvertNode(fbxsdk::FbxNode* fbxNode, Scene& mcScene,
                     mc::ObjectID parentId);

    ObjectID ConvertMesh(fbxsdk::FbxNode* fbxNode, Scene& mcScene);

    ObjectID GetOrCreateMaterial(fbxsdk::FbxSurfaceMaterial* fbxMat, Scene& mcScene);
    ObjectID GetOrCreateTexture(fbxsdk::FbxFileTexture* fbxTex, Scene& mcScene);

    void ConvertAnimations(fbxsdk::FbxScene* fbxScene, Scene& mcScene);
    void ConvertAnimStack(fbxsdk::FbxAnimStack* animStack,
                          fbxsdk::FbxScene* fbxScene,
                          Scene& mcScene);

    void ConvertSkeleton(fbxsdk::FbxScene* fbxScene, Scene& mcScene);

    // FbxSurfaceMaterial 指针 -> mc ObjectID 缓存
    std::unordered_map<fbxsdk::FbxSurfaceMaterial*, mc::ObjectID> m_matCache;
    // FbxFileTexture 指针 -> mc ObjectID 缓存（去重）
    std::unordered_map<fbxsdk::FbxFileTexture*, mc::ObjectID> m_texCache;
    // FbxNode 指针 -> mc ObjectID 映射（动画转换期间使用）
    std::unordered_map<fbxsdk::FbxNode*, mc::ObjectID> m_nodeMap;
    // 临时：meshId → FbxNode（用于骨骼绑定）
    std::unordered_map<mc::ObjectID, fbxsdk::FbxNode*> m_meshNodeMap;
    // 临时：meshId → 控制点到输出顶点的映射（用于蒙皮权重传播）
    // ctrlToOutputMap[ctrlIdx] = 该控制点对应的所有输出顶点索引列表
    std::unordered_map<mc::ObjectID, std::vector<std::vector<uint32_t>>> m_ctrlToOutputMaps;
};

} // namespace mc
