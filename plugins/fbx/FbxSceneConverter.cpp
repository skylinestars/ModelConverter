#include "FbxSceneConverter.h"
#include "FbxMeshHelper.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"

#include <fbxsdk.h>
#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <algorithm>

using namespace fbxsdk;

namespace mc {

// ============================================================
// 辅助：FbxAMatrix -> mc::Matrix4
// ============================================================
// Matrix4 采用列主序存储（m[col * 4 + row]），平移在 m[12..14]（第 3 列）。
// FBX 文档说明 FbxAMatrix 的平移在最后一行（row=3）。
// 因此从 FBX 到 mc::Matrix4 需要做一次 row/col 转置映射：
//   mc(row,col) = fbx(col,row)
// 这样可把 FBX row=3 的平移正确映射到 mc 的 col=3。
static Matrix4 ToMatrix4(const FbxAMatrix& fbx)
{
    Matrix4 r;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            r.m[col * 4 + row] = (float)fbx[col][row];
    return r;
}

static FbxAMatrix GetGeometricTransform(FbxNode* fbxNode)
{
    FbxAMatrix geoTrs;
    geoTrs.SetTRS(
        fbxNode->GetGeometricTranslation(FbxNode::eSourcePivot),
        fbxNode->GetGeometricRotation(FbxNode::eSourcePivot),
        fbxNode->GetGeometricScaling(FbxNode::eSourcePivot));
    return geoTrs;
}

static Axis ToAxis(FbxAxisSystem::EUpVector axis)
{
    switch (axis)
    {
        case FbxAxisSystem::eXAxis: return Axis::X;
        case FbxAxisSystem::eYAxis: return Axis::Y;
        case FbxAxisSystem::eZAxis: return Axis::Z;
        default: return Axis::Y;
    }
}

static Handedness ToHandedness(FbxAxisSystem::ECoordSystem coord)
{
    return coord == FbxAxisSystem::eLeftHanded ? Handedness::Left : Handedness::Right;
}

static std::string UnitNameFromScale(double scaleToCm)
{
    if (std::abs(scaleToCm - 100.0) < 1e-6) return "m";
    if (std::abs(scaleToCm - 1.0) < 1e-6) return "cm";
    if (std::abs(scaleToCm - 0.1) < 1e-6) return "mm";
    if (std::abs(scaleToCm - 2.54) < 1e-6) return "inch";
    return "custom";
}

static void FillMetadataFromFbx(FbxScene* fbxScene, Scene& mcScene)
{
    auto& meta = mcScene.metadata;
    auto& gs = fbxScene->GetGlobalSettings();

    const FbxSystemUnit unit = gs.GetSystemUnit();
    double scaleToCm = unit.GetScaleFactor();
    meta.unitScale = static_cast<float>(scaleToCm * 0.01); // cm -> m
    meta.unit = UnitNameFromScale(scaleToCm);

    const FbxAxisSystem axis = gs.GetAxisSystem();
    int upSign = 1;
    FbxAxisSystem::EUpVector upVector = axis.GetUpVector(upSign);
    meta.upAxis = ToAxis(upVector);
    meta.handedness = ToHandedness(axis.GetCoorSystem());
    meta.custom["upAxisSign"] = std::to_string(upSign);

    int frontSign = 1;
    FbxAxisSystem::EFrontVector frontVector = axis.GetFrontVector(frontSign);
    meta.frontAxis = (upVector == FbxAxisSystem::eYAxis) ? Axis::Z : Axis::Y;
    meta.custom["frontAxisParity"] = std::to_string((int)frontVector);
    meta.custom["frontAxisSign"] = std::to_string(frontSign);

    if (FbxDocumentInfo* info = fbxScene->GetSceneInfo())
    {
        const char* app = info->Original_ApplicationName.Get().Buffer();
        const char* ver = info->Original_ApplicationVersion.Get().Buffer();
        if (app && *app)
        {
            meta.asset.generator = app;
            if (ver && *ver)
                meta.asset.generator += std::string(" ") + ver;
        }

        const char* author = info->mAuthor.Buffer();
        if (author && *author) meta.asset.author = author;

        const char* originalFile = info->Original_FileName.Get().Buffer();
        if (meta.asset.sourceFile.empty() && originalFile && *originalFile)
            meta.asset.sourceFile = originalFile;
    }
}

// ============================================================
// GetOrCreateTexture
// ============================================================
ObjectID FbxSceneConverter::GetOrCreateTexture(FbxFileTexture* fbxTex, Scene& mcScene)
{
    if (!fbxTex) return INVALID_ID;

    auto it = m_texCache.find(fbxTex);
    if (it != m_texCache.end()) return it->second;

    Texture& mcTex = mcScene.AddTexture();

    // 使用贴图文件名（不含路径，但保留扩展名）作为纹理名称
    std::string fileName = fbxTex->GetFileName();
    std::filesystem::path texPath(fileName);
    std::string baseName = texPath.filename().string();
    if (baseName.empty()) {
        baseName = fbxTex->GetName();
    }
    mcTex.name = baseName;
    mcTex.uri  = fileName;

    m_texCache[fbxTex] = mcTex.id;
    return mcTex.id;
}

// ============================================================
// GetOrCreateMaterial
// ============================================================
ObjectID FbxSceneConverter::GetOrCreateMaterial(FbxSurfaceMaterial* fbxMat, Scene& mcScene)
{
    if (!fbxMat) return INVALID_ID;

    auto it = m_matCache.find(fbxMat);
    if (it != m_matCache.end()) return it->second;

    Material& mcMat = mcScene.AddMaterial();
    mcMat.name = fbxMat->GetName();

    if (fbxMat->Is<FbxSurfacePhong>())
    {
        auto* phong = static_cast<FbxSurfacePhong*>(fbxMat);
        mcMat.workflow = Material::Phong;

        FbxDouble3 diff = phong->Diffuse.Get();
        mcMat.diffuse = Vec3((float)diff[0], (float)diff[1], (float)diff[2]);
        mcMat.opacity = 1.0f - (float)phong->TransparencyFactor.Get();

        FbxDouble3 amb = phong->Ambient.Get();
        mcMat.ambient = Vec3((float)amb[0], (float)amb[1], (float)amb[2]);

        mcMat.shininess = (float)phong->Shininess.Get();

        // BaseColor from diffuse for PBR approximation
        mcMat.baseColor = Vec4(mcMat.diffuse.x, mcMat.diffuse.y, mcMat.diffuse.z, mcMat.opacity);
        mcMat.metallic   = 0.0f;
        {
            float s = std::clamp(mcMat.shininess, 0.0f, 128.0f);
            mcMat.roughness = std::clamp(std::sqrt(2.0f / (s + 2.0f)), 0.3f, 1.0f);
        }

        // Diffuse texture
        FbxProperty prop = fbxMat->FindProperty(FbxSurfaceMaterial::sDiffuse);
        if (prop.IsValid() && prop.GetSrcObjectCount<FbxFileTexture>() > 0)
        {
            auto* fbxTex = prop.GetSrcObject<FbxFileTexture>(0);
            if (fbxTex)
            {
                mcMat.baseColorTexture.textureId = GetOrCreateTexture(fbxTex, mcScene);
            }
        }
    }
    else if (fbxMat->Is<FbxSurfaceLambert>())
    {
        auto* lambert = static_cast<FbxSurfaceLambert*>(fbxMat);
        mcMat.workflow = Material::Phong;

        FbxDouble3 diff = lambert->Diffuse.Get();
        mcMat.diffuse = Vec3((float)diff[0], (float)diff[1], (float)diff[2]);
        mcMat.opacity = 1.0f - (float)lambert->TransparencyFactor.Get();
        mcMat.baseColor = Vec4(mcMat.diffuse.x, mcMat.diffuse.y, mcMat.diffuse.z, mcMat.opacity);
        mcMat.metallic  = 0.0f;
        mcMat.roughness = 0.8f;
    }
    else
    {
        mcMat.workflow   = Material::MetallicRoughness;
        mcMat.baseColor  = Vec4(1, 1, 1, 1);
        mcMat.metallic   = 0.0f;
        mcMat.roughness  = 0.5f;
    }

    m_matCache[fbxMat] = mcMat.id;
    return mcMat.id;
}

// ============================================================
// ConvertMesh
// ============================================================
ObjectID FbxSceneConverter::ConvertMesh(FbxNode* fbxNode, Scene& mcScene)
{
    FbxMesh* fbxMesh = fbxNode->GetMesh();
    if (!fbxMesh)
        return INVALID_ID;

    int ctrlCount = fbxMesh->GetControlPointsCount();
    int polyCount = fbxMesh->GetPolygonCount();
    if (ctrlCount == 0 || polyCount == 0)
        return INVALID_ID;

    // 扇形三角化 + 顶点解包（委托给 FbxMeshHelper）
    Mesh& mcMesh = mcScene.AddMesh();
    mcMesh.name = fbxNode->GetName();

    VoidResult triResult = FanTriangulateMesh(fbxMesh, GetGeometricTransform(fbxNode), mcMesh);
    if (!triResult.ok)
    {
        Logger::Instance().LogWarn(
            std::string("FbxSceneConverter: FanTriangulateMesh failed for \"") +
            fbxNode->GetName() + "\": " + triResult.error);
        return INVALID_ID;
    }

    // ---- Section 构建（按 material 分组）----
    std::map<int, MeshSection> sectionMap;
    std::map<int, uint32_t>   sectionStart;

    FbxGeometryElementMaterial* matElem = fbxMesh->GetElementMaterial(0);
    int polyCount2 = fbxMesh->GetPolygonCount();

    // 计算 indexOffset（三角形级别的索引偏移）
    uint32_t indexCursor = 0;
    for (int p = 0; p < polyCount2; ++p)
    {
        int polySize = fbxMesh->GetPolygonSize(p);
        if (polySize < 3) continue;

        int matIdx = 0;
        if (matElem && matElem->GetMappingMode() == FbxGeometryElement::eByPolygon)
            matIdx = matElem->GetIndexArray().GetAt(p);

        int triCount = polySize - 2;     // N 边形产生 N-2 个三角形
        uint32_t idxPerTri = 3;

        if (sectionMap.find(matIdx) == sectionMap.end())
            sectionStart[matIdx] = indexCursor;

        sectionMap[matIdx].indexOffset = sectionStart[matIdx];
        sectionMap[matIdx].indexCount  += triCount * idxPerTri;
        indexCursor += triCount * idxPerTri;
    }

    for (auto& [matIdx, sec] : sectionMap)
    {
        if (matIdx < fbxNode->GetMaterialCount())
            sec.materialId = GetOrCreateMaterial(fbxNode->GetMaterial(matIdx), mcScene);
        mcMesh.sections.push_back(sec);
    }

    if (mcMesh.sections.empty() && !mcMesh.indices.empty())
    {
        MeshSection sec;
        sec.indexOffset = 0;
        sec.indexCount  = static_cast<uint32_t>(mcMesh.indices.size());
        if (fbxNode->GetMaterialCount() > 0)
            sec.materialId = GetOrCreateMaterial(fbxNode->GetMaterial(0), mcScene);
        mcMesh.sections.push_back(sec);
    }

    return mcMesh.id;
}

// ============================================================
// ConvertNode（DFS）
// ============================================================
void FbxSceneConverter::ConvertNode(FbxNode* fbxNode, Scene& mcScene, mc::ObjectID parentId)
{
    if (!fbxNode) return;

    mc::ObjectID nodeId = mcScene.AddNode().id;
    mc::Node* mcNode = mcScene.FindNode(nodeId);
    mcNode->name = fbxNode->GetName();

    // 记录 FbxNode -> mc ObjectID 映射，供动画转换使用
    m_nodeMap[fbxNode] = nodeId;

    // ... rest remains the same


    // Local transform（不包含 Geometric TRS）
    // Geometric TRS 仅作用于当前节点几何体，不应影响子节点层级变换。
    // 这里保持节点 localMatrix 与 FBX 节点本地变换一致；
    // Geometric TRS 在 ConvertMesh 中烘焙到顶点/法线。
    FbxAMatrix localTrs = fbxNode->EvaluateLocalTransform();
    mcNode->localMatrix = ToMatrix4(localTrs);

    // 挂载到父节点或作为根节点
    if (parentId != INVALID_ID)
    {
        mc::Node* parent = mcScene.FindNode(parentId);
        if (parent) parent->children.push_back(nodeId);
    }
    else
    {
        mcScene.rootNodes.push_back(nodeId);
    }

    // Mesh 属性
    FbxNodeAttribute* attr = fbxNode->GetNodeAttribute();
    if (attr && attr->GetAttributeType() == FbxNodeAttribute::eMesh)
    {
        ObjectID meshId = ConvertMesh(fbxNode, mcScene);
        if (meshId != INVALID_ID)
        {
            mcNode->meshIds.push_back(meshId);
            m_meshNodeMap[meshId] = fbxNode;  // Phase15: 骨骼绑定用
        }
    }

    // 递归子节点
    for (int i = 0; i < fbxNode->GetChildCount(); ++i)
        ConvertNode(fbxNode->GetChild(i), mcScene, nodeId);
}

// ============================================================
// ConvertAnimStack（Phase14）
// ============================================================
void FbxSceneConverter::ConvertAnimStack(FbxAnimStack* animStack,
                                          FbxScene* fbxScene,
                                          Scene& mcScene)
{
    if (!animStack) return;

    AnimationClip clip;
    clip.id   = mcScene.AllocateId();
    clip.name = animStack->GetName();

    // 设置动画评估上下文
    fbxScene->SetCurrentAnimationStack(animStack);
    FbxAnimEvaluator* evaluator = fbxScene->GetAnimationEvaluator();

    // 获取动画时间范围
    FbxTimeSpan timeSpan = animStack->GetLocalTimeSpan();
    FbxTime startTime = timeSpan.GetStart();
    FbxTime stopTime  = timeSpan.GetStop();

    clip.startTime = startTime.GetSecondDouble();
    clip.endTime   = stopTime.GetSecondDouble();

    if (clip.Duration() <= 0.0)
    {
        Logger::Instance().LogWarn(
            "FbxSceneConverter: animation \"" + clip.name + "\" has zero duration, skipped.");
        return;
    }

    // 遍历所有已映射的节点，收集动画数据
    for (auto& [fbxNode, mcNodeId] : m_nodeMap)
    {
        // 检查该节点是否有任何动画曲线
        FbxAnimLayer* layer = animStack->GetMember<FbxAnimLayer>(0);
        if (!layer) continue;

        FbxAnimCurve* curveTx = fbxNode->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X);
        FbxAnimCurve* curveTy = fbxNode->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y);
        FbxAnimCurve* curveTz = fbxNode->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z);

        FbxAnimCurve* curveRx = fbxNode->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X);
        FbxAnimCurve* curveRy = fbxNode->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y);
        FbxAnimCurve* curveRz = fbxNode->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z);

        FbxAnimCurve* curveSx = fbxNode->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X);
        FbxAnimCurve* curveSy = fbxNode->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y);
        FbxAnimCurve* curveSz = fbxNode->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z);

        // 判断是否有任何动画曲线
        bool hasTranslate = (curveTx || curveTy || curveTz);
        bool hasRotate    = (curveRx || curveRy || curveRz);
        bool hasScale     = (curveSx || curveSy || curveSz);

        if (!hasTranslate && !hasRotate && !hasScale)
            continue;

        // 收集该节点所有动画曲线上的唯一时间点
        std::set<FbxTime> uniqueTimes;
        auto collectTimes = [&](FbxAnimCurve* curve) {
            if (!curve) return;
            int keyCount = curve->KeyGetCount();
            for (int k = 0; k < keyCount; ++k)
                uniqueTimes.insert(curve->KeyGetTime(k));
        };

        collectTimes(curveTx); collectTimes(curveTy); collectTimes(curveTz);
        collectTimes(curveRx); collectTimes(curveRy); collectTimes(curveRz);
        collectTimes(curveSx); collectTimes(curveSy); collectTimes(curveSz);

        if (uniqueTimes.empty()) continue;

        NodeAnimation nodeAnim;
        nodeAnim.nodeId = mcNodeId;

        // 判断插值模式：取第一条有效曲线的插值类型
        auto getInterp = [](FbxAnimCurve* c) -> AnimationInterpolation {
            if (!c || c->KeyGetCount() == 0)
                return AnimationInterpolation::Linear;
            FbxAnimCurveDef::EInterpolationType it = c->KeyGetInterpolation(0);
            switch (it) {
                case FbxAnimCurveDef::eInterpolationConstant:
                    return AnimationInterpolation::Step;
                case FbxAnimCurveDef::eInterpolationCubic:
                    return AnimationInterpolation::CubicSpline;
                default:
                    return AnimationInterpolation::Linear;
            }
        };

        AnimationInterpolation tInterp = getInterp(curveTx ? curveTx : (curveTy ? curveTy : curveTz));
        AnimationInterpolation rInterp = getInterp(curveRx ? curveRx : (curveRy ? curveRy : curveRz));
        AnimationInterpolation sInterp = getInterp(curveSx ? curveSx : (curveSy ? curveSy : curveSz));

        if (hasTranslate)
        {
            nodeAnim.translation.interpolation = tInterp;
            for (const FbxTime& t : uniqueTimes)
            {
                FbxAMatrix evalMatrix = evaluator->GetNodeLocalTransform(fbxNode, t);
                FbxVector4 trans = evalMatrix.GetT();

                KeyFrame<Vec3> kf;
                kf.time  = t.GetSecondDouble();
                kf.value = Vec3((float)trans[0], (float)trans[1], (float)trans[2]);
                nodeAnim.translation.keys.push_back(kf);
            }
        }

        if (hasRotate)
        {
            nodeAnim.rotation.interpolation = rInterp;
            for (const FbxTime& t : uniqueTimes)
            {
                FbxAMatrix evalMatrix = evaluator->GetNodeLocalTransform(fbxNode, t);
                FbxQuaternion q = evalMatrix.GetQ();

                KeyFrame<Quaternion> kf;
                kf.time  = t.GetSecondDouble();
                kf.value = Quaternion((float)q[0], (float)q[1], (float)q[2], (float)q[3]);
                nodeAnim.rotation.keys.push_back(kf);
            }
        }

        if (hasScale)
        {
            nodeAnim.scale.interpolation = sInterp;
            for (const FbxTime& t : uniqueTimes)
            {
                FbxAMatrix evalMatrix = evaluator->GetNodeLocalTransform(fbxNode, t);
                FbxVector4 scale = evalMatrix.GetS();

                KeyFrame<Vec3> kf;
                kf.time  = t.GetSecondDouble();
                kf.value = Vec3((float)scale[0], (float)scale[1], (float)scale[2]);
                nodeAnim.scale.keys.push_back(kf);
            }
        }

        clip.nodeChannels.push_back(std::move(nodeAnim));
    }

    if (!clip.nodeChannels.empty())
        mcScene.animations.push_back(std::move(clip));
}

// ============================================================
// ConvertAnimations（Phase14）
// ============================================================
void FbxSceneConverter::ConvertAnimations(FbxScene* fbxScene, Scene& mcScene)
{
    int animStackCount = fbxScene->GetSrcObjectCount<FbxAnimStack>();
    if (animStackCount == 0) return;

    Logger::Instance().LogInfo(
        "FbxSceneConverter: found " + std::to_string(animStackCount) + " animation stack(s).");

    for (int i = 0; i < animStackCount; ++i)
    {
        FbxAnimStack* animStack = fbxScene->GetSrcObject<FbxAnimStack>(i);
        if (animStack)
            ConvertAnimStack(animStack, fbxScene, mcScene);
    }

    Logger::Instance().LogInfo(
        "FbxSceneConverter: converted " + std::to_string(mcScene.animations.size()) +
        " animation clip(s).");
}

// ============================================================
// Convert（主入口）
// ============================================================
VoidResult FbxSceneConverter::Convert(FbxManager* manager, FbxScene* fbxScene, Scene& mcScene)
{
    VoidResult result;
    result.ok = true;

    // 读取单位/坐标系元数据（转换在 Pipeline 的 UnitConvertPass/AxisConvertPass 中进行）
    FillMetadataFromFbx(fbxScene, mcScene);
    Logger::Instance().LogInfo(
        std::string("FbxSceneConverter: metadata unit=") + mcScene.metadata.unit +
        " unitScale=" + std::to_string(mcScene.metadata.unitScale) +
        " upAxis=" + std::to_string((int)mcScene.metadata.upAxis) +
        " frontAxis=" + std::to_string((int)mcScene.metadata.frontAxis) +
        " handedness=" + std::to_string((int)mcScene.metadata.handedness) +
        " upAxisSign=" + mcScene.metadata.custom["upAxisSign"] +
        " frontAxisParity=" + mcScene.metadata.custom["frontAxisParity"] +
        " frontAxisSign=" + mcScene.metadata.custom["frontAxisSign"]);

    // 三角化在 ConvertMesh 中按需逐个处理，避免全局 Triangulate
    // 遍历整个场景时触发 NURBS/Patch 等非 Mesh 几何体导致 SDK 断言崩溃。

    FbxNode* root = fbxScene->GetRootNode();
    if (!root)
    {
        result.ok    = false;
        result.error = "FBX scene has no root node";
        return result;
    }

    for (int i = 0; i < root->GetChildCount(); ++i)
    {
        FbxNode* child = root->GetChild(i);
        Logger::Instance().LogInfo(
            std::string("FbxSceneConverter: entering root child node \"") +
            (child ? child->GetName() : "null") + "\"");
        ConvertNode(root->GetChild(i), mcScene, INVALID_ID);
    }

    // Phase15: 骨骼蒙皮
    ConvertSkeleton(fbxScene, mcScene);

    // Phase14: 动画转换（须在节点树构建完成之后）
    ConvertAnimations(fbxScene, mcScene);

    Logger::Instance().LogInfo(
        std::string("FbxSceneConverter: converted ") +
        std::to_string(mcScene.MeshCount()) + " mesh(es), " +
        std::to_string(mcScene.MaterialCount()) + " material(s), " +
        std::to_string(mcScene.TextureCount()) + " texture(s)."
    );

    return result;
}

// ============================================================
// ConvertSkeleton（Phase15）
// ============================================================
void FbxSceneConverter::ConvertSkeleton(FbxScene* fbxScene, Scene& mcScene)
{
    // 收集所有带 skin 的 mesh
    for (auto& [meshId, fbxNode] : m_meshNodeMap)
    {
        FbxMesh* fbxMesh = fbxNode->GetMesh();
        if (!fbxMesh) continue;

        // 找 skin deformer
        int skinCount = fbxMesh->GetDeformerCount(FbxDeformer::eSkin);
        if (skinCount == 0) continue;

        FbxSkin* skin = FbxCast<FbxSkin>(fbxMesh->GetDeformer(0, FbxDeformer::eSkin));
        if (!skin) continue;

        int clusterCount = skin->GetClusterCount();
        if (clusterCount == 0) continue;

        // 创建 Skeleton
        Skeleton skel;
        skel.id   = mcScene.AllocateId();
        skel.name = fbxMesh->GetNode() ? fbxMesh->GetNode()->GetName() : "";

        // 创建 Skin
        Skin mcSkin;
        mcSkin.id         = mcScene.AllocateId();
        mcSkin.meshId     = meshId;
        mcSkin.skeletonId = skel.id;

        // Bone → 编号映射
        std::unordered_map<FbxNode*, int> boneIdxMap;

        for (int ci = 0; ci < clusterCount; ++ci)
        {
            FbxCluster* cluster = skin->GetCluster(ci);
            FbxNode*    link    = cluster->GetLink();
            if (!link) continue;

            // 标记骨骼节点
            auto nodeIt = m_nodeMap.find(link);
            if (nodeIt != m_nodeMap.end())
            {
                Node* nd = mcScene.FindNode(nodeIt->second);
                if (nd) nd->type = NodeType::Bone;
            }

            Bone bone;
            bone.id   = mcScene.AllocateId();
            bone.name = link->GetName();

            // inverseBindPose（FbxAMatrix → Matrix4）
            FbxAMatrix ibp;
            cluster->GetTransformLinkMatrix(ibp);  // 骨骼的世界矩阵
            FbxAMatrix geoMatrix;
            cluster->GetTransformMatrix(geoMatrix); // 网格的世界矩阵
            FbxAMatrix invBind = geoMatrix.Inverse() * ibp;

            bone.inverseBindPose = ToMatrix4(invBind);
            bone.parentBoneId = INVALID_ID;

            boneIdxMap[link] = (int)skel.bones.size();
            skel.bones.push_back(std::move(bone));
        }

        // 骨骼父子关系（从 FBX hierarchy 推断）
        for (int ci = 0; ci < clusterCount; ++ci)
        {
            FbxCluster* cluster = skin->GetCluster(ci);
            FbxNode*    link    = cluster->GetLink();
            if (!link) continue;
            FbxNode* parentLink = link->GetParent();
            if (parentLink)
            {
                auto pi = boneIdxMap.find(parentLink);
                auto ci2 = boneIdxMap.find(link);
                if (pi != boneIdxMap.end() && ci2 != boneIdxMap.end())
                    skel.bones[ci2->second].parentBoneId = skel.bones[pi->second].id;
            }
        }

        mcScene.skeletons.push_back(std::move(skel));
        mcScene.skins.push_back(std::move(mcSkin));

        // 蒙皮权重
        Mesh* mcMesh = mcScene.FindMesh(meshId);
        if (!mcMesh || mcMesh->positions.empty()) continue;

        mcMesh->skinInfluences.resize(mcMesh->positions.size());

        for (int ci = 0; ci < clusterCount; ++ci)
        {
            FbxCluster* cluster = skin->GetCluster(ci);
            FbxNode*    link    = cluster->GetLink();
            if (!link) continue;
            auto bi = boneIdxMap.find(link);
            if (bi == boneIdxMap.end()) continue;
            int boneIdx = bi->second;

            int idxCount = cluster->GetControlPointIndicesCount();
            int* indices = cluster->GetControlPointIndices();
            double* weights = cluster->GetControlPointWeights();

            for (int k = 0; k < idxCount; ++k)
            {
                int vtxIdx = indices[k];
                if (vtxIdx >= (int)mcMesh->skinInfluences.size()) continue;
                float w = (float)weights[k];
                if (w > 0.0f)
                    mcMesh->skinInfluences[vtxIdx].push_back({(uint16_t)boneIdx, w});
            }
        }
    }

    if (!mcScene.skeletons.empty())
        Logger::Instance().LogInfo(
            "FbxSceneConverter: converted " + std::to_string(mcScene.skeletons.size()) +
            " skeleton(s), " + std::to_string(mcScene.skins.size()) + " skin(s).");
}

} // namespace mc
