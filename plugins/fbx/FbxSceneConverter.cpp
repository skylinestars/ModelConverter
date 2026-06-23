#include "FbxSceneConverter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
#include "mc/common/Logger.h"

#include <fbxsdk.h>
#include <map>
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
    if (!fbxMesh) return INVALID_ID;

    // 确保已三角化
    if (!fbxMesh->IsTriangleMesh())
    {
        FbxGeometryConverter conv(fbxMesh->GetScene()->GetFbxManager());
        FbxNodeAttribute* attr = conv.Triangulate(fbxMesh, true);
        if (attr && attr->Is<FbxMesh>())
            fbxMesh = static_cast<FbxMesh*>(attr);
    }

    Mesh& mcMesh = mcScene.AddMesh();
    mcMesh.name = fbxNode->GetName();

    int polyCount = fbxMesh->GetPolygonCount();
    int ctrlCount = fbxMesh->GetControlPointsCount();
    FbxVector4* ctrlPts = fbxMesh->GetControlPoints();
    FbxAMatrix geoTrs = GetGeometricTransform(fbxNode);

    // 获取法线元素
    FbxGeometryElementNormal* normalElem = fbxMesh->GetElementNormal(0);
    // 获取 UV 元素
    FbxGeometryElementUV* uvElem = fbxMesh->GetElementUV(0);
    if (uvElem) mcMesh.uvs.emplace_back();

    // 展开为非索引顶点（对齐法线/UV 映射模式）
    std::unordered_map<uint64_t, uint32_t> vertexMap;

    auto packKey = [](int ctrl, int poly, int vert) -> uint64_t {
        return (uint64_t)ctrl << 32 | (uint64_t)poly << 16 | (uint64_t)vert;
    };

    // Section per material — 用 ordered map 保证按 matIdx 顺序遍历
    // 同时记录每个 section 在 indices 数组中的起始偏移（按写入顺序确定）
    std::map<int, MeshSection> sectionMap;
    std::map<int, uint32_t>   sectionStart;  // matIdx -> indices 写入起始位置

    int vertexId = 0;  // global vertex counter for ByPolygonVertex access
    for (int p = 0; p < polyCount; ++p)
    {
        int matIdx = 0;
        FbxGeometryElementMaterial* matElem = fbxMesh->GetElementMaterial(0);
        if (matElem)
        {
            if (matElem->GetMappingMode() == FbxGeometryElement::eByPolygon)
                matIdx = matElem->GetIndexArray().GetAt(p);
        }

        for (int v = 0; v < 3; ++v, ++vertexId)
        {
            int ctrlIdx = fbxMesh->GetPolygonVertex(p, v);

            // Normal
            FbxVector4 normal(0, 1, 0, 0);
            if (normalElem)
            {
                int ni = (normalElem->GetMappingMode() == FbxGeometryElement::eByControlPoint)
                         ? ctrlIdx : vertexId;
                if (normalElem->GetReferenceMode() == FbxGeometryElement::eIndexToDirect)
                    ni = normalElem->GetIndexArray().GetAt(ni);
                normal = normalElem->GetDirectArray().GetAt(ni);
            }

            // UV
            FbxVector2 uv(0, 0);
            if (uvElem)
            {
                int ui = (uvElem->GetMappingMode() == FbxGeometryElement::eByControlPoint)
                         ? ctrlIdx : vertexId;
                if (uvElem->GetReferenceMode() != FbxGeometryElement::eDirect)
                    ui = uvElem->GetIndexArray().GetAt(ui);
                uv = uvElem->GetDirectArray().GetAt(ui);
            }

            // FBX UV V 轴与 GLTF/OpenGL 相反，翻转 V
            double flippedV = 1.0 - uv[1];

            uint32_t outIdx;
            uint64_t key = packKey(ctrlIdx, p, v);
            auto mapIt = vertexMap.find(key);
            if (mapIt != vertexMap.end())
            {
                outIdx = mapIt->second;
            }
            else
            {
                outIdx = (uint32_t)mcMesh.positions.size();
                FbxVector4& cp = ctrlPts[ctrlIdx];
                FbxVector4 p = geoTrs.MultT(cp);

                FbxVector4 n = geoTrs.MultR(normal);
                const double nLen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nLen > 1e-12)
                {
                    n[0] /= nLen;
                    n[1] /= nLen;
                    n[2] /= nLen;
                }

                mcMesh.positions.push_back({(float)p[0], (float)p[1], (float)p[2]});
                mcMesh.normals.push_back({(float)n[0], (float)n[1], (float)n[2]});
                if (uvElem)
                    mcMesh.uvs[0].push_back({(float)uv[0], (float)flippedV});
                vertexMap[key] = outIdx;
            }

            // 记录该 section 在 indices 中的起始偏移（首次写入时）
            if (sectionMap.find(matIdx) == sectionMap.end())
                sectionStart[matIdx] = (uint32_t)mcMesh.indices.size();

            mcMesh.indices.push_back(outIdx);
            sectionMap[matIdx].indexCount++;
        }
    }

    // 构建 sections：indexOffset 来自实际写入位置，而非事后累加
    for (auto& [matIdx, sec] : sectionMap)
    {
        sec.indexOffset = sectionStart[matIdx];
        if (matIdx < fbxNode->GetMaterialCount())
            sec.materialId = GetOrCreateMaterial(fbxNode->GetMaterial(matIdx), mcScene);
        mcMesh.sections.push_back(sec);
    }

    // 若无 section 信息，添加一个默认 section
    if (mcMesh.sections.empty() && !mcMesh.indices.empty())
    {
        MeshSection sec;
        sec.indexOffset = 0;
        sec.indexCount  = (uint32_t)mcMesh.indices.size();
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
            mcNode->meshIds.push_back(meshId);
    }

    // 递归子节点
    for (int i = 0; i < fbxNode->GetChildCount(); ++i)
        ConvertNode(fbxNode->GetChild(i), mcScene, nodeId);
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

    // 全场景三角化
    FbxGeometryConverter geomConv(manager);
    geomConv.Triangulate(fbxScene, true);

    FbxNode* root = fbxScene->GetRootNode();
    if (!root)
    {
        result.ok    = false;
        result.error = "FBX scene has no root node";
        return result;
    }

    for (int i = 0; i < root->GetChildCount(); ++i)
        ConvertNode(root->GetChild(i), mcScene, INVALID_ID);

    Logger::Instance().LogInfo(
        std::string("FbxSceneConverter: converted ") +
        std::to_string(mcScene.MeshCount()) + " mesh(es), " +
        std::to_string(mcScene.MaterialCount()) + " material(s), " +
        std::to_string(mcScene.TextureCount()) + " texture(s)."
    );

    return result;
}

} // namespace mc
