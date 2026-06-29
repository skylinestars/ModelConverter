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
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mc {

// ============================================================
// 矩阵工具（AddSkins 中用于计算 extra joint IBM）
// 列主序 float[16]，m[col*4+row]
// ============================================================

// C = A × B
static void MatMul4x4(const float* A, const float* B, float* C)
{
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += A[k * 4 + row] * B[col * 4 + k];
            C[col * 4 + row] = s;
        }
}

// 仿射 TRS 矩阵求逆（无 shear），out = m^(-1)
// RS^(-1)[r][c] = RS[c][r] / (scale_r × scale_c)，平移列 = -(RS^(-1)) × T
static void InvertAffineTRS(const float* m, float* out)
{
    float sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    float sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    if (sx < 1e-7f) sx = 1e-7f;
    if (sy < 1e-7f) sy = 1e-7f;
    if (sz < 1e-7f) sz = 1e-7f;
    const float scales[3] = {sx, sy, sz};

    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[c * 4 + r] = m[r * 4 + c] / (scales[r] * scales[c]);

    for (int r = 0; r < 3; ++r)
        out[12 + r] = -(out[r] * m[12] + out[4 + r] * m[13] + out[8 + r] * m[14]);

    out[3] = out[7] = out[11] = 0.0f;
    out[15] = 1.0f;
}

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
    std::unordered_map<ObjectID, int> m_meshToNodeIdxMap;  // meshId → gltf node index（morph 动画用）

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
    // 从 UTF-8 路径字符串中提取文件名（最后一个 '/' 或 '\' 之后的部分）
    static std::string FileNameFromPath(const std::string& path)
    {
        size_t sep = path.find_last_of("/\\");
        return (sep != std::string::npos) ? path.substr(sep + 1) : path;
    }

    static std::string MimeTypeFromPath(const std::string& path)
    {
        std::string name = FileNameFromPath(path);
        size_t dot = name.rfind('.');
        std::string ext = (dot != std::string::npos) ? name.substr(dot) : "";
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
                    size_t dot = img.name.rfind('.');
                    if (dot == std::string::npos) {
                        std::string ext = "png";
                        if (mime == "image/jpeg") ext = "jpg";
                        else if (mime == "image/webp") ext = "webp";
                        img.name = img.name + "." + ext;
                    }
                    imageFileName = FileNameFromPath(img.name);
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
                imageFileName = FileNameFromPath(mcTex.uri);
            }

            if (imageFileName.empty() && !img.name.empty()) {
                imageFileName = FileNameFromPath(img.name);
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
                // 始终将图片数据存入 buffer[0]：
                //   GLB  → 写入二进制块（embedBuffers=true）
                //   GLTF → 写入 .bin 文件（bufferView 引用）
                // 避免 tinygltf 对外部文件扩展名（.tga/.dds 等）的限制
                int bvIdx      = PushImageBufferView(rawBytes);
                img.bufferView = bvIdx;
                img.uri.clear();
                img.name = imageFileName;
            }
            else if (!mcTex.uri.empty())
            {
                img.uri = mcTex.uri;
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

            // skin.skeleton：设为 root joint 的最近非 joint 父节点。
            // Blender GLTF 导入器以 skin.skeleton 节点作为 Armature 容器，
            // 若设为 root joint 自身，Blender 自动生成的叶骨 _end 节点会挂到
            // root joint 的父节点（Empty）下，而非 Armature 内部，造成层级错乱。
            // 将 skeleton 指向 root joint 的父节点后，Blender 能正确将整个骨架归入 Armature。
            {
                std::unordered_set<int> jointSet(gSkin.joints.begin(), gSkin.joints.end());

                // 找 root joint：其父节点不是 joint
                int rootJointNi = gSkin.joints.empty() ? -1 : gSkin.joints[0];
                for (int ji : gSkin.joints)
                {
                    if (ji < 0 || ji >= (int)scene.nodes.size()) continue;
                    ObjectID parentId = scene.nodes[ji].parent;
                    bool parentIsJoint = false;
                    if (parentId != INVALID_ID)
                    {
                        auto pit = m_nodeIdxMap.find(parentId);
                        if (pit != m_nodeIdxMap.end() && jointSet.count(pit->second))
                            parentIsJoint = true;
                    }
                    if (!parentIsJoint) { rootJointNi = ji; break; }
                }

                // skin.skeleton = root joint 的父节点（若不是 joint）；否则为 root joint 本身
                if (rootJointNi >= 0 && rootJointNi < (int)scene.nodes.size())
                {
                    ObjectID parentId = scene.nodes[rootJointNi].parent;
                    bool usedParent   = false;
                    if (parentId != INVALID_ID)
                    {
                        auto pit = m_nodeIdxMap.find(parentId);
                        if (pit != m_nodeIdxMap.end() && !jointSet.count(pit->second))
                        {
                            gSkin.skeleton = pit->second;
                            usedParent     = true;
                        }
                    }
                    if (!usedParent) gSkin.skeleton = rootJointNi;
                }
            }

            // 将 joint 节点的直接非 joint 子节点（如 Blender 的 _end 骨骼）加入 joints。
            // Blender GLTF 导入器对不在 joints 中的节点生成独立 Empty 对象，
            // 放在 Armature 外部。将它们加入 joints 后 Blender 才能把它们当作
            // 非变形骨骼保留在 Armature 内部。
            std::vector<int> extraJoints;
            {
                std::unordered_set<int> jointSetX(gSkin.joints.begin(), gSkin.joints.end());
                for (int ji : gSkin.joints)
                {
                    if (ji < 0 || ji >= (int)scene.nodes.size()) continue;
                    for (ObjectID childId : scene.nodes[ji].children)
                    {
                        auto cit = m_nodeIdxMap.find(childId);
                        if (cit == m_nodeIdxMap.end()) continue;
                        int cni = cit->second;
                        if (!jointSetX.count(cni))
                        {
                            extraJoints.push_back(cni);
                            jointSetX.insert(cni);
                        }
                    }
                }
                for (int ji : extraJoints)
                    gSkin.joints.push_back(ji);
            }

            // 日志：joint 索引
            std::string skelNodeName = (gSkin.skeleton >= 0 && gSkin.skeleton < (int)scene.nodes.size())
                ? scene.nodes[gSkin.skeleton].name : "?";
            Logger::Instance().LogInfo(
                "  skin[" + std::to_string(si) + "] name=\"" + gSkin.name + "\"" +
                " skeletonId=" + std::to_string(mcSkin.skeletonId) +
                " meshId=" + std::to_string(mcSkin.meshId) +
                " joints=" + std::to_string(matchedCount) + "/" + std::to_string(boneCount) +
                (unmatchedCount > 0 ? " (unmatched=" + std::to_string(unmatchedCount) + ")" : "") +
                (!extraJoints.empty() ? " extraNonDeformJoints=" + std::to_string(extraJoints.size()) : "") +
                " skin.skeleton=" + std::to_string(gSkin.skeleton) + "(\"" + skelNodeName + "\")");

            // inverseBindMatrices：变形骨骼用真实 IBM；非变形骨骼（_end 等）用父骨骼 IBM 推算。
            // IBM_extra = R^T × local_Y^(-1) × R × IBM_parent
            // （local_Y 是 Y-up scene 空间的 localMatrix；IBM 均保持 Z-up FBX 空间，与常规骨骼一致）
            // ZUp→YUp 旋转矩阵 R（列主序）及其转置 R^T
            static constexpr float kR_ZtoY[16]  = {1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1};
            static constexpr float kRT_ZtoY[16] = {1,0,0,0, 0,0, 1,0, 0,-1,0,0, 0,0,0,1};

            // 构建 nodeId → 骨骼索引映射（用于查找父骨骼 IBM）
            std::unordered_map<ObjectID, int> nodeIdToBoneIdx;
            for (size_t bi = 0; bi < boneCount; ++bi)
            {
                int ni = gSkin.joints[bi];
                if (ni >= 0 && ni < (int)scene.nodes.size())
                    nodeIdToBoneIdx[scene.nodes[ni].id] = (int)bi;
            }

            size_t totalJoints = boneCount + extraJoints.size();
            std::vector<float> ibm(totalJoints * 16, 0.0f);
            for (size_t bi = 0; bi < boneCount; ++bi)
            {
                const float* m = skel->bones[bi].inverseBindPose.m;
                for (int i = 0; i < 16; ++i)
                    ibm[bi * 16 + i] = m[i];
            }
            for (size_t ej = 0; ej < extraJoints.size(); ++ej)
            {
                float* ibmOut = ibm.data() + (boneCount + ej) * 16;
                int extraNi = extraJoints[ej];
                bool computed = false;
                if (extraNi >= 0 && extraNi < (int)scene.nodes.size())
                {
                    ObjectID parentId = scene.nodes[extraNi].parent;
                    auto pit = nodeIdToBoneIdx.find(parentId);
                    if (pit != nodeIdToBoneIdx.end())
                    {
                        // IBM_extra = R^T × local_Y^(-1) × R × IBM_parent
                        const float* parentIBM = skel->bones[pit->second].inverseBindPose.m;
                        const float* localY    = scene.nodes[extraNi].localMatrix.m;
                        float localInv[16] = {}, tmp1[16] = {}, tmp2[16] = {};
                        InvertAffineTRS(localY, localInv);
                        MatMul4x4(kR_ZtoY, parentIBM, tmp1);   // R × IBM_parent
                        MatMul4x4(localInv, tmp1, tmp2);        // local_Y^(-1) × (R × IBM_parent)
                        MatMul4x4(kRT_ZtoY, tmp2, ibmOut);     // R^T × (...)
                        computed = true;
                    }
                }
                if (!computed)
                {
                    // 回退：identity（父节点找不到骨骼索引时）
                    ibmOut[0] = ibmOut[5] = ibmOut[10] = ibmOut[15] = 1.0f;
                }
            }

            gSkin.inverseBindMatrices = PushAccessor(
                ibm.data(), ibm.size() * sizeof(float),
                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_MAT4,
                (int)totalJoints, 0);

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
                    // 按权重降序取前4个影响，并归一化，避免 bone-index 顺序导致高权重影响被截断
                    auto infs = (vi < mcMesh.skinInfluences.size())
                                ? mcMesh.skinInfluences[vi]
                                : std::vector<VertexInfluence>{};
                    std::sort(infs.begin(), infs.end(), [](const VertexInfluence& a, const VertexInfluence& b) {
                        return a.weight > b.weight;
                    });
                    if (infs.size() > 4) infs.resize(4);
                    float totalW = 0.0f;
                    for (const auto& inf : infs) totalW += inf.weight;
                    if (totalW > 1e-6f)
                        for (auto& inf : infs) inf.weight /= totalW;

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

            // ---- Morph Targets（BlendShape）----
            if (!mcMesh.morphTargets.empty())
            {
                for (const auto& mt : mcMesh.morphTargets)
                {
                    int posAcc = PushAccessor(
                        mt.positionDeltas.data(),
                        mt.positionDeltas.size() * sizeof(Vec3),
                        TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3,
                        (int)mt.positionDeltas.size(), 0);
                    std::map<std::string, int> target;
                    target["POSITION"] = posAcc;
                    for (auto& prim : gMesh.primitives)
                        prim.targets.push_back(target);
                }
                gMesh.weights.assign(mcMesh.morphTargets.size(), 0.0);
                Logger::Instance().LogInfo(
                    "  morphTargets=" + std::to_string(mcMesh.morphTargets.size()) +
                    " for mesh=\"" + mcMesh.name + "\"");
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

            int gNodeIdx = (int)m_model.nodes.size();
            m_nodeIdxMap[mcNode.id] = gNodeIdx;
            for (ObjectID meshId : mcNode.meshIds)
                m_meshToNodeIdxMap[meshId] = gNodeIdx;
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

    // ---- 将 nodeChannels 写入 gAnim（T/R/S 通道）----
    void AddNodeTrsChannels(const AnimationClip& clip, tinygltf::Animation& gAnim,
                            int& tCount, int& rCount, int& sCount)
    {
        for (const auto& nodeAnim : clip.nodeChannels)
        {
            auto nodeIt = m_nodeIdxMap.find(nodeAnim.nodeId);
            if (nodeIt == m_nodeIdxMap.end()) continue;
            int targetNodeIdx = nodeIt->second;

            if (!nodeAnim.translation.keys.empty())
                { AddChannel(gAnim, nodeAnim.translation, targetNodeIdx, "translation"); ++tCount; }
            if (!nodeAnim.rotation.keys.empty())
                { AddChannel(gAnim, nodeAnim.rotation, targetNodeIdx, "rotation"); ++rCount; }
            if (!nodeAnim.scale.keys.empty())
                { AddChannel(gAnim, nodeAnim.scale, targetNodeIdx, "scale"); ++sCount; }
        }
    }

    // ---- 将 morphChannels 写入 gAnim（weights 通道，按 meshId 合并）----
    void AddMorphWeightChannels(const AnimationClip& clip, tinygltf::Animation& gAnim,
                                int& morphChCount)
    {
        if (clip.morphChannels.empty()) return;

        // 按 meshId 分组
        std::unordered_map<ObjectID, std::vector<const MorphAnimation*>> meshMorphMap;
        for (const auto& ma : clip.morphChannels)
            meshMorphMap[ma.meshId].push_back(&ma);

        for (const auto& [meshId, morphAnims] : meshMorphMap)
        {
            auto nodeIt = m_meshToNodeIdxMap.find(meshId);
            if (nodeIt == m_meshToNodeIdxMap.end()) continue;
            int targetNodeIdx = nodeIt->second;

            auto meshIt = m_meshIdxMap.find(meshId);
            if (meshIt == m_meshIdxMap.end()) continue;
            int morphCount = (int)m_model.meshes[meshIt->second].weights.size();
            if (morphCount == 0) continue;

            // 各通道 key 时间的并集
            std::set<float> allTimesSet;
            for (const auto* ma : morphAnims)
                for (const auto& kf : ma->weights.keys)
                    allTimesSet.insert((float)kf.time);

            std::vector<float> times(allTimesSet.begin(), allTimesSet.end());
            if (times.empty()) continue;

            AnimationInterpolation interp = morphAnims[0]->weights.interpolation;

            // 每个时刻展开为 [w0, w1, ..., wN]，STEP 评估
            std::vector<float> values;
            values.reserve(times.size() * morphCount);
            for (float t : times)
            {
                for (int mi = 0; mi < morphCount; ++mi)
                {
                    float w = 0.0f;
                    for (const auto* ma : morphAnims)
                    {
                        if ((int)ma->morphIndex != mi) continue;
                        for (const auto& kf : ma->weights.keys)
                        {
                            if ((float)kf.time <= t) w = kf.value;
                            else break;
                        }
                        break;
                    }
                    values.push_back(w);
                }
            }

            int inputAcc = PushAccessor(
                times.data(), times.size() * sizeof(float),
                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_SCALAR,
                (int)times.size(), 0,
                {static_cast<double>(times.front())},
                {static_cast<double>(times.back())});

            int outputAcc = PushAccessor(
                values.data(), values.size() * sizeof(float),
                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_SCALAR,
                (int)(times.size() * morphCount), 0);

            tinygltf::AnimationSampler sampler;
            sampler.input         = inputAcc;
            sampler.output        = outputAcc;
            sampler.interpolation = InterpToString(interp);
            int samplerIdx = (int)gAnim.samplers.size();
            gAnim.samplers.push_back(std::move(sampler));

            tinygltf::AnimationChannel channel;
            channel.sampler     = samplerIdx;
            channel.target_node = targetNodeIdx;
            channel.target_path = "weights";
            gAnim.channels.push_back(std::move(channel));
            ++morphChCount;
        }
    }

    // ---- Animations —— 编排层（Phase14）----
    void AddAnimations(const Scene& scene)
    {
        for (const auto& clip : scene.animations)
        {
            if (clip.nodeChannels.empty() && clip.morphChannels.empty()) continue;

            tinygltf::Animation gAnim;
            gAnim.name = clip.name;

            int tCount = 0, rCount = 0, sCount = 0;
            AddNodeTrsChannels(clip, gAnim, tCount, rCount, sCount);

            int morphChCount = 0;
            AddMorphWeightChannels(clip, gAnim, morphChCount);

            if (!gAnim.channels.empty())
            {
                int linCount = 0, stepCount = 0, cubicCount = 0;
                for (const auto& s : gAnim.samplers)
                {
                    if (s.interpolation == "LINEAR") ++linCount;
                    else if (s.interpolation == "STEP") ++stepCount;
                    else ++cubicCount;
                }
                Logger::Instance().LogInfo(
                    "GltfBuilder::AddAnimations: clip=\"" + gAnim.name + "\"" +
                    " channels=" + std::to_string(gAnim.channels.size()) +
                    " samplers=" + std::to_string(gAnim.samplers.size()) +
                    " interpolate=(L=" + std::to_string(linCount) +
                    " S=" + std::to_string(stepCount) +
                    " C=" + std::to_string(cubicCount) + ")" +
                    " (T=" + std::to_string(tCount) + " R=" + std::to_string(rCount) +
                    " S=" + std::to_string(sCount) + " morph=" + std::to_string(morphChCount) + ")");
                m_model.animations.push_back(std::move(gAnim));
            }
        }

        FixAnimatedNodeMatrices();

        if (!scene.animations.empty())
            Logger::Instance().LogInfo(
                "GltfExporter: exported " + std::to_string(m_model.animations.size()) +
                " animation clip(s).");
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
