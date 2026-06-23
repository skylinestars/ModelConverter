#include "GltfExporter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
#include "mc/common/Logger.h"

// tinygltf 实现已在 GltfSceneConverter.cpp 中 define，此处只使用声明
#include "tiny_gltf.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace mc {

// ============================================================
// GltfBuilder —— 将 mc::Scene 填充到 tinygltf::Model
// ============================================================
class GltfBuilder
{
public:
    GltfBuilder(tinygltf::Model& model, const ExportOptions& opts, bool embedImages)
        : m_model(model), m_opts(opts), m_embedImages(embedImages)
    {
        m_model.asset.version   = "2.0";
        m_model.asset.generator = "mc GltfExporter Phase11";
        m_model.buffers.emplace_back();  // buffer[0]
    }

    void Build(const Scene& scene)
    {
        AddTextures(scene);
        AddMaterials(scene);
        AddMeshes(scene);
        AddNodes(scene);
        AddScene(scene);
    }

    const std::unordered_map<ObjectID, int>& MeshIdxMap() const { return m_meshIdxMap; }

private:
    tinygltf::Model&   m_model;
    const ExportOptions& m_opts;
    bool m_embedImages;

    std::unordered_map<ObjectID, int> m_texIdxMap;
    std::unordered_map<ObjectID, int> m_matIdxMap;
    std::unordered_map<ObjectID, int> m_meshIdxMap;
    std::unordered_map<ObjectID, int> m_nodeIdxMap;

    // ---- 辅助：向 buffer[0] 追加数据并 4-byte 对齐，返回 accessor 索引 ----
    int PushAccessor(const void* data, size_t bytes,
                     int componentType, int type, int count,
                     int bufTarget,
                     std::vector<double> minV = {},
                     std::vector<double> maxV = {})
    {
        auto& buf = m_model.buffers[0].data;
        size_t byteOffset = buf.size();
        buf.resize(byteOffset + bytes);
        std::memcpy(buf.data() + byteOffset, data, bytes);
        while (buf.size() % 4) buf.push_back(0);

        tinygltf::BufferView bv;
        bv.buffer = 0; bv.byteOffset = (int)byteOffset;
        bv.byteLength = (int)bytes; bv.target = bufTarget;
        int bvIdx = (int)m_model.bufferViews.size();
        m_model.bufferViews.push_back(std::move(bv));

        tinygltf::Accessor acc;
        acc.bufferView = bvIdx; acc.byteOffset = 0;
        acc.componentType = componentType; acc.count = count; acc.type = type;
        acc.minValues = std::move(minV); acc.maxValues = std::move(maxV);
        int accIdx = (int)m_model.accessors.size();
        m_model.accessors.push_back(std::move(acc));
        return accIdx;
    }

    // ---- Textures ----
    static std::string MimeTypeFromPath(const std::filesystem::path& p)
    {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".webp")                  return "image/webp";
        return "image/png";
    }

    // 把图片字节写入 buffer[0]，返回 bufferView 索引（-1 表示无数据）
    int PushImageBufferView(const std::vector<uint8_t>& bytes)
    {
        if (bytes.empty()) return -1;
        auto& buf = m_model.buffers[0].data;
        size_t byteOffset = buf.size();
        buf.insert(buf.end(), bytes.begin(), bytes.end());
        while (buf.size() % 4) buf.push_back(0);

        tinygltf::BufferView bv;
        bv.buffer     = 0;
        bv.byteOffset = (int)byteOffset;
        bv.byteLength = (int)bytes.size();
        bv.target     = 0;  // images 不设 target
        int bvIdx = (int)m_model.bufferViews.size();
        m_model.bufferViews.push_back(std::move(bv));
        return bvIdx;
    }

    void AddTextures(const Scene& scene)
    {
        int texIdx = 0;
        for (const auto& mcTex : scene.textures)
        {
            tinygltf::Image img;
            img.name = mcTex.name;
            if (img.name.empty()) {
                img.name = "texture_" + std::to_string(texIdx);
            } else {
                std::string nameExt = std::filesystem::path(img.name).extension().string();
                if (!nameExt.empty()) {
                    img.name = std::filesystem::path(img.name).stem().string();
                }
            }

            std::vector<uint8_t> rawBytes;
            std::string mime;

            if (mcTex.embedded && !mcTex.embeddedData.empty())
            {
                rawBytes = mcTex.embeddedData;
                mime     = mcTex.mimeType.empty() ? "image/png" : mcTex.mimeType;
            }
            else if (!mcTex.uri.empty())
            {
                std::ifstream ifs(mcTex.uri, std::ios::binary);
                if (ifs)
                {
                    rawBytes = std::vector<uint8_t>(
                        std::istreambuf_iterator<char>(ifs), {});
                    mime = MimeTypeFromPath(mcTex.uri);
                }
            }

            if (!rawBytes.empty())
            {
                img.mimeType = mime;
                if (m_embedImages)
                {
                    int bvIdx      = PushImageBufferView(rawBytes);
                    img.bufferView = bvIdx;
                }
                else
                {
                    img.image  = rawBytes;
                    img.as_is  = true;
                    img.bufferView = -1;
                }
            }
            else if (!mcTex.uri.empty())
            {
                img.uri = mcTex.uri.string();
            }

            tinygltf::Texture gTex;
            gTex.name   = mcTex.name;
            gTex.source = (int)m_model.images.size();
            m_model.images.push_back(std::move(img));
            m_texIdxMap[mcTex.id] = (int)m_model.textures.size();
            m_model.textures.push_back(std::move(gTex));
            ++texIdx;
        }
    }

    // ---- Materials ----
    void SetTexInfo(const TextureRef& ref, tinygltf::TextureInfo& info) const
    {
        auto it = m_texIdxMap.find(ref.textureId);
        if (it != m_texIdxMap.end()) { info.index = it->second; info.texCoord = ref.uvSet; }
    }

    void AddMaterials(const Scene& scene)
    {
        for (const auto& mcMat : scene.materials)
        {
            tinygltf::Material gMat;
            gMat.name = mcMat.name;
            gMat.pbrMetallicRoughness.baseColorFactor =
                {mcMat.baseColor.x, mcMat.baseColor.y, mcMat.baseColor.z, mcMat.baseColor.w};
            gMat.pbrMetallicRoughness.metallicFactor  = mcMat.metallic;
            gMat.pbrMetallicRoughness.roughnessFactor = mcMat.roughness;

            SetTexInfo(mcMat.baseColorTexture,        gMat.pbrMetallicRoughness.baseColorTexture);
            SetTexInfo(mcMat.metallicRoughnessTexture, gMat.pbrMetallicRoughness.metallicRoughnessTexture);

            if (auto it = m_texIdxMap.find(mcMat.normalTexture.textureId);   it != m_texIdxMap.end())
                { gMat.normalTexture.index = it->second;   gMat.normalTexture.texCoord   = mcMat.normalTexture.uvSet; }
            if (auto it = m_texIdxMap.find(mcMat.emissiveTexture.textureId); it != m_texIdxMap.end())
                { gMat.emissiveTexture.index = it->second; gMat.emissiveTexture.texCoord = mcMat.emissiveTexture.uvSet; }
            if (auto it = m_texIdxMap.find(mcMat.occlusionTexture.textureId);it != m_texIdxMap.end())
                { gMat.occlusionTexture.index = it->second;gMat.occlusionTexture.texCoord= mcMat.occlusionTexture.uvSet; }

            gMat.emissiveFactor = {mcMat.emissive.x, mcMat.emissive.y, mcMat.emissive.z};
            gMat.doubleSided    = mcMat.doubleSided;
            gMat.alphaCutoff    = mcMat.alphaCutoff;
            switch (mcMat.alphaMode)
            {
                case AlphaMode::Mask:  gMat.alphaMode = "MASK";  break;
                case AlphaMode::Blend: gMat.alphaMode = "BLEND"; break;
                default:               gMat.alphaMode = "OPAQUE";
            }

            m_matIdxMap[mcMat.id] = (int)m_model.materials.size();
            m_model.materials.push_back(std::move(gMat));
        }
    }

    // ---- Mesh attribute accessors (per-mesh, written once) ----
    struct MeshAttribs { int pos = -1, nrm = -1, uv = -1; };

    MeshAttribs BuildMeshAttribs(const Mesh& mcMesh)
    {
        MeshAttribs r;

        Vec3 vmin{1e30f,1e30f,1e30f}, vmax{-1e30f,-1e30f,-1e30f};
        for (const auto& p : mcMesh.positions)
        {
            vmin.x=std::min(vmin.x,p.x); vmin.y=std::min(vmin.y,p.y); vmin.z=std::min(vmin.z,p.z);
            vmax.x=std::max(vmax.x,p.x); vmax.y=std::max(vmax.y,p.y); vmax.z=std::max(vmax.z,p.z);
        }
        r.pos = PushAccessor(
            mcMesh.positions.data(), mcMesh.positions.size() * sizeof(Vec3),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, (int)mcMesh.positions.size(),
            TINYGLTF_TARGET_ARRAY_BUFFER,
            {vmin.x,vmin.y,vmin.z}, {vmax.x,vmax.y,vmax.z});

        if (m_opts.exportNormals && mcMesh.normals.size() == mcMesh.positions.size())
            r.nrm = PushAccessor(
                mcMesh.normals.data(), mcMesh.normals.size() * sizeof(Vec3),
                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, (int)mcMesh.normals.size(),
                TINYGLTF_TARGET_ARRAY_BUFFER);

        if (m_opts.exportUVs && !mcMesh.uvs.empty() &&
            mcMesh.uvs[0].size() == mcMesh.positions.size())
            r.uv = PushAccessor(
                mcMesh.uvs[0].data(), mcMesh.uvs[0].size() * sizeof(Vec2),
                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, (int)mcMesh.uvs[0].size(),
                TINYGLTF_TARGET_ARRAY_BUFFER);

        return r;
    }

    void AddPrimitive(tinygltf::Mesh& gMesh, const MeshAttribs& a,
                      uint32_t idxOffset, uint32_t idxCount,
                      const std::vector<uint32_t>& indices, ObjectID matId)
    {
        tinygltf::Primitive prim;
        prim.mode = TINYGLTF_MODE_TRIANGLES;
        prim.attributes["POSITION"] = a.pos;
        if (a.nrm >= 0) prim.attributes["NORMAL"]     = a.nrm;
        if (a.uv  >= 0) prim.attributes["TEXCOORD_0"] = a.uv;

        prim.indices = PushAccessor(
            indices.data() + idxOffset, idxCount * sizeof(uint32_t),
            TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, (int)idxCount,
            TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);

        if (matId != INVALID_ID)
            if (auto it = m_matIdxMap.find(matId); it != m_matIdxMap.end())
                prim.material = it->second;

        gMesh.primitives.push_back(std::move(prim));
    }

    // ---- Meshes ----
    void AddMeshes(const Scene& scene)
    {
        for (const auto& mcMesh : scene.meshes)
        {
            if (mcMesh.positions.empty()) continue;

            tinygltf::Mesh gMesh;
            gMesh.name = mcMesh.name;

            // 顶点属性每个 mesh 只写一次，所有 primitive 共享
            MeshAttribs attribs = BuildMeshAttribs(mcMesh);

            if (!mcMesh.sections.empty())
            {
                for (const auto& sec : mcMesh.sections)
                    if (sec.indexCount > 0)
                        AddPrimitive(gMesh, attribs,
                                     sec.indexOffset, sec.indexCount,
                                     mcMesh.indices, sec.materialId);
            }
            else if (!mcMesh.indices.empty())
            {
                AddPrimitive(gMesh, attribs,
                             0, (uint32_t)mcMesh.indices.size(),
                             mcMesh.indices, INVALID_ID);
            }

            m_meshIdxMap[mcMesh.id] = (int)m_model.meshes.size();
            m_model.meshes.push_back(std::move(gMesh));
        }
    }

    // ---- Nodes ----
    void AddNodes(const Scene& scene)
    {
        for (const auto& mcNode : scene.nodes)
        {
            tinygltf::Node gNode;
            gNode.name = mcNode.name;
            if (!mcNode.meshIds.empty())
                if (auto it = m_meshIdxMap.find(mcNode.meshIds[0]); it != m_meshIdxMap.end())
                    gNode.mesh = it->second;

            const float* m = mcNode.localMatrix.m;
            bool isIdentity = true;
            for (int i = 0; i < 16 && isIdentity; ++i)
                isIdentity = (std::abs(m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-6f);
            if (!isIdentity)
            {
                gNode.matrix.resize(16);
                for (int i = 0; i < 16; ++i) gNode.matrix[i] = m[i];
            }

            m_nodeIdxMap[mcNode.id] = (int)m_model.nodes.size();
            m_model.nodes.push_back(std::move(gNode));
        }
        for (const auto& mcNode : scene.nodes)
        {
            int parentIdx = m_nodeIdxMap[mcNode.id];
            for (ObjectID childId : mcNode.children)
                if (auto it = m_nodeIdxMap.find(childId); it != m_nodeIdxMap.end())
                    m_model.nodes[parentIdx].children.push_back(it->second);
        }
    }

    // ---- Scene ----
    void AddScene(const Scene& scene)
    {
        tinygltf::Scene gScene;
        gScene.name = "Scene";
        for (ObjectID rootId : scene.rootNodes)
            if (auto it = m_nodeIdxMap.find(rootId); it != m_nodeIdxMap.end())
                gScene.nodes.push_back(it->second);
        m_model.scenes.push_back(std::move(gScene));
        m_model.defaultScene = 0;
    }
};

// ============================================================
// CanExport
// ============================================================
bool GltfExporter::CanExport(const std::string& ext) const
{
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == ".gltf" || lower == ".glb";
}

// ============================================================
// Export
// ============================================================
VoidResult GltfExporter::Export(const Scene& scene, ExportContext& ctx)
{
    if (ctx.outputPath.empty())
        return {false, "GltfExporter: outputPath is empty"};

    std::string ext = std::filesystem::path(ctx.outputPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    bool asBinary = (ext == ".glb");

    tinygltf::Model gltf;
    bool embedImages = asBinary || ctx.options.embedTextures;
    GltfBuilder builder(gltf, ctx.options, embedImages);
    builder.Build(scene);

    tinygltf::TinyGLTF writer;
    bool ok;
    if (asBinary)
    {
        ok = writer.WriteGltfSceneToFile(&gltf, ctx.outputPath,
                                          /*embedImages=*/true,
                                          /*embedBuffers=*/true,
                                          /*prettyPrint=*/false,
                                          /*isBinary=*/true);
    }
    else
    {
        gltf.buffers[0].uri =
            std::filesystem::path(ctx.outputPath).stem().string() + ".bin";
        ok = writer.WriteGltfSceneToFile(&gltf, ctx.outputPath,
                                          /*embedImages=*/ctx.options.embedTextures,
                                          /*embedBuffers=*/false,
                                          /*prettyPrint=*/ctx.options.prettyPrint,
                                          /*writeBinary=*/false);
    }

    if (!ok)
        return {false, std::string("GltfExporter: write failed for '") + ctx.outputPath + "'"};

    ctx.meshesExported    = scene.MeshCount();
    ctx.materialsExported = scene.MaterialCount();
    ctx.texturesExported  = scene.TextureCount();
    ctx.nodesExported     = scene.NodeCount();

    Logger::Instance().LogInfo(
        std::string("GltfExporter: exported ") +
        std::to_string(ctx.meshesExported) + " mesh(es), " +
        std::to_string(ctx.materialsExported) + " material(s) -> " + ctx.outputPath);

    return {true, ""};
}

} // namespace mc
