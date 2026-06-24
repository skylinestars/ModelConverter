#include "FbxExporter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"

#include <fbxsdk.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

using namespace fbxsdk;

namespace mc {

// ============================================================
// FbxBuilder —— 将 mc::Scene 写入 FbxScene
// 每个方法 < 60 行；Export() 做高层调度。
// ============================================================
class FbxBuilder
{
public:
    FbxBuilder(FbxManager* mgr, FbxScene* scene, const ExportOptions& opts,
               const std::filesystem::path& outputDir)
        : m_mgr(mgr), m_fbxScene(scene), m_opts(opts), m_outputDir(outputDir) {}

    void Build(const Scene& mcScene)
    {
        IndexScene(mcScene);
        CreateMaterials(mcScene);
        CreateNodes(mcScene);
        WireRootNodes(mcScene);
        AddAnimations(mcScene);
    }

private:
    FbxManager*           m_mgr;
    FbxScene*             m_fbxScene;
    const ExportOptions&  m_opts;
    std::filesystem::path m_outputDir;

    std::unordered_map<ObjectID, FbxSurfaceMaterial*> m_matMap;
    std::unordered_map<ObjectID, const Mesh*>         m_meshSrcMap;
    std::unordered_map<ObjectID, const Texture*>      m_texSrcMap;
    std::unordered_map<ObjectID, std::filesystem::path> m_texPathCache;
    std::unordered_map<ObjectID, FbxMesh*>            m_meshMap;
    std::unordered_map<ObjectID, FbxNode*>            m_nodeMap;

    void IndexScene(const Scene& scene)
    {
        for (const auto& m : scene.meshes)    m_meshSrcMap[m.id] = &m;
        for (const auto& t : scene.textures)  m_texSrcMap[t.id] = &t;
    }

    static std::string ExtFromMime(const std::string& mime)
    {
        if (mime == "image/jpeg") return ".jpg";
        if (mime == "image/webp") return ".webp";
        if (mime == "image/bmp")  return ".bmp";
        return ".png";
    }

    static std::string SafeTextureStem(const Texture& tex)
    {
        std::string stem = tex.name.empty() ? ("tex_" + std::to_string(tex.id)) : tex.name;
        std::replace(stem.begin(), stem.end(), ' ', '_');
        return stem;
    }

    static std::filesystem::path Utf8PathFromString(const std::string& s)
    {
        return std::filesystem::u8path(s);
    }

    std::filesystem::path ResolveTexturePath(const Texture& tex)
    {
        auto cached = m_texPathCache.find(tex.id);
        if (cached != m_texPathCache.end()) return cached->second;

        std::filesystem::create_directories(m_outputDir);
        std::string stem = SafeTextureStem(tex);

        if (!tex.uri.empty())
        {
            std::filesystem::path srcPath = tex.uri;
            std::filesystem::path outPath = m_outputDir / srcPath.filename();
            if (outPath.filename().empty())
                outPath = m_outputDir / Utf8PathFromString(stem + srcPath.extension().u8string());

            std::error_code ec;
            if (std::filesystem::exists(srcPath, ec))
            {
                std::filesystem::copy_file(srcPath, outPath,
                                           std::filesystem::copy_options::overwrite_existing,
                                           ec);
                if (!ec)
                {
                    m_texPathCache[tex.id] = outPath;
                    return outPath;
                }
            }
        }

        if (!tex.embeddedData.empty())
        {
            std::filesystem::path outPath = m_outputDir / Utf8PathFromString(stem + ExtFromMime(tex.mimeType));
            std::ofstream ofs(outPath, std::ios::binary);
            if (!ofs) return {};
            ofs.write(reinterpret_cast<const char*>(tex.embeddedData.data()),
                      static_cast<std::streamsize>(tex.embeddedData.size()));
            if (!ofs) return {};
            m_texPathCache[tex.id] = outPath;
            return outPath;
        }

        return {};
    }

    void BindBaseColorTexture(FbxSurfacePhong* fbxMat, const Material& mat)
    {
        auto texIt = m_texSrcMap.find(mat.baseColorTexture.textureId);
        if (texIt == m_texSrcMap.end()) return;

        std::filesystem::path texPath = ResolveTexturePath(*texIt->second);
        if (texPath.empty()) return;

        FbxFileTexture* fbxTex = FbxFileTexture::Create(m_fbxScene, (mat.name + "_BaseColor").c_str());
        std::string absPath = std::filesystem::absolute(texPath).u8string();
        std::string relPath = texPath.filename().u8string();
        fbxTex->SetFileName(absPath.c_str());
        fbxTex->SetRelativeFileName(relPath.c_str());
        fbxTex->SetTextureUse(FbxTexture::eStandard);
        fbxTex->SetMappingType(FbxTexture::eUV);
        fbxTex->SetMaterialUse(FbxFileTexture::eModelMaterial);
        fbxTex->SetSwapUV(false);
        fbxTex->SetTranslation(0.0, 0.0);
        fbxTex->SetScale(1.0, 1.0);
        fbxTex->SetRotation(0.0, 0.0);

        fbxMat->Diffuse.ConnectSrcObject(fbxTex);
    }

    // ---- Materials ----
    FbxSurfaceMaterial* MakeMaterial(const Material& mat)
    {
        auto* fbxMat = FbxSurfacePhong::Create(m_fbxScene, mat.name.c_str());
        fbxMat->Diffuse         .Set(FbxDouble3(mat.baseColor.x, mat.baseColor.y, mat.baseColor.z));
        fbxMat->DiffuseFactor   .Set(mat.baseColor.w);
        fbxMat->Emissive        .Set(FbxDouble3(mat.emissive.x,  mat.emissive.y,  mat.emissive.z));
        fbxMat->Shininess       .Set(mat.shininess);
        fbxMat->ShadingModel    .Set("Phong");
        fbxMat->TransparencyFactor.Set(1.0 - mat.opacity);
        BindBaseColorTexture(fbxMat, mat);
        return fbxMat;
    }

    void CreateMaterials(const Scene& scene)
    {
        for (const auto& mat : scene.materials)
            m_matMap[mat.id] = MakeMaterial(mat);
    }

    // ---- Mesh control-points & polygon layout ----
    void SetControlPoints(FbxMesh* fbxMesh, const Mesh& mcMesh)
    {
        fbxMesh->InitControlPoints((int)mcMesh.positions.size());
        FbxVector4* cp = fbxMesh->GetControlPoints();
        for (size_t i = 0; i < mcMesh.positions.size(); ++i)
            cp[i] = FbxVector4(mcMesh.positions[i].x, mcMesh.positions[i].y,
                               mcMesh.positions[i].z, 1.0);
    }

    void AddNormals(FbxMesh* fbxMesh, const Mesh& mcMesh)
    {
        if (!m_opts.exportNormals || mcMesh.normals.empty()) return;
        auto* layer = fbxMesh->GetLayer(0);
        auto* elem  = FbxLayerElementNormal::Create(fbxMesh, "");
        elem->SetMappingMode(FbxLayerElement::eByControlPoint);
        elem->SetReferenceMode(FbxLayerElement::eDirect);
        for (const auto& n : mcMesh.normals)
            elem->GetDirectArray().Add(FbxVector4(n.x, n.y, n.z, 0.0));
        layer->SetNormals(elem);
    }

    void AddUVs(FbxMesh* fbxMesh, const Mesh& mcMesh)
    {
        if (!m_opts.exportUVs || mcMesh.uvs.empty() || mcMesh.uvs[0].empty()) return;
        auto* elem = fbxMesh->CreateElementUV("UVMap");
        elem->SetMappingMode(FbxLayerElement::eByControlPoint);
        elem->SetReferenceMode(FbxLayerElement::eDirect);
        for (const auto& uv : mcMesh.uvs[0])
            elem->GetDirectArray().Add(FbxVector2(uv.x, 1.0 - uv.y));
    }

    int FindNodeMaterialIndex(FbxNode* node, FbxSurfaceMaterial* mat)
    {
        for (int i = 0; i < node->GetMaterialCount(); ++i)
            if (node->GetMaterial(i) == mat) return i;
        return -1;
    }

    int EnsureNodeMaterial(FbxNode* node, ObjectID matId)
    {
        auto it = m_matMap.find(matId);
        if (it == m_matMap.end()) return -1;

        FbxSurfaceMaterial* fbxMat = it->second;
        int idx = FindNodeMaterialIndex(node, fbxMat);
        if (idx >= 0) return idx;
        return node->AddMaterial(fbxMat);
    }

    void EnsureNodeMaterials(FbxNode* node, const Mesh& mcMesh)
    {
        for (const auto& sec : mcMesh.sections)
            EnsureNodeMaterial(node, sec.materialId);
    }

    void AddSectionPolygons(FbxMesh* fbxMesh, const Mesh& mcMesh,
                             uint32_t idxOffset, uint32_t idxCount, int matIdx)
    {
        for (uint32_t i = idxOffset; i + 2 < idxOffset + idxCount; i += 3)
        {
            fbxMesh->BeginPolygon(matIdx);
            fbxMesh->AddPolygon((int)mcMesh.indices[i]);
            fbxMesh->AddPolygon((int)mcMesh.indices[i + 1]);
            fbxMesh->AddPolygon((int)mcMesh.indices[i + 2]);
            fbxMesh->EndPolygon();
        }
    }

    FbxMesh* BuildMesh(FbxNode* meshNode, const Mesh& mcMesh)
    {
        auto* fbxMesh = FbxMesh::Create(m_fbxScene, mcMesh.name.c_str());
        fbxMesh->CreateLayer();
        SetControlPoints(fbxMesh, mcMesh);
        AddNormals(fbxMesh, mcMesh);
        AddUVs(fbxMesh, mcMesh);

        if (!mcMesh.sections.empty())
        {
            EnsureNodeMaterials(meshNode, mcMesh);
            bool hasNodeMaterial = meshNode->GetMaterialCount() > 0;
            FbxLayerElementMaterial* matElem = nullptr;
            if (hasNodeMaterial)
            {
                matElem = fbxMesh->CreateElementMaterial();
                matElem->SetMappingMode(FbxLayerElement::eByPolygon);
                matElem->SetReferenceMode(FbxLayerElement::eIndexToDirect);
            }

            for (const auto& sec : mcMesh.sections)
            {
                int matIdx = EnsureNodeMaterial(meshNode, sec.materialId);
                AddSectionPolygons(fbxMesh, mcMesh, sec.indexOffset, sec.indexCount, matIdx);

                if (matElem)
                {
                    int fillIdx = (matIdx >= 0) ? matIdx : 0;
                    uint32_t polyCount = sec.indexCount / 3;
                    for (uint32_t p = 0; p < polyCount; ++p)
                        matElem->GetIndexArray().Add(fillIdx);
                }
            }
        }
        else if (!mcMesh.indices.empty())
        {
            AddSectionPolygons(fbxMesh, mcMesh, 0, (uint32_t)mcMesh.indices.size(), -1);
        }

        return fbxMesh;
    }

    // ---- Nodes ----
    FbxNode* MakeSceneNode(const Node& mcNode)
    {
        auto* fbxNode = FbxNode::Create(m_fbxScene, mcNode.name.c_str());

        // localMatrix -> FbxAMatrix
        const float* m = mcNode.localMatrix.m;
        FbxAMatrix mat;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                mat.mData[col][row] = m[col * 4 + row];

        fbxNode->LclTranslation.Set(mat.GetT());
        fbxNode->LclRotation.Set(mat.GetR());
        fbxNode->LclScaling.Set(mat.GetS());

        if (!mcNode.meshIds.empty())
        {
            auto srcIt = m_meshSrcMap.find(mcNode.meshIds[0]);
            if (srcIt != m_meshSrcMap.end())
            {
                FbxMesh* fbxMesh = nullptr;
                auto built = m_meshMap.find(mcNode.meshIds[0]);
                if (built != m_meshMap.end())
                {
                    fbxMesh = built->second;
                    EnsureNodeMaterials(fbxNode, *srcIt->second);
                }
                else
                {
                    fbxMesh = BuildMesh(fbxNode, *srcIt->second);
                    m_meshMap[mcNode.meshIds[0]] = fbxMesh;
                }
                if (fbxMesh) fbxNode->SetNodeAttribute(fbxMesh);
            }
        }

        return fbxNode;
    }

    void CreateNodes(const Scene& scene)
    {
        for (const auto& mcNode : scene.nodes)
        {
            FbxNode* fbxNode = MakeSceneNode(mcNode);
            m_nodeMap[mcNode.id] = fbxNode;
        }
        for (const auto& mcNode : scene.nodes)
        {
            FbxNode* fbxNode = m_nodeMap[mcNode.id];
            for (ObjectID childId : mcNode.children)
            {
                auto it = m_nodeMap.find(childId);
                if (it != m_nodeMap.end()) fbxNode->AddChild(it->second);
            }
        }
    }

    void WireRootNodes(const Scene& scene)
    {
        FbxNode* root = m_fbxScene->GetRootNode();
        for (ObjectID rootId : scene.rootNodes)
        {
            auto it = m_nodeMap.find(rootId);
            if (it != m_nodeMap.end()) root->AddChild(it->second);
        }
    }

    // ---- Animations（Phase14）----
    // Quaternion → Euler 角度（度），使用 ZYX 旋转顺序
    static FbxDouble3 QuatToEulerDegrees(const Quaternion& q)
    {
        // 标准化四元数
        float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        float x = q.x / std::sqrt(len2);
        float y = q.y / std::sqrt(len2);
        float z = q.z / std::sqrt(len2);
        float w = q.w / std::sqrt(len2);

        // ZYX 内旋 → Euler (roll, pitch, yaw) = (zRot, xRot, yRot)
        float sinRCosP = 2.0f * (w * x + y * z);
        float cosRCosP = 1.0f - 2.0f * (x * x + y * y);
        float rollZ = std::atan2(sinRCosP, cosRCosP);

        float sinP    = 2.0f * (w * y - z * x);
        float pitchX;
        if (std::abs(sinP) >= 1.0f)
            pitchX = std::copysign(3.14159265f / 2.0f, sinP);
        else
            pitchX = std::asin(sinP);

        float sinYCosP = 2.0f * (w * z + x * y);
        float cosYCosP = 1.0f - 2.0f * (y * y + z * z);
        float yawY = std::atan2(sinYCosP, cosYCosP);

        constexpr float kRadToDeg = 180.0f / 3.14159265358979f;
        return FbxDouble3(pitchX * kRadToDeg, yawY * kRadToDeg, rollZ * kRadToDeg);
    }

    void AddAnimCurve(FbxAnimCurve* curve, const std::vector<KeyFrame<float>>& keys,
                      FbxAnimCurveDef::EInterpolationType interp)
    {
        if (!curve) return;
        curve->KeyModifyBegin();
        for (const auto& kf : keys)
        {
            FbxTime fbxtime;
            fbxtime.SetSecondDouble(kf.time);
            int keyIdx = curve->KeyAdd(fbxtime);
            curve->KeySetValue(keyIdx, kf.value);
            curve->KeySetInterpolation(keyIdx, interp);
        }
        curve->KeyModifyEnd();
    }

    void AddAnimCurve(FbxAnimCurve* curve, const std::vector<KeyFrame<Vec3>>& keys,
                      int component, FbxAnimCurveDef::EInterpolationType interp)
    {
        if (!curve) return;
        curve->KeyModifyBegin();
        for (const auto& kf : keys)
        {
            FbxTime fbxtime;
            fbxtime.SetSecondDouble(kf.time);
            int keyIdx = curve->KeyAdd(fbxtime);
            float val = (&kf.value.x)[component];
            curve->KeySetValue(keyIdx, val);
            curve->KeySetInterpolation(keyIdx, interp);
        }
        curve->KeyModifyEnd();
    }

    static FbxAnimCurveDef::EInterpolationType ToFbxInterp(AnimationInterpolation interp)
    {
        switch (interp)
        {
            case AnimationInterpolation::Step:        return FbxAnimCurveDef::eInterpolationConstant;
            case AnimationInterpolation::CubicSpline: return FbxAnimCurveDef::eInterpolationCubic;
            default:                                   return FbxAnimCurveDef::eInterpolationLinear;
        }
    }

    void AddNodeAnimation(FbxNode* fbxNode, const NodeAnimation& nodeAnim,
                           FbxAnimLayer* layer)
    {
        FbxAnimCurveDef::EInterpolationType tInterp = ToFbxInterp(nodeAnim.translation.interpolation);
        FbxAnimCurveDef::EInterpolationType rInterp = ToFbxInterp(nodeAnim.rotation.interpolation);
        FbxAnimCurveDef::EInterpolationType sInterp = ToFbxInterp(nodeAnim.scale.interpolation);

        // Translation
        if (!nodeAnim.translation.keys.empty())
        {
            FbxAnimCurveNode* tNode = fbxNode->LclTranslation.GetCurveNode(layer, true);
            if (tNode)
            {
                AddAnimCurve(tNode->CreateCurve(tNode->GetName(), FBXSDK_CURVENODE_COMPONENT_X),
                             nodeAnim.translation.keys, 0, tInterp);
                AddAnimCurve(tNode->CreateCurve(tNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Y),
                             nodeAnim.translation.keys, 1, tInterp);
                AddAnimCurve(tNode->CreateCurve(tNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Z),
                             nodeAnim.translation.keys, 2, tInterp);
            }
        }

        // Rotation: Quaternion → Euler（度），与 MakeSceneNode 的 mat.GetR() 保持一致
        if (!nodeAnim.rotation.keys.empty())
        {
            FbxAnimCurveNode* rNode = fbxNode->LclRotation.GetCurveNode(layer, true);
            if (rNode)
            {
                FbxAnimCurve* rx = rNode->CreateCurve(rNode->GetName(), FBXSDK_CURVENODE_COMPONENT_X);
                FbxAnimCurve* ry = rNode->CreateCurve(rNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Y);
                FbxAnimCurve* rz = rNode->CreateCurve(rNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Z);

                if (rx) rx->KeyModifyBegin();
                if (ry) ry->KeyModifyBegin();
                if (rz) rz->KeyModifyBegin();

                for (const auto& kf : nodeAnim.rotation.keys)
                {
                    // 用 FBX SDK 内置转换，与 MakeSceneNode 的 mat.GetR() 一致
                    FbxQuaternion fbxQ(kf.value.x, kf.value.y, kf.value.z, kf.value.w);
                    FbxAMatrix rotMtx;
                    rotMtx.SetQ(fbxQ);
                    FbxVector4 euler = rotMtx.GetR();

                    FbxTime fbxtime;
                    fbxtime.SetSecondDouble(kf.time);

                    if (rx) { int ki = rx->KeyAdd(fbxtime); rx->KeySetValue(ki, static_cast<float>(euler[0])); rx->KeySetInterpolation(ki, rInterp); }
                    if (ry) { int ki = ry->KeyAdd(fbxtime); ry->KeySetValue(ki, static_cast<float>(euler[1])); ry->KeySetInterpolation(ki, rInterp); }
                    if (rz) { int ki = rz->KeyAdd(fbxtime); rz->KeySetValue(ki, static_cast<float>(euler[2])); rz->KeySetInterpolation(ki, rInterp); }
                }

                if (rx) rx->KeyModifyEnd();
                if (ry) ry->KeyModifyEnd();
                if (rz) rz->KeyModifyEnd();
            }
        }

        // Scale
        if (!nodeAnim.scale.keys.empty())
        {
            FbxAnimCurveNode* sNode = fbxNode->LclScaling.GetCurveNode(layer, true);
            if (sNode)
            {
                AddAnimCurve(sNode->CreateCurve(sNode->GetName(), FBXSDK_CURVENODE_COMPONENT_X),
                             nodeAnim.scale.keys, 0, sInterp);
                AddAnimCurve(sNode->CreateCurve(sNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Y),
                             nodeAnim.scale.keys, 1, sInterp);
                AddAnimCurve(sNode->CreateCurve(sNode->GetName(), FBXSDK_CURVENODE_COMPONENT_Z),
                             nodeAnim.scale.keys, 2, sInterp);
            }
        }
    }

    void AddAnimations(const Scene& scene)
    {
        if (scene.animations.empty()) return;

        for (const auto& clip : scene.animations)
        {
            if (clip.nodeChannels.empty()) continue;

            // 创建 FbxAnimStack
            FbxAnimStack* animStack = FbxAnimStack::Create(m_fbxScene, clip.name.c_str());
            FbxTime startFbxTime, stopFbxTime;
            startFbxTime.SetSecondDouble(clip.startTime);
            stopFbxTime.SetSecondDouble(clip.endTime);
            FbxTimeSpan timeSpan;
            timeSpan.Set(startFbxTime, stopFbxTime);
            animStack->SetLocalTimeSpan(timeSpan);

            // 创建 FbxAnimLayer
            FbxAnimLayer* layer = FbxAnimLayer::Create(m_fbxScene, "BaseLayer");
            animStack->AddMember(layer);

            // 为每个 NodeAnimation 添加曲线
            for (const auto& nodeAnim : clip.nodeChannels)
            {
                auto nodeIt = m_nodeMap.find(nodeAnim.nodeId);
                if (nodeIt == m_nodeMap.end()) continue;

                AddNodeAnimation(nodeIt->second, nodeAnim, layer);
            }
        }

        Logger::Instance().LogInfo(
            "FbxExporter: exported " + std::to_string(scene.animations.size()) +
            " animation clip(s).");
    }
};

// ============================================================
// CanExport
// ============================================================
bool FbxExporter::CanExport(const std::string& ext) const
{
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == ".fbx";
}

// ============================================================
// Export  （高层调度，< 60 行）
// ============================================================
VoidResult FbxExporter::Export(const Scene& scene, ExportContext& ctx)
{
    if (ctx.outputPath.empty())
        return {false, "FbxExporter: outputPath is empty"};

    FbxManager* mgr  = FbxManager::Create();
    FbxIOSettings* ios = FbxIOSettings::Create(mgr, IOSROOT);
    mgr->SetIOSettings(ios);

    FbxScene* fbxScene = FbxScene::Create(mgr, "ExportedScene");
    fbxScene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem::MayaYUp);
    fbxScene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);

    FbxBuilder builder(mgr, fbxScene, ctx.options,
                       std::filesystem::path(ctx.outputPath).parent_path());
    builder.Build(scene);

    std::string ext = std::filesystem::path(ctx.outputPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    int fileFormat = mgr->GetIOPluginRegistry()->GetNativeWriterFormat();
    if (ext == ".fbx")
    {
        int fmt = mgr->GetIOPluginRegistry()->FindWriterIDByDescription("FBX binary (*.fbx)");
        if (fmt >= 0) fileFormat = fmt;
    }

    fbxsdk::FbxExporter* fbxExp = fbxsdk::FbxExporter::Create(mgr, "");
    bool initOk = fbxExp->Initialize(ctx.outputPath.c_str(), fileFormat, mgr->GetIOSettings());
    if (!initOk)
    {
        std::string err = fbxExp->GetStatus().GetErrorString();
        fbxExp->Destroy(); fbxScene->Destroy(); mgr->Destroy();
        return {false, std::string("FbxExporter: Initialize failed: ") + err};
    }

    bool exportOk = fbxExp->Export(fbxScene);
    fbxExp->Destroy();
    fbxScene->Destroy();
    mgr->Destroy();

    if (!exportOk)
        return {false, std::string("FbxExporter: export failed for '") + ctx.outputPath + "'"};

    ctx.meshesExported    = scene.MeshCount();
    ctx.materialsExported = scene.MaterialCount();
    ctx.texturesExported  = scene.TextureCount();
    ctx.nodesExported     = scene.NodeCount();

    Logger::Instance().LogInfo(
        std::string("FbxExporter: exported ") +
        std::to_string(ctx.meshesExported) + " mesh(es), " +
        std::to_string(ctx.materialsExported) + " material(s) -> " + ctx.outputPath);

    return {true, ""};
}

} // namespace mc
