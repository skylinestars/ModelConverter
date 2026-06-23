#include "FbxExporter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
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
