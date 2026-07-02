#include "GltfExporter.h"
#include "GltfCommonUtils.h"
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

namespace mc
{
    // ============================================================
    // GltfBuilder —— 将 mc::Scene 填充到 tinygltf::Model
    // ============================================================
    class GltfBuilder
    {
    public:
        GltfBuilder(tinygltf::Model &model, const ExportOptions &opts, bool embedImages)
            : m_model(model), m_opts(opts), m_embedImages(embedImages)
        {
            m_model.asset.version = "2.0";
            m_model.asset.generator = "mc GltfExporter Phase11";
            m_model.buffers.emplace_back(); // buffer[0]
        }

        void Build(const Scene &scene)
        {
            AddTextures(scene);
            AddMaterials(scene);
            AddMeshes(scene);
            AddNodes(scene);
            AddSkins(scene);
            AddScene(scene);
            AddAnimations(scene);
        }

        const std::unordered_map<ObjectID, int> &MeshIdxMap() const { return m_meshIdxMap; }

    private:
        tinygltf::Model &m_model;
        const ExportOptions &m_opts;
        bool m_embedImages;

        std::unordered_map<ObjectID, int> m_texIdxMap;
        std::unordered_map<ObjectID, int> m_matIdxMap;
        std::unordered_map<ObjectID, int> m_meshIdxMap;
        std::unordered_map<ObjectID, int> m_nodeIdxMap;
        std::unordered_map<ObjectID, int> m_meshToNodeIdxMap; // meshId → gltf node index（morph 动画用）

        // ---- 辅助：向 buffer[0] 追加数据并 4-byte 对齐，返回 accessor 索引 ----
        int PushAccessor(const void *data, size_t bytes,
                         int componentType, int type, int count,
                         int bufTarget,
                         std::vector<double> minV = {},
                         std::vector<double> maxV = {})
        {
            auto &buf = m_model.buffers[0].data;
            size_t byteOffset = buf.size();
            buf.resize(byteOffset + bytes);
            std::memcpy(buf.data() + byteOffset, data, bytes);
            while (buf.size() % 4)
                buf.push_back(0);

            tinygltf::BufferView bv;
            bv.buffer = 0;
            bv.byteOffset = (int)byteOffset;
            bv.byteLength = (int)bytes;
            bv.target = bufTarget;
            int bvIdx = (int)m_model.bufferViews.size();
            m_model.bufferViews.push_back(std::move(bv));

            tinygltf::Accessor acc;
            acc.bufferView = bvIdx;
            acc.byteOffset = 0;
            acc.componentType = componentType;
            acc.count = count;
            acc.type = type;
            acc.minValues = std::move(minV);
            acc.maxValues = std::move(maxV);
            int accIdx = (int)m_model.accessors.size();
            m_model.accessors.push_back(std::move(acc));
            return accIdx;
        }

        struct TextureBuildData
        {
            std::vector<uint8_t> rawBytes;
            std::string mime;
            std::string imageFileName;
        };

        void ResolveEmbeddedTextureData(const Texture &mcTex,
                                        tinygltf::Image &img,
                                        TextureBuildData &data) const
        {
            if (!mcTex.embedded || mcTex.embeddedData.empty())
                return;

            data.rawBytes = mcTex.embeddedData;
            data.mime = mcTex.mimeType.empty() ? "image/png" : mcTex.mimeType;

            if (img.name.empty())
                return;

            size_t dot = img.name.rfind('.');
            if (dot == std::string::npos)
            {
                std::string ext = "png";
                if (data.mime == "image/jpeg")
                    ext = "jpg";
                else if (data.mime == "image/webp")
                    ext = "webp";
                img.name = img.name + "." + ext;
            }
            data.imageFileName = ExtractFileNameFromPath(img.name);
        }

        void ResolveUriTextureData(const Scene &scene,
                                   const Texture &mcTex,
                                   TextureBuildData &data) const
        {
            if (mcTex.uri.empty())
                return;

            std::filesystem::path resolvedPath;
            std::filesystem::path texPath = std::filesystem::u8path(mcTex.uri);

            std::vector<std::filesystem::path> candidates;
            candidates.push_back(texPath);

            if (!texPath.is_absolute() && !scene.metadata.asset.sourceFile.empty())
            {
                std::filesystem::path srcFile = std::filesystem::u8path(scene.metadata.asset.sourceFile);
                std::filesystem::path srcDir = srcFile.parent_path();
                if (!srcDir.empty())
                    candidates.push_back(srcDir / texPath);
            }

            for (const auto &p : candidates)
            {
                std::ifstream ifs(p, std::ios::binary);
                if (!ifs)
                    continue;

                data.rawBytes = std::vector<uint8_t>(
                    std::istreambuf_iterator<char>(ifs), {});
                if (data.rawBytes.empty())
                    continue;

                resolvedPath = p;
                data.mime = DetectMimeTypeFromPath(p.u8string());
                break;
            }

            if (!resolvedPath.empty())
                data.imageFileName = ExtractFileNameFromPath(resolvedPath.u8string());
            else
                data.imageFileName = ExtractFileNameFromPath(mcTex.uri);
        }

        void NormalizeTextureMimeAndName(int texIdx,
                                         tinygltf::Image &img,
                                         TextureBuildData &data) const
        {
            if (!data.rawBytes.empty())
            {
                std::string detectedMime = DetectMimeTypeFromBytes(data.rawBytes);
                if (!detectedMime.empty())
                    data.mime = detectedMime;
            }

            if (data.imageFileName.empty() && !img.name.empty())
                data.imageFileName = ExtractFileNameFromPath(img.name);

            if (!data.imageFileName.empty())
                return;

            std::string ext = "png";
            if (data.mime == "image/jpeg")
                ext = "jpg";
            else if (data.mime == "image/webp")
                ext = "webp";

            data.imageFileName = "texture_" + std::to_string(texIdx) + "." + ext;
            img.name = data.imageFileName;
        }

        bool WriteTextureImage(const Texture &mcTex,
                               tinygltf::Image &img,
                               const TextureBuildData &data) const
        {
            if (!data.rawBytes.empty())
            {
                std::string mime = data.mime.empty() ? "image/png" : data.mime;
                img.mimeType = mime;
                int bvIdx = AppendImageBufferView(m_model, data.rawBytes);
                img.bufferView = bvIdx;
                img.uri.clear();
                img.name = data.imageFileName;
                return true;
            }

            if (!mcTex.uri.empty() && !m_embedImages)
            {
                img.uri = mcTex.uri;
                return true;
            }

            Logger::Instance().LogWarn(
                "GltfBuilder::AddTextures: skip texture \"" + mcTex.name +
                "\" (cannot load bytes for embedded image, uri=\"" + mcTex.uri + "\")");
            return false;
        }

        // ---- Textures ----
        void AddTextures(const Scene &scene)
        {
            int texIdx = 0;
            for (const auto &mcTex : scene.textures)
            {
                tinygltf::Image img;
                img.name = mcTex.name;

                TextureBuildData data;
                ResolveEmbeddedTextureData(mcTex, img, data);
                if (data.rawBytes.empty())
                    ResolveUriTextureData(scene, mcTex, data);

                NormalizeTextureMimeAndName(texIdx, img, data);
                if (!WriteTextureImage(mcTex, img, data))
                {
                    ++texIdx;
                    continue;
                }

                tinygltf::Texture gTex;
                gTex.name = mcTex.name;
                gTex.source = (int)m_model.images.size();
                m_model.images.push_back(std::move(img));
                m_texIdxMap[mcTex.id] = (int)m_model.textures.size();
                m_model.textures.push_back(std::move(gTex));
                ++texIdx;
            }
        }

        // ---- Materials ----
        void SetTexInfo(const TextureRef &ref, tinygltf::TextureInfo &info) const
        {
            auto it = m_texIdxMap.find(ref.textureId);
            if (it != m_texIdxMap.end())
            {
                info.index = it->second;
                info.texCoord = ref.uvSet;
            }
        }

        void AddMaterials(const Scene &scene)
        {
            for (const auto &mcMat : scene.materials)
            {
                tinygltf::Material gMat;
                gMat.name = mcMat.name;
                gMat.pbrMetallicRoughness.baseColorFactor =
                    {mcMat.baseColor.x, mcMat.baseColor.y, mcMat.baseColor.z, mcMat.baseColor.w};
                gMat.pbrMetallicRoughness.metallicFactor = mcMat.metallic;
                gMat.pbrMetallicRoughness.roughnessFactor = mcMat.roughness;

                SetTexInfo(mcMat.baseColorTexture, gMat.pbrMetallicRoughness.baseColorTexture);
                SetTexInfo(mcMat.metallicRoughnessTexture, gMat.pbrMetallicRoughness.metallicRoughnessTexture);

                if (auto it = m_texIdxMap.find(mcMat.normalTexture.textureId); it != m_texIdxMap.end())
                {
                    gMat.normalTexture.index = it->second;
                    gMat.normalTexture.texCoord = mcMat.normalTexture.uvSet;
                }
                if (auto it = m_texIdxMap.find(mcMat.emissiveTexture.textureId); it != m_texIdxMap.end())
                {
                    gMat.emissiveTexture.index = it->second;
                    gMat.emissiveTexture.texCoord = mcMat.emissiveTexture.uvSet;
                }
                if (auto it = m_texIdxMap.find(mcMat.occlusionTexture.textureId); it != m_texIdxMap.end())
                {
                    gMat.occlusionTexture.index = it->second;
                    gMat.occlusionTexture.texCoord = mcMat.occlusionTexture.uvSet;
                }

                gMat.emissiveFactor = {mcMat.emissive.x, mcMat.emissive.y, mcMat.emissive.z};

                // FBX 模型法线经常反向，强制双面渲染避免背面透明
                gMat.doubleSided = true;

                switch (mcMat.alphaMode)
                {
                case AlphaMode::Mask:
                    gMat.alphaMode = "MASK";
                    gMat.alphaCutoff = mcMat.alphaCutoff;
                    break;
                case AlphaMode::Blend:
                    gMat.alphaMode = "BLEND";
                    break;
                default:
                    gMat.alphaMode = "OPAQUE";
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
        struct MeshAttribs
        {
            int pos = -1, nrm = -1;
            std::vector<int> uvs; // uvs[i] 对应 TEXCOORD_i 的 accessor 下标，-1 表示该套 UV 未写出
        };

        MeshAttribs BuildMeshAttribs(const Mesh &mcMesh)
        {
            MeshAttribs r;

            Vec3 vmin{1e30f, 1e30f, 1e30f}, vmax{-1e30f, -1e30f, -1e30f};
            for (const auto &p : mcMesh.positions)
            {
                vmin.x = std::min(vmin.x, p.x);
                vmin.y = std::min(vmin.y, p.y);
                vmin.z = std::min(vmin.z, p.z);
                vmax.x = std::max(vmax.x, p.x);
                vmax.y = std::max(vmax.y, p.y);
                vmax.z = std::max(vmax.z, p.z);
            }
            r.pos = PushAccessor(
                mcMesh.positions.data(), mcMesh.positions.size() * sizeof(Vec3),
                TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, (int)mcMesh.positions.size(),
                TINYGLTF_TARGET_ARRAY_BUFFER,
                {vmin.x, vmin.y, vmin.z}, {vmax.x, vmax.y, vmax.z});

            if (m_opts.exportNormals && mcMesh.normals.size() == mcMesh.positions.size())
                r.nrm = PushAccessor(
                    mcMesh.normals.data(), mcMesh.normals.size() * sizeof(Vec3),
                    TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, (int)mcMesh.normals.size(),
                    TINYGLTF_TARGET_ARRAY_BUFFER);

            // 逐套导出 UV（TEXCOORD_0/1/2...），法线贴图等常用非 0 号 UV 集，
            // 若只导出第一套会导致对应 UV 通道在 glTF 里缺失
            if (m_opts.exportUVs)
            {
                r.uvs.resize(mcMesh.uvs.size(), -1);
                for (size_t i = 0; i < mcMesh.uvs.size(); ++i)
                {
                    if (mcMesh.uvs[i].size() != mcMesh.positions.size())
                        continue;
                    r.uvs[i] = PushAccessor(
                        mcMesh.uvs[i].data(), mcMesh.uvs[i].size() * sizeof(Vec2),
                        TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, (int)mcMesh.uvs[i].size(),
                        TINYGLTF_TARGET_ARRAY_BUFFER);
                }
            }

            return r;
        }

        void AddPrimitive(tinygltf::Mesh &gMesh, const MeshAttribs &a,
                          uint32_t idxOffset, uint32_t idxCount,
                          const std::vector<uint32_t> &indices, ObjectID matId)
        {
            tinygltf::Primitive prim;
            prim.mode = TINYGLTF_MODE_TRIANGLES;
            prim.attributes["POSITION"] = a.pos;
            if (a.nrm >= 0)
                prim.attributes["NORMAL"] = a.nrm;
            for (size_t i = 0; i < a.uvs.size(); ++i)
                if (a.uvs[i] >= 0)
                    prim.attributes["TEXCOORD_" + std::to_string(i)] = a.uvs[i];

            prim.indices = PushAccessor(
                indices.data() + idxOffset, idxCount * sizeof(uint32_t),
                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, (int)idxCount,
                TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);

            if (matId != INVALID_ID)
                if (auto it = m_matIdxMap.find(matId); it != m_matIdxMap.end())
                    prim.material = it->second;

            gMesh.primitives.push_back(std::move(prim));
        }

        const Skeleton *FindSkeletonForSkin(const Scene &scene, const Skin &mcSkin) const
        {
            for (const auto &s : scene.skeletons)
                if (s.id == mcSkin.skeletonId)
                    return &s;
            return nullptr;
        }

        void BuildSkinJointsFromBones(const Scene &scene,
                                      const Skeleton &skel,
                                      tinygltf::Skin &gSkin,
                                      size_t skinIndex,
                                      int &matchedCount,
                                      int &unmatchedCount) const
        {
            size_t boneCount = skel.bones.size();
            gSkin.joints.assign(boneCount, -1);
            matchedCount = 0;
            unmatchedCount = 0;

            for (size_t bi = 0; bi < boneCount; ++bi)
            {
                for (size_t ni = 0; ni < scene.nodes.size(); ++ni)
                {
                    if (scene.nodes[ni].name == skel.bones[bi].name)
                    {
                        gSkin.joints[bi] = (int)ni;
                        ++matchedCount;
                        break;
                    }
                }
                if (gSkin.joints[bi] < 0)
                {
                    ++unmatchedCount;
                    if (unmatchedCount <= 5)
                        Logger::Instance().LogWarn(
                            "GltfBuilder::AddSkins: bone \"" + skel.bones[bi].name +
                            "\" has no matching scene node (skin[" + std::to_string(skinIndex) + "])");
                }
            }
        }

        void ResolveSkinSkeletonNode(const Scene &scene, tinygltf::Skin &gSkin) const
        {
            std::unordered_set<int> jointSet(gSkin.joints.begin(), gSkin.joints.end());

            int rootJointNi = gSkin.joints.empty() ? -1 : gSkin.joints[0];
            for (int ji : gSkin.joints)
            {
                if (ji < 0 || ji >= (int)scene.nodes.size())
                    continue;

                ObjectID parentId = scene.nodes[ji].parent;
                bool parentIsJoint = false;
                if (parentId != INVALID_ID)
                {
                    auto pit = m_nodeIdxMap.find(parentId);
                    if (pit != m_nodeIdxMap.end() && jointSet.count(pit->second))
                        parentIsJoint = true;
                }

                if (!parentIsJoint)
                {
                    rootJointNi = ji;
                    break;
                }
            }

            if (rootJointNi < 0 || rootJointNi >= (int)scene.nodes.size())
                return;

            ObjectID parentId = scene.nodes[rootJointNi].parent;
            if (parentId != INVALID_ID)
            {
                auto pit = m_nodeIdxMap.find(parentId);
                if (pit != m_nodeIdxMap.end() && !jointSet.count(pit->second))
                {
                    gSkin.skeleton = pit->second;
                    return;
                }
            }

            gSkin.skeleton = rootJointNi;
        }

        std::vector<int> CollectAndAppendExtraJoints(const Scene &scene, tinygltf::Skin &gSkin) const
        {
            std::vector<int> extraJoints;
            std::unordered_set<int> jointSet(gSkin.joints.begin(), gSkin.joints.end());

            for (int ji : gSkin.joints)
            {
                if (ji < 0 || ji >= (int)scene.nodes.size())
                    continue;

                for (ObjectID childId : scene.nodes[ji].children)
                {
                    auto cit = m_nodeIdxMap.find(childId);
                    if (cit == m_nodeIdxMap.end())
                        continue;

                    int cni = cit->second;
                    if (jointSet.count(cni))
                        continue;

                    extraJoints.push_back(cni);
                    jointSet.insert(cni);
                }
            }

            for (int ji : extraJoints)
                gSkin.joints.push_back(ji);

            return extraJoints;
        }

        std::vector<float> BuildSkinInverseBindMatrices(const Scene &scene,
                                                        const Skeleton &skel,
                                                        const tinygltf::Skin &gSkin,
                                                        const std::vector<int> &extraJoints) const
        {
            static constexpr float kR_ZtoY[16] = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1};
            static constexpr float kRT_ZtoY[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1};

            size_t boneCount = skel.bones.size();
            size_t totalJoints = gSkin.joints.size();

            std::unordered_map<ObjectID, int> nodeIdToBoneIdx;
            for (size_t bi = 0; bi < boneCount && bi < gSkin.joints.size(); ++bi)
            {
                int ni = gSkin.joints[bi];
                if (ni >= 0 && ni < (int)scene.nodes.size())
                    nodeIdToBoneIdx[scene.nodes[ni].id] = (int)bi;
            }

            std::vector<float> ibm(totalJoints * 16, 0.0f);
            for (size_t bi = 0; bi < boneCount; ++bi)
            {
                const float *m = skel.bones[bi].inverseBindPose.m;
                for (int i = 0; i < 16; ++i)
                    ibm[bi * 16 + i] = m[i];
            }

            for (size_t ej = 0; ej < extraJoints.size(); ++ej)
            {
                float *ibmOut = ibm.data() + (boneCount + ej) * 16;
                int extraNi = extraJoints[ej];
                bool computed = false;

                if (extraNi >= 0 && extraNi < (int)scene.nodes.size())
                {
                    ObjectID parentId = scene.nodes[extraNi].parent;
                    auto pit = nodeIdToBoneIdx.find(parentId);
                    if (pit != nodeIdToBoneIdx.end())
                    {
                        const float *parentIBM = skel.bones[pit->second].inverseBindPose.m;
                        const float *localY = scene.nodes[extraNi].localMatrix.m;
                        float localInv[16] = {}, tmp1[16] = {}, tmp2[16] = {};
                        InvertAffineTrsMatrix(localY, localInv);
                        MultiplyMat4x4(kR_ZtoY, parentIBM, tmp1);
                        MultiplyMat4x4(localInv, tmp1, tmp2);
                        MultiplyMat4x4(kRT_ZtoY, tmp2, ibmOut);
                        computed = true;
                    }
                }

                if (!computed)
                    ibmOut[0] = ibmOut[5] = ibmOut[10] = ibmOut[15] = 1.0f;
            }

            return ibm;
        }

        void LogSkinBuildSummary(const Scene &scene,
                                 size_t skinIndex,
                                 const Skin &mcSkin,
                                 const tinygltf::Skin &gSkin,
                                 size_t boneCount,
                                 int matchedCount,
                                 int unmatchedCount,
                                 size_t extraJointCount) const
        {
            std::string skelNodeName = (gSkin.skeleton >= 0 && gSkin.skeleton < (int)scene.nodes.size())
                                           ? scene.nodes[gSkin.skeleton].name
                                           : "?";
            Logger::Instance().LogInfo(
                "  skin[" + std::to_string(skinIndex) + "] name=\"" + gSkin.name + "\"" +
                " skeletonId=" + std::to_string(mcSkin.skeletonId) +
                " meshId=" + std::to_string(mcSkin.meshId) +
                " joints=" + std::to_string(matchedCount) + "/" + std::to_string(boneCount) +
                (unmatchedCount > 0 ? " (unmatched=" + std::to_string(unmatchedCount) + ")" : "") +
                (extraJointCount > 0 ? " extraNonDeformJoints=" + std::to_string(extraJointCount) : "") +
                " skin.skeleton=" + std::to_string(gSkin.skeleton) + "(\"" + skelNodeName + "\")");
        }

        // ---- Skins（Phase15）----
        void AddSkins(const Scene &scene)
        {
            if (scene.skeletons.empty() || scene.skins.empty())
                return;

            Logger::Instance().LogInfo(
                "GltfBuilder::AddSkins: skeletons=" + std::to_string(scene.skeletons.size()) +
                " skins=" + std::to_string(scene.skins.size()));

            for (size_t si = 0; si < scene.skins.size(); ++si)
            {
                const auto &mcSkin = scene.skins[si];
                const Skeleton *skel = FindSkeletonForSkin(scene, mcSkin);
                if (!skel || skel->bones.empty())
                    continue;

                tinygltf::Skin gSkin;
                gSkin.name = mcSkin.name;

                size_t boneCount = skel->bones.size();
                int matchedCount = 0;
                int unmatchedCount = 0;
                BuildSkinJointsFromBones(scene, *skel, gSkin, si, matchedCount, unmatchedCount);
                ResolveSkinSkeletonNode(scene, gSkin);
                std::vector<int> extraJoints = CollectAndAppendExtraJoints(scene, gSkin);

                LogSkinBuildSummary(scene, si, mcSkin, gSkin,
                                    boneCount, matchedCount, unmatchedCount, extraJoints.size());

                std::vector<float> ibm = BuildSkinInverseBindMatrices(scene, *skel, gSkin, extraJoints);
                size_t totalJoints = gSkin.joints.size();

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

        void LogMeshSkinStats(const Mesh &mcMesh) const
        {
            size_t skinnedVerts = 0;
            size_t totalInfs = 0;
            float maxW = 0.0f;
            uint16_t maxJoint = 0;
            uint16_t minJoint = 65535;
            for (const auto &infs : mcMesh.skinInfluences)
            {
                if (infs.empty())
                    continue;

                ++skinnedVerts;
                totalInfs += infs.size();
                for (const auto &inf : infs)
                {
                    maxW = std::max(maxW, inf.weight);
                    maxJoint = std::max(maxJoint, inf.joint);
                    minJoint = std::min(minJoint, inf.joint);
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
        }

        void BuildSkinAttributeArrays(const Mesh &mcMesh,
                                      std::vector<uint16_t> &joints,
                                      std::vector<float> &weights) const
        {
            joints.clear();
            weights.clear();
            joints.reserve(mcMesh.positions.size() * 4);
            weights.reserve(mcMesh.positions.size() * 4);

            for (size_t vi = 0; vi < mcMesh.positions.size(); ++vi)
            {
                auto infs = (vi < mcMesh.skinInfluences.size())
                                ? mcMesh.skinInfluences[vi]
                                : std::vector<VertexInfluence>{};
                std::sort(infs.begin(), infs.end(), [](const VertexInfluence &a, const VertexInfluence &b)
                          { return a.weight > b.weight; });
                if (infs.size() > 4)
                    infs.resize(4);

                float totalW = 0.0f;
                for (const auto &inf : infs)
                    totalW += inf.weight;
                if (totalW > 1e-6f)
                    for (auto &inf : infs)
                        inf.weight /= totalW;

                for (int c = 0; c < 4; ++c)
                {
                    if (c < (int)infs.size())
                    {
                        joints.push_back(infs[c].joint);
                        weights.push_back(infs[c].weight);
                    }
                    else
                    {
                        joints.push_back(0);
                        weights.push_back(0.0f);
                    }
                }
            }
        }

        void AddSkinAttributesToMesh(const Mesh &mcMesh, tinygltf::Mesh &gMesh)
        {
            if (mcMesh.skinInfluences.empty())
                return;

            LogMeshSkinStats(mcMesh);

            std::vector<uint16_t> joints;
            std::vector<float> weights;
            BuildSkinAttributeArrays(mcMesh, joints, weights);

            int jAcc = PushAccessor(joints.data(), joints.size() * sizeof(uint16_t),
                                    TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_VEC4,
                                    (int)mcMesh.positions.size(), TINYGLTF_TARGET_ARRAY_BUFFER);
            int wAcc = PushAccessor(weights.data(), weights.size() * sizeof(float),
                                    TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC4,
                                    (int)mcMesh.positions.size(), TINYGLTF_TARGET_ARRAY_BUFFER);

            for (auto &prim : gMesh.primitives)
            {
                prim.attributes["JOINTS_0"] = jAcc;
                prim.attributes["WEIGHTS_0"] = wAcc;
            }

            Logger::Instance().LogInfo(
                "  JOINTS_0 accessor=" + std::to_string(jAcc) +
                " WEIGHTS_0 accessor=" + std::to_string(wAcc));
        }

        void AddMorphTargetsToMesh(const Mesh &mcMesh, tinygltf::Mesh &gMesh)
        {
            if (mcMesh.morphTargets.empty())
                return;

            for (const auto &mt : mcMesh.morphTargets)
            {
                Vec3 vmin{1e30f, 1e30f, 1e30f}, vmax{-1e30f, -1e30f, -1e30f};
                for (const auto &d : mt.positionDeltas)
                {
                    vmin.x = std::min(vmin.x, d.x);
                    vmin.y = std::min(vmin.y, d.y);
                    vmin.z = std::min(vmin.z, d.z);
                    vmax.x = std::max(vmax.x, d.x);
                    vmax.y = std::max(vmax.y, d.y);
                    vmax.z = std::max(vmax.z, d.z);
                }

                std::vector<double> minV;
                std::vector<double> maxV;
                if (!mt.positionDeltas.empty())
                {
                    minV = {vmin.x, vmin.y, vmin.z};
                    maxV = {vmax.x, vmax.y, vmax.z};
                }

                int posAcc = PushAccessor(
                    mt.positionDeltas.data(),
                    mt.positionDeltas.size() * sizeof(Vec3),
                    TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3,
                    (int)mt.positionDeltas.size(), 0,
                    std::move(minV), std::move(maxV));

                std::map<std::string, int> target;
                target["POSITION"] = posAcc;
                for (auto &prim : gMesh.primitives)
                    prim.targets.push_back(target);
            }

            gMesh.weights.assign(mcMesh.morphTargets.size(), 0.0);
            Logger::Instance().LogInfo(
                "  morphTargets=" + std::to_string(mcMesh.morphTargets.size()) +
                " for mesh=\"" + mcMesh.name + "\"");
        }

        // ---- Meshes ----
        void AddMeshes(const Scene &scene)
        {
            for (const auto &mcMesh : scene.meshes)
            {
                if (mcMesh.positions.empty())
                    continue;

                tinygltf::Mesh gMesh;
                gMesh.name = mcMesh.name;

                // 顶点属性每个 mesh 只写一次，所有 primitive 共享
                MeshAttribs attribs = BuildMeshAttribs(mcMesh);

                if (!mcMesh.sections.empty())
                {
                    for (const auto &sec : mcMesh.sections)
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

                AddSkinAttributesToMesh(mcMesh, gMesh);
                AddMorphTargetsToMesh(mcMesh, gMesh);

                m_meshIdxMap[mcMesh.id] = (int)m_model.meshes.size();
                m_model.meshes.push_back(std::move(gMesh));
            }
        }

        // ---- Nodes ----
        void AddNodes(const Scene &scene)
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

            for (const auto &mcNode : scene.nodes)
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

                const float *m = mcNode.localMatrix.m;
                bool isIdentity = true;
                for (int i = 0; i < 16 && isIdentity; ++i)
                    isIdentity = (std::abs(m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-6f);
                if (!isIdentity)
                {
                    gNode.matrix.resize(16);
                    for (int i = 0; i < 16; ++i)
                        gNode.matrix[i] = m[i];
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

            for (const auto &mcNode : scene.nodes)
            {
                int parentIdx = m_nodeIdxMap[mcNode.id];
                for (ObjectID childId : mcNode.children)
                    if (auto it = m_nodeIdxMap.find(childId); it != m_nodeIdxMap.end())
                        m_model.nodes[parentIdx].children.push_back(it->second);
            }
        }

        // ---- 辅助：列主序矩阵 → TRS（仅动画节点使用）----
        static void DecomposeMatrixToTRS(const float *m, tinygltf::Node &gNode)
        {
            gNode.translation = {static_cast<double>(m[12]),
                                 static_cast<double>(m[13]),
                                 static_cast<double>(m[14])};
            float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
            float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
            gNode.scale = {static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz)};

            float r00 = m[0] / sx, r01 = m[4] / sy, r02 = m[8] / sz;
            float r10 = m[1] / sx, r11 = m[5] / sy, r12 = m[9] / sz;
            float r20 = m[2] / sx, r21 = m[6] / sy, r22 = m[10] / sz;
            float trace = r00 + r11 + r22, qx, qy, qz, qw;
            if (trace > 0.0f)
            {
                float s = std::sqrt(trace + 1.0f) * 2.0f;
                qw = 0.25f * s;
                qx = (r21 - r12) / s;
                qy = (r02 - r20) / s;
                qz = (r10 - r01) / s;
            }
            else if (r00 > r11 && r00 > r22)
            {
                float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
                qw = (r21 - r12) / s;
                qx = 0.25f * s;
                qy = (r01 + r10) / s;
                qz = (r02 + r20) / s;
            }
            else if (r11 > r22)
            {
                float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
                qw = (r02 - r20) / s;
                qx = (r01 + r10) / s;
                qy = 0.25f * s;
                qz = (r12 + r21) / s;
            }
            else
            {
                float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
                qw = (r10 - r01) / s;
                qx = (r02 + r20) / s;
                qy = (r12 + r21) / s;
                qz = 0.25f * s;
            }
            float qLen = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
            if (qLen > 1e-6f)
            {
                qx /= qLen;
                qy /= qLen;
                qz /= qLen;
                qw /= qLen;
            }
            else
            {
                qx = 0.0f;
                qy = 0.0f;
                qz = 0.0f;
                qw = 1.0f;
            }
            gNode.rotation = {static_cast<double>(qx), static_cast<double>(qy),
                              static_cast<double>(qz), static_cast<double>(qw)};
        }

        // 将被动画 channel 引用的节点从 matrix 改为 TRS（Gltf 规范要求）
        void FixAnimatedNodeMatrices()
        {
            std::unordered_set<int> animatedNodes;
            for (const auto &anim : m_model.animations)
                for (const auto &ch : anim.channels)
                    animatedNodes.insert(ch.target_node);
            Logger::Instance().LogInfo(
                "GltfBuilder::FixAnimatedNodeMatrices: " + std::to_string(animatedNodes.size()) +
                " nodes with animation will be converted from matrix to TRS.");
            for (int idx : animatedNodes)
            {
                auto &node = m_model.nodes[idx];
                if (node.matrix.empty())
                    continue;
                std::vector<double> mat = std::move(node.matrix);
                node.matrix.clear();
                float mf[16];
                for (int i = 0; i < 16; ++i)
                    mf[i] = static_cast<float>(mat[i]);
                DecomposeMatrixToTRS(mf, node);
            }
        }

        // ---- Scene ----
        void AddScene(const Scene &scene)
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
        void AddNodeTrsChannels(const AnimationClip &clip, tinygltf::Animation &gAnim,
                                int &tCount, int &rCount, int &sCount)
        {
            for (const auto &nodeAnim : clip.nodeChannels)
            {
                auto nodeIt = m_nodeIdxMap.find(nodeAnim.nodeId);
                if (nodeIt == m_nodeIdxMap.end())
                    continue;
                int targetNodeIdx = nodeIt->second;

                if (!nodeAnim.translation.keys.empty())
                {
                    AddChannel(gAnim, nodeAnim.translation, targetNodeIdx, "translation");
                    ++tCount;
                }
                if (!nodeAnim.rotation.keys.empty())
                {
                    AddChannel(gAnim, nodeAnim.rotation, targetNodeIdx, "rotation");
                    ++rCount;
                }
                if (!nodeAnim.scale.keys.empty())
                {
                    AddChannel(gAnim, nodeAnim.scale, targetNodeIdx, "scale");
                    ++sCount;
                }
            }
        }

        // ---- 将 morphChannels 写入 gAnim（weights 通道，按 meshId 合并）----
        void AddMorphWeightChannels(const AnimationClip &clip, tinygltf::Animation &gAnim,
                                    int &morphChCount)
        {
            if (clip.morphChannels.empty())
                return;

            // 按 meshId 分组
            std::unordered_map<ObjectID, std::vector<const MorphAnimation *>> meshMorphMap;
            for (const auto &ma : clip.morphChannels)
                meshMorphMap[ma.meshId].push_back(&ma);

            for (const auto &[meshId, morphAnims] : meshMorphMap)
            {
                auto nodeIt = m_meshToNodeIdxMap.find(meshId);
                if (nodeIt == m_meshToNodeIdxMap.end())
                    continue;
                int targetNodeIdx = nodeIt->second;

                auto meshIt = m_meshIdxMap.find(meshId);
                if (meshIt == m_meshIdxMap.end())
                    continue;
                int morphCount = (int)m_model.meshes[meshIt->second].weights.size();
                if (morphCount == 0)
                    continue;

                // 各通道 key 时间的并集
                std::set<float> allTimesSet;
                for (const auto *ma : morphAnims)
                    for (const auto &kf : ma->weights.keys)
                        allTimesSet.insert((float)kf.time);

                std::vector<float> times(allTimesSet.begin(), allTimesSet.end());
                if (times.empty())
                    continue;

                AnimationInterpolation interp = morphAnims[0]->weights.interpolation;

                // 每个时刻展开为 [w0, w1, ..., wN]，STEP 评估
                std::vector<float> values;
                values.reserve(times.size() * morphCount);
                for (float t : times)
                {
                    for (int mi = 0; mi < morphCount; ++mi)
                    {
                        float w = 0.0f;
                        for (const auto *ma : morphAnims)
                        {
                            if ((int)ma->morphIndex != mi)
                                continue;
                            for (const auto &kf : ma->weights.keys)
                            {
                                if ((float)kf.time <= t)
                                    w = kf.value;
                                else
                                    break;
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
                sampler.input = inputAcc;
                sampler.output = outputAcc;
                sampler.interpolation = InterpToString(interp);
                int samplerIdx = (int)gAnim.samplers.size();
                gAnim.samplers.push_back(std::move(sampler));

                tinygltf::AnimationChannel channel;
                channel.sampler = samplerIdx;
                channel.target_node = targetNodeIdx;
                channel.target_path = "weights";
                gAnim.channels.push_back(std::move(channel));
                ++morphChCount;
            }
        }

        // ---- Animations —— 编排层（Phase14）----
        void AddAnimations(const Scene &scene)
        {
            for (const auto &clip : scene.animations)
            {
                if (clip.nodeChannels.empty() && clip.morphChannels.empty())
                    continue;

                tinygltf::Animation gAnim;
                gAnim.name = clip.name;

                int tCount = 0, rCount = 0, sCount = 0;
                AddNodeTrsChannels(clip, gAnim, tCount, rCount, sCount);

                int morphChCount = 0;
                AddMorphWeightChannels(clip, gAnim, morphChCount);

                if (!gAnim.channels.empty())
                {
                    int linCount = 0, stepCount = 0, cubicCount = 0;
                    for (const auto &s : gAnim.samplers)
                    {
                        if (s.interpolation == "LINEAR")
                            ++linCount;
                        else if (s.interpolation == "STEP")
                            ++stepCount;
                        else
                            ++cubicCount;
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
        void AddChannel(tinygltf::Animation &gAnim,
                        const TrackVec3 &track,
                        int targetNodeIdx,
                        const std::string &path)
        {
            if (track.keys.empty())
                return;

            bool isCubic = (track.interpolation == AnimationInterpolation::CubicSpline);
            size_t keyCount = track.keys.size();

            // 构建时间戳数组
            std::vector<float> times;
            times.reserve(keyCount);
            for (const auto &kf : track.keys)
                times.push_back(static_cast<float>(kf.time));

            // 构建值数组
            size_t perFrame = isCubic ? 9 : 3; // inTan(xyz) + value(xyz) + outTan(xyz)
            std::vector<float> values;
            values.reserve(keyCount * perFrame);
            for (const auto &kf : track.keys)
            {
                if (isCubic)
                {
                    values.push_back(kf.inTan.x);
                    values.push_back(kf.inTan.y);
                    values.push_back(kf.inTan.z);
                }
                values.push_back(kf.value.x);
                values.push_back(kf.value.y);
                values.push_back(kf.value.z);
                if (isCubic)
                {
                    values.push_back(kf.outTan.x);
                    values.push_back(kf.outTan.y);
                    values.push_back(kf.outTan.z);
                }
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
            sampler.input = inputAcc;
            sampler.output = outputAcc;
            sampler.interpolation = InterpToString(track.interpolation);
            int samplerIdx = (int)gAnim.samplers.size();
            gAnim.samplers.push_back(std::move(sampler));

            tinygltf::AnimationChannel channel;
            channel.sampler = samplerIdx;
            channel.target_node = targetNodeIdx;
            channel.target_path = path;
            gAnim.channels.push_back(std::move(channel));
        }

        // 将 TrackQuat 写入 sampler + channel
        void AddChannel(tinygltf::Animation &gAnim,
                        const TrackQuat &track,
                        int targetNodeIdx,
                        const std::string &path)
        {
            if (track.keys.empty())
                return;

            bool isCubic = (track.interpolation == AnimationInterpolation::CubicSpline);
            size_t keyCount = track.keys.size();

            std::vector<float> times;
            times.reserve(keyCount);
            for (const auto &kf : track.keys)
                times.push_back(static_cast<float>(kf.time));

            size_t perFrame = isCubic ? 12 : 4; // inTan(xyzw) + value(xyzw) + outTan(xyzw)
            std::vector<float> values;
            values.reserve(keyCount * perFrame);
            for (const auto &kf : track.keys)
            {
                if (isCubic)
                {
                    values.push_back(kf.inTan.x);
                    values.push_back(kf.inTan.y);
                    values.push_back(kf.inTan.z);
                    values.push_back(kf.inTan.w);
                }
                values.push_back(kf.value.x);
                values.push_back(kf.value.y);
                values.push_back(kf.value.z);
                values.push_back(kf.value.w);
                if (isCubic)
                {
                    values.push_back(kf.outTan.x);
                    values.push_back(kf.outTan.y);
                    values.push_back(kf.outTan.z);
                    values.push_back(kf.outTan.w);
                }
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
            sampler.input = inputAcc;
            sampler.output = outputAcc;
            sampler.interpolation = InterpToString(track.interpolation);
            int samplerIdx = (int)gAnim.samplers.size();
            gAnim.samplers.push_back(std::move(sampler));

            tinygltf::AnimationChannel channel;
            channel.sampler = samplerIdx;
            channel.target_node = targetNodeIdx;
            channel.target_path = path;
            gAnim.channels.push_back(std::move(channel));
        }

        static const char *InterpToString(AnimationInterpolation interp)
        {
            switch (interp)
            {
            case AnimationInterpolation::Step:
                return "STEP";
            case AnimationInterpolation::CubicSpline:
                return "CUBICSPLINE";
            default:
                return "LINEAR";
            }
        }
    };

    // ============================================================
    // CanExport
    // ============================================================
    bool GltfExporter::CanExport(const std::string &ext) const
    {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower == ".gltf" || lower == ".glb";
    }

    // ============================================================
    // Export
    // ============================================================
    VoidResult GltfExporter::Export(const Scene &scene, ExportContext &ctx)
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

        ctx.meshesExported = scene.MeshCount();
        ctx.materialsExported = scene.MaterialCount();
        ctx.texturesExported = scene.TextureCount();
        ctx.nodesExported = scene.NodeCount();

        Logger::Instance().LogInfo(
            std::string("GltfExporter: exported ") +
            std::to_string(ctx.meshesExported) + " mesh(es), " +
            std::to_string(ctx.materialsExported) + " material(s) -> " + ctx.outputPath);

        return {true, ""};
    }

} // namespace mc
