#include "GltfExporter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"

// tinygltf 实现已在 GltfSceneConverter.cpp 中 define，此处只使用声明
#include "tiny_gltf.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
        AddSkins(scene);
        AddScene(scene);
        AddAnimations(scene);
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

            std::vector<uint8_t> rawBytes;
            std::string mime;
            std::string imageFileName;

            if (mcTex.embedded && !mcTex.embeddedData.empty())
            {
                rawBytes = mcTex.embeddedData;
                mime     = mcTex.mimeType.empty() ? "image/png" : mcTex.mimeType;
                if (!img.name.empty()) {
                    std::filesystem::path namePath(img.name);
                    if (!namePath.has_extension()) {
                        std::string ext = "png";
                        if (mime == "image/jpeg") ext = "jpg";
                        else if (mime == "image/webp") ext = "webp";
                        img.name = img.name + "." + ext;
                    }
                    imageFileName = std::filesystem::path(img.name).filename().string();
                }
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
                imageFileName = std::filesystem::path(mcTex.uri).filename().string();
            }

            if (imageFileName.empty() && !img.name.empty()) {
                imageFileName = std::filesystem::path(img.name).filename().string();
            }
            if (imageFileName.empty()) {
                std::string ext = "png";
                if (mime == "image/jpeg") ext = "jpg";
                else if (mime == "image/webp") ext = "webp";
                imageFileName = "texture_" + std::to_string(texIdx) + "." + ext;
                img.name = imageFileName;
            }

            if (!rawBytes.empty())
            {
                img.mimeType = mime;
                if (m_embedImages)
                {
                    int bvIdx      = PushImageBufferView(rawBytes);
                    img.bufferView = bvIdx;
                    img.uri.clear();
                    img.name = imageFileName;
                }
                else
                {
                    img.image  = rawBytes;
                    img.as_is  = true;
                    img.bufferView = -1;
                    img.uri = imageFileName;
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

            // FBX 模型法线经常反向，强制双面渲染避免背面透明
            gMat.doubleSided = true;

            switch (mcMat.alphaMode)
            {
                case AlphaMode::Mask:  gMat.alphaMode = "MASK"; gMat.alphaCutoff = mcMat.alphaCutoff; break;
                case AlphaMode::Blend: gMat.alphaMode = "BLEND"; break;
                default:               gMat.alphaMode = "OPAQUE";
            }

            Logger::Instance().LogInfo(
                "GltfBuilder::AddMaterials: \"" + mcMat.name + "\"" +
                " baseColor=(" + std::to_string(mcMat.baseColor.x) + "," +
                                 std::to_string(mcMat.baseColor.y) + "," +
                                 std::to_string(mcMat.baseColor.z) + "," +
                                 std::to_string(mcMat.baseColor.w) + ")" +
                " alphaMode=" + gMat.alphaMode +
                " doubleSided=" + (gMat.doubleSided ? "true" : "false"));

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

    // ---- Skins（Phase15）----
    void AddSkins(const Scene& scene)
    {
        if (scene.skeletons.empty() || scene.skins.empty()) return;

        Logger::Instance().LogInfo(
            "GltfBuilder::AddSkins: skeletons=" + std::to_string(scene.skeletons.size()) +
            " skins=" + std::to_string(scene.skins.size()));

        // skeleton/mesh → bone node index 查找
        std::unordered_map<ObjectID, int> boneNodeIdx;
        for (size_t i = 0; i < scene.nodes.size(); ++i)
            boneNodeIdx[scene.nodes[i].id] = (int)i;

        for (size_t si = 0; si < scene.skins.size(); ++si)
        {
            const auto& mcSkin = scene.skins[si];
            const Skeleton* skel = nullptr;
            for (const auto& s : scene.skeletons)
                if (s.id == mcSkin.skeletonId) { skel = &s; break; }
            if (!skel || skel->bones.empty()) continue;

            tinygltf::Skin gSkin;
            gSkin.name = mcSkin.name;

            // joints: gltf node indices (通过 bone.name → node.name)
            size_t boneCount = skel->bones.size();
            gSkin.joints.resize(boneCount, INVALID_ID);
            int matchedCount = 0;
            int unmatchedCount = 0;
            for (size_t bi = 0; bi < boneCount; ++bi)
            {
                for (size_t ni = 0; ni < scene.nodes.size(); ++ni)
                {
                    if (scene.nodes[ni].name == skel->bones[bi].name)
                    { gSkin.joints[bi] = (int)ni; ++matchedCount; break; }
                }
                if (gSkin.joints[bi] == INVALID_ID)
                {
                    ++unmatchedCount;
                    if (unmatchedCount <= 5)
                        Logger::Instance().LogWarn(
                            "GltfBuilder::AddSkins: bone \"" + skel->bones[bi].name +
                            "\" has no matching scene node (skin[" + std::to_string(si) + "])");
                }
            }

            // 诊断日志：打印每个 joint node 的类型、父子关系（ChatGPT 建议排查 hierarchy）
            Logger::Instance().LogInfo(
                "  skin[" + std::to_string(si) + "] joint nodes hierarchy:");
            std::unordered_set<int> jointSet(gSkin.joints.begin(), gSkin.joints.end());
            for (size_t bi = 0; bi < boneCount; ++bi)
            {
                int ni = gSkin.joints[bi];
                if (ni < 0 || ni >= (int)scene.nodes.size()) continue;
                const auto& nd = scene.nodes[ni];
                // 向上追溯 parent 链
                std::string parentChain = nd.name;
                ObjectID pid = nd.parent;
                int depth = 0;
                while (pid != INVALID_ID && depth < 10)
                {
                    auto pIt = boneNodeIdx.find(pid);
                    if (pIt != boneNodeIdx.end())
                        parentChain = scene.nodes[pIt->second].name + " → " + parentChain;
                    else
                        parentChain = "?(id=" + std::to_string(pid) + ") → " + parentChain;
                    pid = (pIt != boneNodeIdx.end() && pIt->second < (int)scene.nodes.size())
                        ? scene.nodes[pIt->second].parent : INVALID_ID;
                    if (pIt == boneNodeIdx.end() || pIt->second >= (int)scene.nodes.size()) break;
                    ++depth;
                }
                Logger::Instance().LogInfo(
                    "    joint[" + std::to_string(bi) + "] node=" + std::to_string(ni) +
                    " \"" + nd.name + "\" type=" + std::to_string((int)nd.type) +
                    " children=" + std::to_string(nd.children.size()) +
                    " chain: " + parentChain);
            }

            // skeleton root node：GLTF 规范中 skeleton 就是 root joint（joints[0]）
            // 大多数工具（包括 Blender）依赖它来识别骨架层级
            if (!gSkin.joints.empty() && gSkin.joints[0] >= 0)
                gSkin.skeleton = gSkin.joints[0];

            // 日志：joint 索引
            Logger::Instance().LogInfo(
                "  skin[" + std::to_string(si) + "] name=\"" + gSkin.name + "\"" +
                " skeletonId=" + std::to_string(mcSkin.skeletonId) +
                " meshId=" + std::to_string(mcSkin.meshId) +
                " joints=" + std::to_string(matchedCount) + "/" + std::to_string(boneCount) +
                (unmatchedCount > 0 ? " (unmatched=" + std::to_string(unmatchedCount) + ")" : ""));
            std::string jointStr = "    joints: [";
            for (size_t bi = 0; bi < std::min(boneCount, size_t(10)); ++bi)
            {
                if (bi > 0) jointStr += ",";
                jointStr += std::to_string(gSkin.joints[bi]);
            }
            if (boneCount > 10) jointStr += ",...";
            jointStr += "]";
            Logger::Instance().LogInfo(jointStr);

            // inverseBindMatrices
            std::vector<float> ibm(boneCount * 16);
            for (size_t bi = 0; bi < boneCount; ++bi)
            {
                const float* m = skel->bones[bi].inverseBindPose.m;
                for (int i = 0; i < 16; ++i)
                    ibm[bi * 16 + i] = m[i];
            }

            // 日志：第一个骨骼的 IBM 矩阵（诊断用）
            if (boneCount > 0)
            {
                const float* m0 = &ibm[0];
                Logger::Instance().LogInfo(
                    "    IBM[0] row0=(" + std::to_string(m0[0]) + "," + std::to_string(m0[4]) + "," + std::to_string(m0[8]) + "," + std::to_string(m0[12]) + ")");
                Logger::Instance().LogInfo(
                    "    IBM[0] row1=(" + std::to_string(m0[1]) + "," + std::to_string(m0[5]) + "," + std::to_string(m0[9]) + "," + std::to_string(m0[13]) + ")");
                Logger::Instance().LogInfo(
                    "    IBM[0] row2=(" + std::to_string(m0[2]) + "," + std::to_string(m0[6]) + "," + std::to_string(m0[10]) + "," + std::to_string(m0[14]) + ")");
                Logger::Instance().LogInfo(
                    "    IBM[0] row3=(" + std::to_string(m0[3]) + "," + std::to_string(m0[7]) + "," + std::to_string(m0[11]) + "," + std::to_string(m0[15]) + ")");
            }

            gSkin.inverseBindMatrices = PushAccessor(
                ibm.data(), ibm.size() * sizeof(float),
                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_MAT4,
                (int)boneCount, 0);

            m_model.skins.push_back(std::move(gSkin));
        }

        Logger::Instance().LogInfo(
            "GltfExporter: exported " + std::to_string(m_model.skins.size()) + " skin(s).");
    }

    // skin 查找：meshId → gltf skin index (从 AddSkins 填充)
    std::unordered_map<ObjectID, int> m_skinToMeshIdx;

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

            // Phase15: 蒙皮权重 → JOINTS_0 + WEIGHTS_0
            if (!mcMesh.skinInfluences.empty())
            {
                // 统计蒙皮权重
                size_t skinnedVerts = 0;
                size_t totalInfs = 0;
                float maxW = 0.0f;
                uint16_t maxJoint = 0;
                uint16_t minJoint = 65535;
                for (const auto& infs : mcMesh.skinInfluences)
                {
                    if (!infs.empty())
                    {
                        ++skinnedVerts;
                        totalInfs += infs.size();
                        for (const auto& inf : infs)
                        {
                            maxW = std::max(maxW, inf.weight);
                            maxJoint = std::max(maxJoint, inf.joint);
                            minJoint = std::min(minJoint, inf.joint);
                        }
                    }
                }
                Logger::Instance().LogInfo(
                    "GltfBuilder::AddMeshes: mesh=\"" + mcMesh.name + "\"" +
                    " positions=" + std::to_string(mcMesh.positions.size()) +
                    " skinnedVerts=" + std::to_string(skinnedVerts) +
                    " totalInfs=" + std::to_string(totalInfs) +
                    " maxJointIdx=" + std::to_string(maxJoint) +
                    " minJointIdx=" + std::to_string(minJoint) +
                    " maxWeight=" + std::to_string(maxW));

                std::vector<uint16_t> joints;
                std::vector<float>   weights;
                joints.reserve(mcMesh.positions.size() * 4);
                weights.reserve(mcMesh.positions.size() * 4);
                for (size_t vi = 0; vi < mcMesh.positions.size(); ++vi)
                {
                    const auto& infs = vi < mcMesh.skinInfluences.size()
                                       ? mcMesh.skinInfluences[vi]
                                       : std::vector<VertexInfluence>{};
                    for (int c = 0; c < 4; ++c)
                    {
                        if (c < (int)infs.size()) { joints.push_back(infs[c].joint); weights.push_back(infs[c].weight); }
                        else { joints.push_back(0); weights.push_back(0.0f); }
                    }
                }
                int jAcc = PushAccessor(joints.data(), joints.size() * sizeof(uint16_t),
                                         TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC4,
                                         (int)mcMesh.positions.size(), TINYGLTF_TARGET_ARRAY_BUFFER);
                int wAcc = PushAccessor(weights.data(), weights.size() * sizeof(float),
                                         TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4,
                                         (int)mcMesh.positions.size(), TINYGLTF_TARGET_ARRAY_BUFFER);
                // 写入所有 primitive
                for (auto& prim : gMesh.primitives)
                {
                    prim.attributes["JOINTS_0"] = jAcc;
                    prim.attributes["WEIGHTS_0"] = wAcc;
                }

                Logger::Instance().LogInfo(
                    "  JOINTS_0 accessor=" + std::to_string(jAcc) +
                    " WEIGHTS_0 accessor=" + std::to_string(wAcc));
            }

            m_meshIdxMap[mcMesh.id] = (int)m_model.meshes.size();
            m_model.meshes.push_back(std::move(gMesh));
        }
    }

    // ---- Nodes ----
    void AddNodes(const Scene& scene)
    {
        // Phase15: 查找 skin → mesh 映射
        std::unordered_map<ObjectID, int> meshSkinIdx;
        for (size_t i = 0; i < scene.skins.size(); ++i)
            meshSkinIdx[scene.skins[i].meshId] = (int)i;

        Logger::Instance().LogInfo(
            "GltfBuilder::AddNodes: total nodes=" + std::to_string(scene.nodes.size()) +
            " rootNodes=" + std::to_string(scene.rootNodes.size()));

        int meshNodeCount = 0;
        int skinnedNodeCount = 0;
        int boneNodeCount = 0;

        for (const auto& mcNode : scene.nodes)
        {
            tinygltf::Node gNode;
            gNode.name = mcNode.name;
            if (!mcNode.meshIds.empty())
            {
                if (auto it = m_meshIdxMap.find(mcNode.meshIds[0]); it != m_meshIdxMap.end())
                    gNode.mesh = it->second;
                // 设置 skin 引用
                auto si = meshSkinIdx.find(mcNode.meshIds[0]);
                if (si != meshSkinIdx.end())
                {
                    gNode.skin = si->second;
                    ++skinnedNodeCount;
                    Logger::Instance().LogInfo(
                        "  node \"" + mcNode.name + "\" meshIdx=" + std::to_string(gNode.mesh) +
                        " skinIdx=" + std::to_string(gNode.skin) +
                        " type=" + (mcNode.type == NodeType::Bone ? "Bone" : "Mesh"));
                }
                ++meshNodeCount;
            }

            if (mcNode.type == NodeType::Bone)
                ++boneNodeCount;

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

        Logger::Instance().LogInfo(
            "  nodes summary: meshNodes=" + std::to_string(meshNodeCount) +
            " skinnedNodes=" + std::to_string(skinnedNodeCount) +
            " boneNodes=" + std::to_string(boneNodeCount));

        for (const auto& mcNode : scene.nodes)
        {
            int parentIdx = m_nodeIdxMap[mcNode.id];
            for (ObjectID childId : mcNode.children)
                if (auto it = m_nodeIdxMap.find(childId); it != m_nodeIdxMap.end())
                    m_model.nodes[parentIdx].children.push_back(it->second);
        }
    }

    // ---- 辅助：列主序矩阵 → TRS（仅动画节点使用）----
    static void DecomposeMatrixToTRS(const float* m, tinygltf::Node& gNode)
    {
        gNode.translation = {static_cast<double>(m[12]),
                              static_cast<double>(m[13]),
                              static_cast<double>(m[14])};
        float sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        float sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
        float sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
        gNode.scale = {static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz)};

        float r00=m[0]/sx, r01=m[4]/sy, r02=m[8]/sz;
        float r10=m[1]/sx, r11=m[5]/sy, r12=m[9]/sz;
        float r20=m[2]/sx, r21=m[6]/sy, r22=m[10]/sz;
        float trace = r00+r11+r22, qx, qy, qz, qw;
        if (trace > 0.0f) {
            float s = std::sqrt(trace+1.0f)*2.0f; qw=0.25f*s;
            qx=(r21-r12)/s; qy=(r02-r20)/s; qz=(r10-r01)/s;
        } else if (r00>r11 && r00>r22) {
            float s = std::sqrt(1.0f+r00-r11-r22)*2.0f; qw=(r21-r12)/s;
            qx=0.25f*s; qy=(r01+r10)/s; qz=(r02+r20)/s;
        } else if (r11>r22) {
            float s = std::sqrt(1.0f+r11-r00-r22)*2.0f; qw=(r02-r20)/s;
            qx=(r01+r10)/s; qy=0.25f*s; qz=(r12+r21)/s;
        } else {
            float s = std::sqrt(1.0f+r22-r00-r11)*2.0f; qw=(r10-r01)/s;
            qx=(r02+r20)/s; qy=(r12+r21)/s; qz=0.25f*s;
        }
        float qLen = std::sqrt(qx*qx+qy*qy+qz*qz+qw*qw);
        if (qLen > 1e-6f) { qx/=qLen; qy/=qLen; qz/=qLen; qw/=qLen; }
        else { qx=0.0f; qy=0.0f; qz=0.0f; qw=1.0f; }
        gNode.rotation = {static_cast<double>(qx),static_cast<double>(qy),
                           static_cast<double>(qz),static_cast<double>(qw)};
    }

    // 将被动画 channel 引用的节点从 matrix 改为 TRS（Gltf 规范要求）
    void FixAnimatedNodeMatrices()
    {
        std::unordered_set<int> animatedNodes;
        for (const auto& anim : m_model.animations)
            for (const auto& ch : anim.channels)
                animatedNodes.insert(ch.target_node);
        Logger::Instance().LogInfo(
            "GltfBuilder::FixAnimatedNodeMatrices: " + std::to_string(animatedNodes.size()) +
            " nodes with animation will be converted from matrix to TRS.");
        for (int idx : animatedNodes)
        {
            auto& node = m_model.nodes[idx];
            if (node.matrix.empty()) continue;
            std::vector<double> mat = std::move(node.matrix);
            node.matrix.clear();
            float mf[16];
            for (int i=0;i<16;++i) mf[i] = static_cast<float>(mat[i]);
            DecomposeMatrixToTRS(mf, node);
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

    // ---- Animations（Phase14）----
    void AddAnimations(const Scene& scene)
    {
        for (const auto& clip : scene.animations)
        {
            if (clip.nodeChannels.empty() && clip.morphChannels.empty())
                continue;

            tinygltf::Animation gAnim;
            gAnim.name = clip.name;

            int tCount = 0, rCount = 0, sCount = 0;

            // 遍历所有 NodeAnimation 通道
            for (const auto& nodeAnim : clip.nodeChannels)
            {
                auto nodeIt = m_nodeIdxMap.find(nodeAnim.nodeId);
                if (nodeIt == m_nodeIdxMap.end()) continue;
                int targetNodeIdx = nodeIt->second;

                // Translation
                if (!nodeAnim.translation.keys.empty())
                {
                    AddChannel(gAnim, nodeAnim.translation, targetNodeIdx, "translation");
                    ++tCount;
                }

                // Rotation
                if (!nodeAnim.rotation.keys.empty())
                {
                    AddChannel(gAnim, nodeAnim.rotation, targetNodeIdx, "rotation");
                    ++rCount;
                }

                // Scale
                if (!nodeAnim.scale.keys.empty())
                {
                    AddChannel(gAnim, nodeAnim.scale, targetNodeIdx, "scale");
                    ++sCount;
                }
            }

            if (!gAnim.channels.empty())
            {
                // 统计各插值类型的通道数量
                int linCount = 0, stepCount = 0, cubicCount = 0;
                for (const auto& s : gAnim.samplers)
                {
                    if (s.interpolation == "LINEAR") ++linCount;
                    else if (s.interpolation == "STEP") ++stepCount;
                    else if (s.interpolation == "CUBICSPLINE") ++cubicCount;
                }

                Logger::Instance().LogInfo(
                    "GltfBuilder::AddAnimations: clip=\"" + gAnim.name + "\"" +
                    " channels=" + std::to_string(gAnim.channels.size()) +
                    " samplers=" + std::to_string(gAnim.samplers.size()) +
                    " interpolate=(L=" + std::to_string(linCount) +
                    " S=" + std::to_string(stepCount) +
                    " C=" + std::to_string(cubicCount) + ")" +
                    " (T=" + std::to_string(tCount) + " R=" + std::to_string(rCount) + " S=" + std::to_string(sCount) + ")");
                m_model.animations.push_back(std::move(gAnim));
            }
        }

        // 修复：动画 channel 节点不能同时有 matrix 属性，改为 TRS
        FixAnimatedNodeMatrices();

        if (!scene.animations.empty())
        {
            Logger::Instance().LogInfo(
                "GltfExporter: exported " + std::to_string(m_model.animations.size()) +
                " animation clip(s).");
        }
    }

private:
    // 将 TrackVec3 写入 sampler + channel
    void AddChannel(tinygltf::Animation& gAnim,
                    const TrackVec3& track,
                    int targetNodeIdx,
                    const std::string& path)
    {
        if (track.keys.empty()) return;

        bool isCubic = (track.interpolation == AnimationInterpolation::CubicSpline);
        size_t keyCount = track.keys.size();

        // 构建时间戳数组
        std::vector<float> times;
        times.reserve(keyCount);
        for (const auto& kf : track.keys)
            times.push_back(static_cast<float>(kf.time));

        // 构建值数组
        size_t perFrame = isCubic ? 9 : 3;  // inTan(xyz) + value(xyz) + outTan(xyz)
        std::vector<float> values;
        values.reserve(keyCount * perFrame);
        for (const auto& kf : track.keys)
        {
            if (isCubic) { values.push_back(kf.inTan.x);  values.push_back(kf.inTan.y);  values.push_back(kf.inTan.z); }
            values.push_back(kf.value.x); values.push_back(kf.value.y); values.push_back(kf.value.z);
            if (isCubic) { values.push_back(kf.outTan.x); values.push_back(kf.outTan.y); values.push_back(kf.outTan.z); }
        }

        int inputAcc = PushAccessor(
            times.data(), times.size() * sizeof(float),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_SCALAR,
            (int)times.size(), 0,
            {static_cast<double>(times.front())},
            {static_cast<double>(times.back())});

        // CubicSpline 规范：count = 3 * keyCount（in-tangent + value + out-tangent 各一组）
        int outputCount = isCubic ? (int)(keyCount * 3) : (int)keyCount;
        int outputAcc = PushAccessor(
            values.data(), values.size() * sizeof(float),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3,
            outputCount, 0);

        // 运行时校验：input 和 output 逻辑 key 数必须一致
        int inputLogicalKeys = (int)times.size();
        int outputLogicalKeys = isCubic ? (outputCount / 3) : outputCount;
        if (inputLogicalKeys != outputLogicalKeys)
            Logger::Instance().LogError(
                "GltfBuilder::AddChannel(VEC3): count mismatch! inputKeys=" +
                std::to_string(inputLogicalKeys) + " outputKeys=" + std::to_string(outputLogicalKeys) +
                " path=" + path + " interp=" + InterpToString(track.interpolation));

        tinygltf::AnimationSampler sampler;
        sampler.input         = inputAcc;
        sampler.output        = outputAcc;
        sampler.interpolation = InterpToString(track.interpolation);
        int samplerIdx = (int)gAnim.samplers.size();
        gAnim.samplers.push_back(std::move(sampler));

        tinygltf::AnimationChannel channel;
        channel.sampler       = samplerIdx;
        channel.target_node   = targetNodeIdx;
        channel.target_path   = path;
        gAnim.channels.push_back(std::move(channel));
    }

    // 将 TrackQuat 写入 sampler + channel
    void AddChannel(tinygltf::Animation& gAnim,
                    const TrackQuat& track,
                    int targetNodeIdx,
                    const std::string& path)
    {
        if (track.keys.empty()) return;

        bool isCubic = (track.interpolation == AnimationInterpolation::CubicSpline);
        size_t keyCount = track.keys.size();

        std::vector<float> times;
        times.reserve(keyCount);
        for (const auto& kf : track.keys)
            times.push_back(static_cast<float>(kf.time));

        size_t perFrame = isCubic ? 12 : 4;  // inTan(xyzw) + value(xyzw) + outTan(xyzw)
        std::vector<float> values;
        values.reserve(keyCount * perFrame);
        for (const auto& kf : track.keys)
        {
            if (isCubic) { values.push_back(kf.inTan.x); values.push_back(kf.inTan.y); values.push_back(kf.inTan.z); values.push_back(kf.inTan.w); }
            values.push_back(kf.value.x); values.push_back(kf.value.y); values.push_back(kf.value.z); values.push_back(kf.value.w);
            if (isCubic) { values.push_back(kf.outTan.x); values.push_back(kf.outTan.y); values.push_back(kf.outTan.z); values.push_back(kf.outTan.w); }
        }

        int inputAcc = PushAccessor(
            times.data(), times.size() * sizeof(float),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_SCALAR,
            (int)times.size(), 0,
            {static_cast<double>(times.front())},
            {static_cast<double>(times.back())});

        // CubicSpline 规范：count = 3 * keyCount（in-tangent + value + out-tangent 各一组）
        int outputCount = isCubic ? (int)(keyCount * 3) : (int)keyCount;
        int outputAcc = PushAccessor(
            values.data(), values.size() * sizeof(float),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4,
            outputCount, 0);

        // 运行时校验：input 和 output 逻辑 key 数必须一致
        int inputLogicalKeys = (int)times.size();
        int outputLogicalKeys = isCubic ? (outputCount / 3) : outputCount;
        if (inputLogicalKeys != outputLogicalKeys)
            Logger::Instance().LogError(
                "GltfBuilder::AddChannel(VEC4): count mismatch! inputKeys=" +
                std::to_string(inputLogicalKeys) + " outputKeys=" + std::to_string(outputLogicalKeys) +
                " path=" + path + " interp=" + InterpToString(track.interpolation));

        tinygltf::AnimationSampler sampler;
        sampler.input         = inputAcc;
        sampler.output        = outputAcc;
        sampler.interpolation = InterpToString(track.interpolation);
        int samplerIdx = (int)gAnim.samplers.size();
        gAnim.samplers.push_back(std::move(sampler));

        tinygltf::AnimationChannel channel;
        channel.sampler       = samplerIdx;
        channel.target_node   = targetNodeIdx;
        channel.target_path   = path;
        gAnim.channels.push_back(std::move(channel));
    }

    static const char* InterpToString(AnimationInterpolation interp)
    {
        switch (interp)
        {
            case AnimationInterpolation::Step:        return "STEP";
            case AnimationInterpolation::CubicSpline: return "CUBICSPLINE";
            default:                                   return "LINEAR";
        }
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
