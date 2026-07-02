#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"

#include <unordered_map>
#include <vector>

namespace fbxsdk
{
    class FbxManager;
    class FbxScene;
    class FbxNode;
    class FbxMesh;
    class FbxSkin;
    class FbxSurfaceMaterial;
    class FbxFileTexture;
    class FbxAnimStack;
    class FbxAnimLayer;
} // namespace fbxsdk

namespace mc
{

    // ============================================================
    // FbxSceneConverter
    // ============================================================
    // 将 FbxScene 转换为 mc::Scene。

    class FbxSceneConverter
    {
    public:
        VoidResult Convert(fbxsdk::FbxManager *manager, fbxsdk::FbxScene *fbxScene, Scene &mcScene);

    private:
        void ConvertNode(fbxsdk::FbxNode *fbxNode, Scene &mcScene,
                         mc::ObjectID parentId);

        ObjectID ConvertMesh(fbxsdk::FbxNode *fbxNode, Scene &mcScene);

        ObjectID GetOrCreateMaterial(fbxsdk::FbxSurfaceMaterial *fbxMat, Scene &mcScene);
        ObjectID GetOrCreateTexture(fbxsdk::FbxFileTexture *fbxTex, Scene &mcScene);
        // 从材质指定属性（如 FbxSurfaceMaterial::sNormalMap）读取贴图，
        // 并根据 UVSet 属性还原 texCoord 索引，找不到贴图时返回默认 TextureRef（textureId=INVALID_ID）
        TextureRef ReadMaterialTexture(fbxsdk::FbxSurfaceMaterial *fbxMat, const char *propName, Scene &mcScene);

        void ConvertAnimations(fbxsdk::FbxScene *fbxScene, Scene &mcScene);
        void ConvertAnimStack(fbxsdk::FbxAnimStack *animStack,
                              fbxsdk::FbxScene *fbxScene,
                              Scene &mcScene);
        // 从 animStack 提取 TRS 节点动画通道到 clip.nodeChannels
        void ConvertNodeTrsChannels(fbxsdk::FbxAnimStack *animStack,
                                    fbxsdk::FbxScene *fbxScene,
                                    AnimationClip &clip);
        // 从 animStack 提取 BlendShape 权重通道到 clip.morphChannels
        void ConvertMorphWeightChannels(fbxsdk::FbxAnimStack *animStack,
                                        Scene &mcScene,
                                        AnimationClip &clip);

        void ConvertSkeleton(fbxsdk::FbxScene *fbxScene, Scene &mcScene);
        void ConvertSkeletonForMesh(mc::ObjectID meshId,
                                    fbxsdk::FbxNode *fbxNode,
                                    Scene &mcScene);
        fbxsdk::FbxSkin *GetPrimarySkin(fbxsdk::FbxMesh *fbxMesh) const;
        void BuildSkeletonBones(fbxsdk::FbxSkin *skin,
                                fbxsdk::FbxNode *fbxNode,
                                Scene &mcScene,
                                Skeleton &skeleton,
                                std::unordered_map<fbxsdk::FbxNode *, int> &boneIdxMap);
        void BuildBoneHierarchy(fbxsdk::FbxSkin *skin,
                                const std::unordered_map<fbxsdk::FbxNode *, int> &boneIdxMap,
                                Skeleton &skeleton);
        bool ApplySkinWeights(mc::ObjectID meshId,
                              fbxsdk::FbxSkin *skin,
                              const std::unordered_map<fbxsdk::FbxNode *, int> &boneIdxMap,
                              Scene &mcScene);
        size_t FixLeafBoneNodes(const std::unordered_map<fbxsdk::FbxNode *, int> &boneIdxMap,
                                Scene &mcScene);

        // 检测并剥除根节点上由 FBX 导出器写入的单位补偿 scale
        void StripUnitCompensationScale(Scene &mcScene);

        // FbxSurfaceMaterial 指针 -> mc ObjectID 缓存
        std::unordered_map<fbxsdk::FbxSurfaceMaterial *, mc::ObjectID> m_matCache;
        // FbxFileTexture 指针 -> mc ObjectID 缓存（去重）
        std::unordered_map<fbxsdk::FbxFileTexture *, mc::ObjectID> m_texCache;
        // FbxNode 指针 -> mc ObjectID 映射（动画转换期间使用）
        std::unordered_map<fbxsdk::FbxNode *, mc::ObjectID> m_nodeMap;
        // 临时：meshId → FbxNode（用于骨骼绑定）
        std::unordered_map<mc::ObjectID, fbxsdk::FbxNode *> m_meshNodeMap;
        // 临时：meshId → 控制点到输出顶点的映射（用于蒙皮权重传播）
        // ctrlToOutputMap[ctrlIdx] = 该控制点对应的所有输出顶点索引列表
        std::unordered_map<mc::ObjectID, std::vector<std::vector<uint32_t>>> m_ctrlToOutputMaps;
        // 记录被剥除了单位补偿 scale 的根节点 ID → 被剥除的补偿系数（如 100）
        // ConvertNodeTrsChannels 用它来同步修正 S/T 动画通道
        std::unordered_map<mc::ObjectID, float> m_strippedRootScales;

        // FBX Manager 指针（ConvertMesh 中 FbxGeometryConverter 三角化需要）
        fbxsdk::FbxManager *m_manager = nullptr;
    };

} // namespace mc
