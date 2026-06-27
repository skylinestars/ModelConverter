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
// 从 FBX 材质中读取漫反射颜色。
// FBX SDK 的类型化访问器（FbxSurfaceLambert::Diffuse）对应内部属性名 "DiffuseColor"，
// 但部分导出器（如 CC4、某些版本 Blender）将颜色写入名为 "Diffuse" 的自定义属性，
// 两者属性名不同，SDK 访问器读不到自定义属性只会返回 SDK 默认值（约 0.8）。
// 解决方案：若类型化访问器结果接近白色/默认值，
// 再按优先级依次尝试 "Diffuse"、"DiffuseColor" 等常见属性名。
static FbxDouble3 ReadDiffuseColor(FbxSurfaceMaterial* mat,
                                    FbxDouble3 typedResult)
{
    // 若类型化结果不是"接近白色/默认"，直接采用（避免覆盖真实白色材质）
    // 判断标准：R、G、B 三通道均 > 0.95 → 视为可能是默认值，再做兜底查找
    const bool looksDefault = (typedResult[0] > 0.95 &&
                                typedResult[1] > 0.95 &&
                                typedResult[2] > 0.95);
    if (!looksDefault)
        return typedResult;

    // 兜底：按优先级尝试常见属性名（"Diffuse" 优先于其他）
    static const char* kNames[] = {
        "Diffuse",          // 部分工具写此名而非 "DiffuseColor"
        "DiffuseColor",     // FBX SDK 标准名，但可能已被 typedResult 读过
        "base_color", "BaseColor", "baseColor",
        "color",      "Color",
        "albedo",     "Albedo",
        nullptr
    };
    for (int i = 0; kNames[i]; ++i)
    {
        FbxProperty prop = mat->FindProperty(kNames[i]);
        if (!prop.IsValid()) continue;
        EFbxType dt = prop.GetPropertyDataType().GetType();
        if (dt == eFbxDouble3)
        {
            FbxDouble3 c = prop.Get<FbxDouble3>();
            // 找到了非白色颜色则采用，找到白色也直接返回（可能真就是白色）
            return c;
        }
        if (dt == eFbxDouble4)
        {
            FbxDouble4 c = prop.Get<FbxDouble4>();
            return FbxDouble3(c[0], c[1], c[2]);
        }
    }
    return typedResult;  // 所有尝试都没找到，返回原始类型化结果
}

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

        // phong->Diffuse 读的是内部属性 "DiffuseColor"；若接近白色，再尝试 "Diffuse" 等属性
        FbxDouble3 diff = ReadDiffuseColor(fbxMat, phong->Diffuse.Get());
        mcMat.diffuse = Vec3((float)diff[0], (float)diff[1], (float)diff[2]);

        // FBX TransparencyFactor 语义：0.0=不透明，1.0=全透明
        // 但 Mixamo 等导出器会设为 1.0 作为默认值，导致 opacity=0
        // 确保 opacity 不低于合理最小值
        float transpFactor = (float)phong->TransparencyFactor.Get();
        mcMat.opacity = 1.0f - transpFactor;
        if (mcMat.opacity <= 0.05f) mcMat.opacity = 1.0f;

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
                mcMat.baseColorTexture.textureId = GetOrCreateTexture(fbxTex, mcScene);
        }
    }
    else if (fbxMat->Is<FbxSurfaceLambert>())
    {
        auto* lambert = static_cast<FbxSurfaceLambert*>(fbxMat);
        mcMat.workflow = Material::Phong;

        FbxDouble3 diff = ReadDiffuseColor(fbxMat, lambert->Diffuse.Get());
        mcMat.diffuse = Vec3((float)diff[0], (float)diff[1], (float)diff[2]);

        mcMat.opacity = 1.0f - (float)lambert->TransparencyFactor.Get();
        if (mcMat.opacity <= 0.05f) mcMat.opacity = 1.0f;

        mcMat.baseColor = Vec4(mcMat.diffuse.x, mcMat.diffuse.y, mcMat.diffuse.z, mcMat.opacity);
        mcMat.metallic  = 0.0f;
        mcMat.roughness = 0.8f;
    }
    else
    {
        // 非标准材质（不继承自 FbxSurfaceLambert），如 3ds Max Physical Material、
        // Maya Standard Surface、CC4 自定义着色器等。
        // 记录材质类名辅助诊断，并尝试从常见属性名中读取颜色。
        Logger::Instance().LogInfo(
            std::string("FbxSceneConverter: material \"") + fbxMat->GetName() +
            "\" is non-Lambert (class=" + fbxMat->GetClassId().GetName() +
            "), reading color from custom properties");

        mcMat.workflow  = Material::MetallicRoughness;
        mcMat.metallic  = 0.0f;
        mcMat.roughness = 0.5f;

        // 尝试常见属性名读取颜色（ReadDiffuseColor 中以 (1,1,1) 触发兜底搜索）
        FbxDouble3 c = ReadDiffuseColor(fbxMat, FbxDouble3(1, 1, 1));
        mcMat.baseColor = Vec4((float)c[0], (float)c[1], (float)c[2], 1.0f);
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

    // 若 FBX 网格没有显式法线（部分导出器只写几何体，依赖查看器自行计算法线）
    // 则调用 FBX SDK 从面法线+光滑组生成，与 FBX Review 的行为保持一致。
    // 不带参数默认：pOverwrite=false（不覆盖已有法线），pByCtrlPoint=false（逐多边形顶点）
    if (fbxMesh->GetElementNormalCount() == 0)
    {
        fbxMesh->GenerateNormals();
        Logger::Instance().LogInfo(
            std::string("FbxSceneConverter: mesh \"") + fbxNode->GetName() +
            "\" has no normals, generated from geometry");
    }

    // 扇形三角化 + 顶点解包（委托给 FbxMeshHelper）
    Mesh& mcMesh = mcScene.AddMesh();
    mcMesh.name = fbxNode->GetName();

    // 构建控制点到输出顶点的映射（后续蒙皮权重传播需要）
    // GeometricTransform 烘焙到顶点：与 ConvertNode 的 localMatrix（不含 geo）一致
    std::vector<std::vector<uint32_t>> ctrlToOutputMap;
    VoidResult triResult = FanTriangulateMesh(fbxMesh, GetGeometricTransform(fbxNode), mcMesh, &ctrlToOutputMap);
    if (!triResult.ok)
    {
        Logger::Instance().LogWarn(
            std::string("FbxSceneConverter: FanTriangulateMesh failed for \"") +
            fbxNode->GetName() + "\": " + triResult.error);
        return INVALID_ID;
    }

    // 存储映射供 ConvertSkeleton 使用
    m_ctrlToOutputMaps[mcMesh.id] = std::move(ctrlToOutputMap);

    // ---- Section 构建（按材质排序后重建 index buffer）----
    // FBX 中不同材质的面往往交错排列（如面序 mat0, mat1, mat0...）。
    // GLTF primitive 要求每个 section 的 indices 必须是连续范围，
    // 所以必须先收集每个材质对应的三角形索引，重建连续的 index buffer，
    // 再设定 section 的 indexOffset / indexCount，否则末尾的三角形会丢失。

    FbxGeometryElementMaterial* matElem = fbxMesh->GetElementMaterial(0);
    int polyCount2 = fbxMesh->GetPolygonCount();

    // 第一步：逐多边形将三角形索引按材质分桶（保持原始顺序，无需排序）
    std::map<int, std::vector<uint32_t>> perMatIndices;
    {
        int triIdx = 0;  // 全局三角形计数（与 FanTriangulateMesh 的输出顺序严格对应）
        for (int p = 0; p < polyCount2; ++p)
        {
            int polySize = fbxMesh->GetPolygonSize(p);
            if (polySize < 3) continue;  // 与 FanTriangulateMesh 保持相同跳过条件

            int matIdx = 0;
            if (matElem && matElem->GetMappingMode() == FbxGeometryElement::eByPolygon)
                matIdx = matElem->GetIndexArray().GetAt(p);

            int triCount = polySize - 2;
            for (int t = 0; t < triCount; ++t)
            {
                uint32_t base = static_cast<uint32_t>(triIdx * 3);
                perMatIndices[matIdx].push_back(mcMesh.indices[base + 0]);
                perMatIndices[matIdx].push_back(mcMesh.indices[base + 1]);
                perMatIndices[matIdx].push_back(mcMesh.indices[base + 2]);
                ++triIdx;
            }
        }
    }

    // 第二步：重建连续 index buffer 并生成 sections
    mcMesh.indices.clear();
    for (auto& [matIdx, idxList] : perMatIndices)
    {
        MeshSection sec;
        sec.indexOffset = static_cast<uint32_t>(mcMesh.indices.size());
        sec.indexCount  = static_cast<uint32_t>(idxList.size());
        if (matIdx < fbxNode->GetMaterialCount())
            sec.materialId = GetOrCreateMaterial(fbxNode->GetMaterial(matIdx), mcScene);
        mcMesh.indices.insert(mcMesh.indices.end(), idxList.begin(), idxList.end());
        mcMesh.sections.push_back(sec);
    }

    // fallback：无材质分配信息时生成单 section
    if (mcMesh.sections.empty() && !mcMesh.indices.empty())
    {
        MeshSection sec;
        sec.indexOffset = 0;
        sec.indexCount  = static_cast<uint32_t>(mcMesh.indices.size());
        if (fbxNode->GetMaterialCount() > 0)
            sec.materialId = GetOrCreateMaterial(fbxNode->GetMaterial(0), mcScene);
        mcMesh.sections.push_back(sec);
    }

    // ---- BlendShape（Morph Target）----
    const auto& ctoMap = m_ctrlToOutputMaps[mcMesh.id];
    const FbxVector4* basePts = fbxMesh->GetControlPoints();
    FbxAMatrix geoTrs = GetGeometricTransform(fbxNode);
    int bsDeformerCount = fbxMesh->GetDeformerCount(FbxDeformer::eBlendShape);
    for (int di = 0; di < bsDeformerCount; ++di)
    {
        FbxBlendShape* blendShape = static_cast<FbxBlendShape*>(
            fbxMesh->GetDeformer(di, FbxDeformer::eBlendShape));
        if (!blendShape) continue;

        int channelCount = blendShape->GetBlendShapeChannelCount();
        for (int ci = 0; ci < channelCount; ++ci)
        {
            FbxBlendShapeChannel* channel = blendShape->GetBlendShapeChannel(ci);
            if (!channel) continue;

            int targetCount = channel->GetTargetShapeCount();
            if (targetCount == 0) continue;
            // 取全权重目标形状
            FbxShape* shape = channel->GetTargetShape(targetCount - 1);
            if (!shape) continue;

            MorphTarget mt;
            mt.name = channel->GetName();
            mt.positionDeltas.resize(mcMesh.positions.size(), Vec3(0.0f, 0.0f, 0.0f));

            int sparseCount = shape->GetControlPointIndicesCount();
            if (sparseCount > 0)
            {
                // 稀疏模式：只有部分控制点有位移
                const int* spIndices = shape->GetControlPointIndices();
                const FbxVector4* dispPts = shape->GetControlPoints();
                for (int si = 0; si < sparseCount; ++si)
                {
                    int ctrlIdx = spIndices[si];
                    if (ctrlIdx < 0 || ctrlIdx >= (int)ctoMap.size()) continue;
                    FbxVector4 base = geoTrs.MultT(basePts[ctrlIdx]);
                    FbxVector4 disp = geoTrs.MultT(dispPts[si]);
                    Vec3 d((float)(disp[0]-base[0]), (float)(disp[1]-base[1]), (float)(disp[2]-base[2]));
                    for (uint32_t outIdx : ctoMap[ctrlIdx])
                        if (outIdx < (uint32_t)mt.positionDeltas.size())
                            mt.positionDeltas[outIdx] = d;
                }
            }
            else
            {
                // 全量模式：所有控制点都列出
                const FbxVector4* dispPts = shape->GetControlPoints();
                int shapeCtrlCount = shape->GetControlPointsCount();
                for (int pi = 0; pi < shapeCtrlCount && pi < (int)ctoMap.size(); ++pi)
                {
                    FbxVector4 base = geoTrs.MultT(basePts[pi]);
                    FbxVector4 disp = geoTrs.MultT(dispPts[pi]);
                    Vec3 d((float)(disp[0]-base[0]), (float)(disp[1]-base[1]), (float)(disp[2]-base[2]));
                    for (uint32_t outIdx : ctoMap[pi])
                        if (outIdx < (uint32_t)mt.positionDeltas.size())
                            mt.positionDeltas[outIdx] = d;
                }
            }
            mcMesh.morphTargets.push_back(std::move(mt));
        }
    }
    if (!mcMesh.morphTargets.empty())
        Logger::Instance().LogInfo(
            "FbxSceneConverter: mesh \"" + mcMesh.name + "\" morphTargets=" +
            std::to_string(mcMesh.morphTargets.size()));

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


    // Local transform（与原始行为一致，不合并 GeometricTransform）
    FbxAMatrix localTrs = fbxNode->EvaluateLocalTransform();
    mcNode->localMatrix = ToMatrix4(localTrs);

    // 挂载到父节点或作为根节点
    if (parentId != INVALID_ID)
    {
        mc::Node* parent = mcScene.FindNode(parentId);
        if (parent)
        {
            mcNode->parent = parentId;
            parent->children.push_back(nodeId);
        }
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
// ConvertNodeTrsChannels —— 提取 TRS 节点动画到 clip.nodeChannels
// ============================================================
void FbxSceneConverter::ConvertNodeTrsChannels(FbxAnimStack* animStack,
                                                FbxScene* fbxScene,
                                                AnimationClip& clip)
{
    FbxAnimLayer* layer = animStack->GetMember<FbxAnimLayer>(0);
    if (!layer) return;

    FbxAnimEvaluator* evaluator = fbxScene->GetAnimationEvaluator();

    auto getInterp = [](FbxAnimCurve* c) -> AnimationInterpolation {
        if (!c || c->KeyGetCount() == 0)
            return AnimationInterpolation::Linear;
        switch (c->KeyGetInterpolation(0)) {
            case FbxAnimCurveDef::eInterpolationConstant: return AnimationInterpolation::Step;
            case FbxAnimCurveDef::eInterpolationCubic:   return AnimationInterpolation::CubicSpline;
            default:                                       return AnimationInterpolation::Linear;
        }
    };

    for (auto& [fbxNode, mcNodeId] : m_nodeMap)
    {
        FbxAnimCurve* curveTx = fbxNode->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X);
        FbxAnimCurve* curveTy = fbxNode->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y);
        FbxAnimCurve* curveTz = fbxNode->LclTranslation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z);
        FbxAnimCurve* curveRx = fbxNode->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X);
        FbxAnimCurve* curveRy = fbxNode->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y);
        FbxAnimCurve* curveRz = fbxNode->LclRotation.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z);
        FbxAnimCurve* curveSx = fbxNode->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X);
        FbxAnimCurve* curveSy = fbxNode->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y);
        FbxAnimCurve* curveSz = fbxNode->LclScaling.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z);

        bool hasTranslate = (curveTx || curveTy || curveTz);
        bool hasRotate    = (curveRx || curveRy || curveRz);
        bool hasScale     = (curveSx || curveSy || curveSz);
        if (!hasTranslate && !hasRotate && !hasScale) continue;

        // 收集所有曲线上的唯一时间点
        std::set<FbxTime> uniqueTimes;
        auto collectTimes = [&](FbxAnimCurve* curve) {
            if (!curve) return;
            for (int k = 0; k < curve->KeyGetCount(); ++k)
                uniqueTimes.insert(curve->KeyGetTime(k));
        };
        collectTimes(curveTx); collectTimes(curveTy); collectTimes(curveTz);
        collectTimes(curveRx); collectTimes(curveRy); collectTimes(curveRz);
        collectTimes(curveSx); collectTimes(curveSy); collectTimes(curveSz);
        if (uniqueTimes.empty()) continue;

        NodeAnimation nodeAnim;
        nodeAnim.nodeId = mcNodeId;

        if (hasTranslate)
        {
            nodeAnim.translation.interpolation = getInterp(curveTx ? curveTx : (curveTy ? curveTy : curveTz));
            for (const FbxTime& t : uniqueTimes)
            {
                FbxVector4 trans = evaluator->GetNodeLocalTransform(fbxNode, t).GetT();
                KeyFrame<Vec3> kf;
                kf.time  = t.GetSecondDouble();
                kf.value = Vec3((float)trans[0], (float)trans[1], (float)trans[2]);
                nodeAnim.translation.keys.push_back(kf);
            }
        }
        if (hasRotate)
        {
            nodeAnim.rotation.interpolation = getInterp(curveRx ? curveRx : (curveRy ? curveRy : curveRz));
            for (const FbxTime& t : uniqueTimes)
            {
                FbxQuaternion q = evaluator->GetNodeLocalTransform(fbxNode, t).GetQ();
                KeyFrame<Quaternion> kf;
                kf.time  = t.GetSecondDouble();
                kf.value = Quaternion((float)q[0], (float)q[1], (float)q[2], (float)q[3]);
                nodeAnim.rotation.keys.push_back(kf);
            }
        }
        if (hasScale)
        {
            nodeAnim.scale.interpolation = getInterp(curveSx ? curveSx : (curveSy ? curveSy : curveSz));
            for (const FbxTime& t : uniqueTimes)
            {
                FbxVector4 scale = evaluator->GetNodeLocalTransform(fbxNode, t).GetS();
                KeyFrame<Vec3> kf;
                kf.time  = t.GetSecondDouble();
                kf.value = Vec3((float)scale[0], (float)scale[1], (float)scale[2]);
                nodeAnim.scale.keys.push_back(kf);
            }
        }
        // 若该节点曾被 StripUnitCompensationScale 剥除补偿 scale，
        // 需同步将 S/T 动画 keys 也除以相同系数，否则动画会把节点拉回 S=100
        auto strippedIt = m_strippedRootScales.find(mcNodeId);
        if (strippedIt != m_strippedRootScales.end())
        {
            float comp = strippedIt->second;
            for (auto& kf : nodeAnim.scale.keys)
                { kf.value.x /= comp; kf.value.y /= comp; kf.value.z /= comp; }
            for (auto& kf : nodeAnim.translation.keys)
                { kf.value.x /= comp; kf.value.y /= comp; kf.value.z /= comp; }
        }

        clip.nodeChannels.push_back(std::move(nodeAnim));
    }
}

// ============================================================
// ConvertMorphWeightChannels —— 提取 BlendShape 权重曲线到 clip.morphChannels
// ============================================================
void FbxSceneConverter::ConvertMorphWeightChannels(FbxAnimStack* animStack,
                                                    Scene& mcScene,
                                                    AnimationClip& clip)
{
    FbxAnimLayer* layer = animStack->GetMember<FbxAnimLayer>(0);
    if (!layer) return;

    for (auto& mcMesh : mcScene.meshes)
    {
        if (mcMesh.morphTargets.empty()) continue;

        auto meshNodeIt = m_meshNodeMap.find(mcMesh.id);
        if (meshNodeIt == m_meshNodeMap.end()) continue;
        FbxMesh* fbxMeshPtr = meshNodeIt->second->GetMesh();
        if (!fbxMeshPtr) continue;

        int bsDeformerCount = fbxMeshPtr->GetDeformerCount(FbxDeformer::eBlendShape);
        uint32_t morphIdx = 0;
        for (int di = 0; di < bsDeformerCount; ++di)
        {
            FbxBlendShape* blendShape = static_cast<FbxBlendShape*>(
                fbxMeshPtr->GetDeformer(di, FbxDeformer::eBlendShape));
            if (!blendShape) continue;

            int channelCount = blendShape->GetBlendShapeChannelCount();
            for (int ci = 0; ci < channelCount; ++ci, ++morphIdx)
            {
                FbxBlendShapeChannel* channel = blendShape->GetBlendShapeChannel(ci);
                if (!channel) continue;

                FbxAnimCurve* curve = channel->DeformPercent.GetCurve(layer);
                if (!curve || curve->KeyGetCount() == 0) continue;

                AnimationInterpolation interp = AnimationInterpolation::Linear;
                switch (curve->KeyGetInterpolation(0)) {
                    case FbxAnimCurveDef::eInterpolationConstant: interp = AnimationInterpolation::Step; break;
                    case FbxAnimCurveDef::eInterpolationCubic:   interp = AnimationInterpolation::CubicSpline; break;
                    default: break;
                }

                MorphAnimation morphAnim;
                morphAnim.meshId     = mcMesh.id;
                morphAnim.morphIndex = morphIdx;
                morphAnim.weights.interpolation = interp;

                int keyCount = curve->KeyGetCount();
                for (int k = 0; k < keyCount; ++k)
                {
                    KeyFrame<float> kf;
                    kf.time  = curve->KeyGetTime(k).GetSecondDouble();
                    kf.value = (float)(curve->KeyGetValue(k) / 100.0);  // FBX 0-100% → 0-1
                    morphAnim.weights.keys.push_back(kf);
                }
                clip.morphChannels.push_back(std::move(morphAnim));
            }
        }
    }
}

// ============================================================
// ConvertAnimStack —— 编排层：建 clip → 提取 TRS + Morph → 推入 Scene
// ============================================================
void FbxSceneConverter::ConvertAnimStack(FbxAnimStack* animStack,
                                          FbxScene* fbxScene,
                                          Scene& mcScene)
{
    if (!animStack) return;

    AnimationClip clip;
    clip.id   = mcScene.AllocateId();
    clip.name = animStack->GetName();

    fbxScene->SetCurrentAnimationStack(animStack);

    FbxTimeSpan timeSpan = animStack->GetLocalTimeSpan();
    clip.startTime = timeSpan.GetStart().GetSecondDouble();
    clip.endTime   = timeSpan.GetStop().GetSecondDouble();

    if (clip.Duration() <= 0.0)
    {
        Logger::Instance().LogWarn(
            "FbxSceneConverter: animation \"" + clip.name + "\" has zero duration, skipped.");
        return;
    }

    ConvertNodeTrsChannels(animStack, fbxScene, clip);
    ConvertMorphWeightChannels(animStack, mcScene, clip);

    std::string logMsg =
        "FbxSceneConverter::ConvertAnimStack: clip=\"" + clip.name + "\"" +
        " duration={" + std::to_string(clip.startTime) + "s ~ " + std::to_string(clip.endTime) + "s}" +
        " nodeChannels=" + std::to_string(clip.nodeChannels.size()) +
        " morphChannels=" + std::to_string(clip.morphChannels.size());

    if (!clip.nodeChannels.empty() || !clip.morphChannels.empty())
        mcScene.animations.push_back(std::move(clip));

    Logger::Instance().LogInfo(logMsg);
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

    // 给每个动画 clip 打印通道摘要
    for (int i = 0; i < animStackCount; ++i)
    {
        FbxAnimStack* animStack = fbxScene->GetSrcObject<FbxAnimStack>(i);
        if (animStack)
            ConvertAnimStack(animStack, fbxScene, mcScene);
    }

    // 汇总日志
    for (const auto& clip : mcScene.animations)
    {
        int boneChannels = 0;
        int nonBoneChannels = 0;
        for (const auto& ch : clip.nodeChannels)
        {
            Node* nd = mcScene.FindNode(ch.nodeId);
            if (nd && nd->type == NodeType::Bone)
                ++boneChannels;
            else
                ++nonBoneChannels;
        }
        Logger::Instance().LogInfo(
            "  AnimationClip \"" + clip.name + "\": " +
            std::to_string(clip.nodeChannels.size()) + " channels " +
            "(bone=" + std::to_string(boneChannels) + " nonBone=" + std::to_string(nonBoneChannels) + ")");
    }

    Logger::Instance().LogInfo(
        "FbxSceneConverter: converted " + std::to_string(mcScene.animations.size()) +
        " animation clip(s).");
}

// ============================================================
// StripUnitCompensationScale
// ============================================================
// 部分 DCC 工具（Blender 关闭 Apply Unit）导出 FBX 时：
//   - GlobalSettings 声明单位 = cm（GetScaleFactor=1.0）
//   - 但顶点坐标实际是 Blender 米制（1 unit = 1 m），未乘以 100
//   - 为让 cm 查看器正确显示，在根节点上写入 scale=100（1/unitScale）作补偿
//
// 识别条件：至少一个根节点各轴 scale 均约等于 1/unitScale（如 100），且 unitScale < 1（非米制文件）
// 处理（两阶段）：
//   Phase1: 检测是否存在补偿根节点（scale ≈ expectedCompScale）
//   Phase2: 若检测到，对【所有】根节点的旋转-缩放列除以 expectedCompScale
//     - 纯补偿节点（scale=100）→ scale 恢复为 1
//     - 混合节点（scale = userScale×100，如 2.289）→ scale 恢复为 userScale（如 0.02289）
//   并将 meta.unitScale 设为 1.0——顶点坐标已是米制，UnitConvertPass 无需再缩放
void FbxSceneConverter::StripUnitCompensationScale(Scene& mcScene)
{
    const float unitScale = mcScene.metadata.unitScale;
    // 仅对 unitScale < 1 的文件（cm / mm 等）检测，meter 文件（unitScale=1）跳过
    if (unitScale >= 1.0f - 1e-4f) return;

    const float expectedCompScale = 1.0f / unitScale;  // 如 cm → 100
    const float tolerance         = expectedCompScale * 0.05f;

    // Phase 1: 检测是否存在单位补偿根节点
    bool hasCompensation = false;
    for (ObjectID rootId : mcScene.rootNodes)
    {
        Node* nd = mcScene.FindNode(rootId);
        if (!nd) continue;

        const float* m = nd->localMatrix.m;
        float sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        float sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
        float sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);

        if (std::abs(sx - expectedCompScale) < tolerance &&
            std::abs(sy - expectedCompScale) < tolerance &&
            std::abs(sz - expectedCompScale) < tolerance)
        {
            hasCompensation = true;
            break;
        }
    }

    if (!hasCompensation) return;

    // Phase 2: 对所有根节点的旋转-缩放列及平移列除以 expectedCompScale
    // 旋转-缩放列（col 0-2）：消除比例补偿，scale 从 userScale×100 恢复为 userScale
    // 平移列（col 3，m[12..14]）：FBX 中平移量与缩放量同单位，须一并换算为米
    for (ObjectID rootId : mcScene.rootNodes)
    {
        Node* nd = mcScene.FindNode(rootId);
        if (!nd) continue;

        float* m = nd->localMatrix.m;
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                m[col * 4 + row] /= expectedCompScale;
        m[12] /= expectedCompScale;
        m[13] /= expectedCompScale;
        m[14] /= expectedCompScale;

        m_strippedRootScales[nd->id] = expectedCompScale;
        Logger::Instance().LogInfo(
            std::string("FbxSceneConverter: stripped unit-compensation scale (") +
            std::to_string(expectedCompScale) +
            ") from root node \"" + nd->name + "\"");
    }

    // 顶点坐标与节点平移量已是米制，告知 UnitConvertPass 无需再缩放
    mcScene.metadata.unitScale = 1.0f;
    mcScene.metadata.unit      = "m";
    Logger::Instance().LogInfo(
        "FbxSceneConverter: unit-compensation scale stripped; "
        "meta.unitScale reset to 1.0 (positions already in meters)");
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
        ConvertNode(root->GetChild(i), mcScene, INVALID_ID);
    }

    // 检测并剥除根节点上的"单位补偿 scale"
    // 部分 FBX 导出器（如 Blender 关闭 Apply Unit 时）不转换顶点坐标，
    // 而是在根节点上写入 scale=1/unitScale（如 cm 文件写 scale=100）来补偿单位差。
    // 我们读出的顶点坐标此时已是真实米制值，不需要 UnitConvertPass 再缩放；
    // 但 scale=100 若原样写入 GLB，会导致 Blender 等工具显示的骨骼可视化放大 100 倍。
    StripUnitCompensationScale(mcScene);

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

        Logger::Instance().LogInfo(
            "FbxSceneConverter::ConvertSkeleton: mesh=\"" + std::string(fbxNode->GetName()) +
            "\" meshId=" + std::to_string(meshId) +
            " clusterCount=" + std::to_string(clusterCount));

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
            cluster->GetTransformLinkMatrix(ibp);  // 骨骼的世界矩阵 L_bind
            FbxAMatrix geoMatrix;
            cluster->GetTransformMatrix(geoMatrix); // 网格的世界矩阵 M_bind（可能含 GeometricTransform）

            // 逆绑定矩阵 = L_bind^(-1) * M_bind_without_geo
            // 顶点已在 FanTriangulateMesh 中烘焙了 GeometricTransform
            // 所以需要从 M_bind 中移除 GeometricTransform，避免双重应用
            FbxAMatrix meshGeo = GetGeometricTransform(fbxNode);
            FbxAMatrix invBind = ibp.Inverse() * (geoMatrix * meshGeo.Inverse());

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

        Logger::Instance().LogInfo(
            "  Skeleton boneCount=" + std::to_string(skel.bones.size()));

        mcScene.skeletons.push_back(std::move(skel));
        mcScene.skins.push_back(std::move(mcSkin));

        // 蒙皮权重
        Mesh* mcMesh = mcScene.FindMesh(meshId);
        if (!mcMesh || mcMesh->positions.empty()) continue;

        mcMesh->skinInfluences.resize(mcMesh->positions.size());

        // 查找该 mesh 的控制点到输出顶点映射
        auto ctrlMapIt = m_ctrlToOutputMaps.find(meshId);
        const auto& ctrlToOutput = (ctrlMapIt != m_ctrlToOutputMaps.end())
            ? ctrlMapIt->second
            : std::vector<std::vector<uint32_t>>{};

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
                int ctrlIdx = indices[k];
                float w = (float)weights[k];
                if (w <= 0.0f) continue;

                // 使用控制点到输出顶点的映射，将权重传播到所有从该控制点产生的输出顶点
                if (ctrlIdx >= 0 && ctrlIdx < (int)ctrlToOutput.size() && !ctrlToOutput[ctrlIdx].empty())
                {
                    for (uint32_t outIdx : ctrlToOutput[ctrlIdx])
                    {
                        if (outIdx < mcMesh->skinInfluences.size())
                            mcMesh->skinInfluences[outIdx].push_back({(uint16_t)boneIdx, w});
                    }
                }
                else
                {
                    // 回退：没有映射时直接用控制点索引（兼容旧逻辑）
                    if (ctrlIdx >= 0 && ctrlIdx < (int)mcMesh->skinInfluences.size())
                        mcMesh->skinInfluences[ctrlIdx].push_back({(uint16_t)boneIdx, w});
                }
            }
        }

        // 统计蒙皮权重
        size_t vertexCount = mcMesh->positions.size();
        size_t verticesWithSkin = 0;
        size_t maxInfPerVtx = 0;
        float totalWeight = 0.0f;
        size_t totalInfs = 0;
        for (const auto& infs : mcMesh->skinInfluences)
        {
            if (!infs.empty())
            {
                ++verticesWithSkin;
                maxInfPerVtx = std::max(maxInfPerVtx, infs.size());
                for (const auto& inf : infs)
                    totalWeight += inf.weight;
                totalInfs += infs.size();
            }
        }

        // 统计控制点到输出顶点的映射膨胀率
        size_t ctrlCount = ctrlToOutput.size();
        size_t ctrlWithMultiOut = 0;
        if (ctrlCount > 0)
        {
            for (const auto& outputs : ctrlToOutput)
                if (outputs.size() > 1) ++ctrlWithMultiOut;
        }
        Logger::Instance().LogInfo(
            "  CtrlMapping: ctrlCount=" + std::to_string(ctrlCount) +
            " expansionRatio=" + (ctrlCount > 0 ? std::to_string((float)vertexCount / ctrlCount) : "N/A") +
            " ctrlWithMultiOut=" + std::to_string(ctrlWithMultiOut));

        Logger::Instance().LogInfo(
            "  SkinWeight: vertices=" + std::to_string(vertexCount) +
            " verticesWithSkin=" + std::to_string(verticesWithSkin) +
            " maxInfPerVtx=" + std::to_string(maxInfPerVtx) +
            " totalInfluences=" + std::to_string(totalInfs) +
            " avgWeight=" + (totalInfs > 0 ? std::to_string(totalWeight / totalInfs) : "0"));
    }

    if (!mcScene.skeletons.empty())
        Logger::Instance().LogInfo(
            "FbxSceneConverter: converted " + std::to_string(mcScene.skeletons.size()) +
            " skeleton(s), " + std::to_string(mcScene.skins.size()) + " skin(s).");
}

} // namespace mc
