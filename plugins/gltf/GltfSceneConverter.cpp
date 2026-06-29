#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"

#include "GltfSceneConverter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Texture.h"
#include "mc/core/Node.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace mc {

// ============================================================
// 辅助：从 accessor 读取 float 数组
// ============================================================
static std::vector<float> ReadFloatAccessor(const tinygltf::Model& model, int accessorIdx)
{
    const auto& acc  = model.accessors[accessorIdx];
    const auto& bv   = model.bufferViews[acc.bufferView];
    const auto& buf  = model.buffers[bv.buffer];

    int componentCount = tinygltf::GetNumComponentsInType(acc.type);
    size_t stride = bv.byteStride ? bv.byteStride
                                  : componentCount * tinygltf::GetComponentSizeInBytes(acc.componentType);

    std::vector<float> out;
    out.reserve(acc.count * componentCount);

    const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    for (size_t i = 0; i < acc.count; ++i)
    {
        const uint8_t* ptr = base + i * stride;
        for (int c = 0; c < componentCount; ++c)
        {
            float val = 0.0f;
            if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                std::memcpy(&val, ptr + c * 4, 4);
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                uint16_t u; std::memcpy(&u, ptr + c * 2, 2);
                val = static_cast<float>(u) / 65535.0f;
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            {
                val = static_cast<float>(ptr[c]) / 255.0f;
            }
            out.push_back(val);
        }
    }
    return out;
}

// ============================================================
// 辅助：从 accessor 读取 uint32 索引
// ============================================================
static std::vector<uint32_t> ReadIndexAccessor(const tinygltf::Model& model, int accessorIdx)
{
    const auto& acc = model.accessors[accessorIdx];
    const auto& bv  = model.bufferViews[acc.bufferView];
    const auto& buf = model.buffers[bv.buffer];

    std::vector<uint32_t> out;
    out.reserve(acc.count);

    const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    size_t compSize = tinygltf::GetComponentSizeInBytes(acc.componentType);
    size_t stride   = bv.byteStride ? bv.byteStride : compSize;

    for (size_t i = 0; i < acc.count; ++i)
    {
        const uint8_t* ptr = base + i * stride;
        uint32_t idx = 0;
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
            std::memcpy(&idx, ptr, 4);
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        { uint16_t u; std::memcpy(&u, ptr, 2); idx = u; }
        else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            idx = *ptr;
        out.push_back(idx);
    }
    return out;
}

// ============================================================
// ConvertTextures
// ============================================================
void GltfSceneConverter::ConvertTextures(const tinygltf::Model& model,
                                          const std::string& baseDir,
                                          Scene& mcScene)
{
    m_texIdMap.resize(model.textures.size(), INVALID_ID);

    for (size_t i = 0; i < model.textures.size(); ++i)
    {
        const auto& gTex = model.textures[i];
        Texture& mcTex = mcScene.AddTexture();
        m_texIdMap[i] = mcTex.id;
        mcTex.name = gTex.name;

        if (gTex.source >= 0 && gTex.source < (int)model.images.size())
        {
            const auto& img = model.images[gTex.source];
            if (mcTex.name.empty())
                mcTex.name = img.name;

            if (img.bufferView >= 0 && img.bufferView < (int)model.bufferViews.size())
            {
                const auto& bv = model.bufferViews[img.bufferView];
                if (bv.buffer >= 0 && bv.buffer < (int)model.buffers.size())
                {
                    const auto& buf = model.buffers[bv.buffer];
                    if (bv.byteOffset + bv.byteLength <= buf.data.size())
                    {
                        const auto beginIt = buf.data.begin() + static_cast<std::ptrdiff_t>(bv.byteOffset);
                        const auto endIt   = beginIt + static_cast<std::ptrdiff_t>(bv.byteLength);
                        mcTex.embedded = true;
                        mcTex.embeddedData.assign(beginIt, endIt);
                        mcTex.mimeType = img.mimeType;
                    }
                }
            }

            if (!mcTex.embedded && !img.image.empty())
            {
                mcTex.embedded     = true;
                mcTex.embeddedData = img.image;
                mcTex.mimeType     = img.mimeType;
            }

            if (!mcTex.embedded && !img.uri.empty())
            {
                std::filesystem::path p = std::filesystem::u8path(baseDir) /
                                          std::filesystem::u8path(img.uri);
                mcTex.uri      = p.u8string();
                mcTex.embedded = false;
            }
        }

        // Sampler
        if (gTex.sampler >= 0 && gTex.sampler < (int)model.samplers.size())
        {
            const auto& s = model.samplers[gTex.sampler];
            auto toFilter = [](int gl) -> TextureFilter {
                switch (gl) {
                    case 9728: return TextureFilter::Nearest;
                    case 9729: return TextureFilter::Linear;
                    case 9984: return TextureFilter::NearestMipmapNearest;
                    case 9985: return TextureFilter::LinearMipmapNearest;
                    case 9986: return TextureFilter::NearestMipmapLinear;
                    case 9987: return TextureFilter::LinearMipmapLinear;
                    default:   return TextureFilter::LinearMipmapLinear;
                }
            };
            auto toWrap = [](int gl) -> TextureWrap {
                switch (gl) {
                    case 33071: return TextureWrap::ClampToEdge;
                    case 33648: return TextureWrap::MirroredRepeat;
                    default:    return TextureWrap::Repeat;
                }
            };
            if (s.minFilter >= 0) mcTex.sampler.minFilter = toFilter(s.minFilter);
            if (s.magFilter >= 0) mcTex.sampler.magFilter = toFilter(s.magFilter);
            mcTex.sampler.wrapS = toWrap(s.wrapS);
            mcTex.sampler.wrapT = toWrap(s.wrapT);
        }
    }
}

// ============================================================
// ConvertMaterials
// ============================================================
void GltfSceneConverter::ConvertMaterials(const tinygltf::Model& model, Scene& mcScene)
{
    m_matIdMap.resize(model.materials.size(), INVALID_ID);

    for (size_t i = 0; i < model.materials.size(); ++i)
    {
        const auto& gMat = model.materials[i];
        Material& mcMat = mcScene.AddMaterial();
        m_matIdMap[i] = mcMat.id;

        mcMat.name     = gMat.name;
        mcMat.workflow = Material::MetallicRoughness;

        const auto& pbr = gMat.pbrMetallicRoughness;

        // Base color factor
        if (pbr.baseColorFactor.size() == 4)
        {
            mcMat.baseColor = Vec4(
                (float)pbr.baseColorFactor[0],
                (float)pbr.baseColorFactor[1],
                (float)pbr.baseColorFactor[2],
                (float)pbr.baseColorFactor[3]
            );
        }
        mcMat.metallic  = (float)pbr.metallicFactor;
        mcMat.roughness = (float)pbr.roughnessFactor;

        // Base color texture
        if (pbr.baseColorTexture.index >= 0 &&
            pbr.baseColorTexture.index < (int)m_texIdMap.size())
        {
            mcMat.baseColorTexture.textureId = m_texIdMap[pbr.baseColorTexture.index];
            mcMat.baseColorTexture.uvSet     = pbr.baseColorTexture.texCoord;
        }

        // MetallicRoughness texture
        if (pbr.metallicRoughnessTexture.index >= 0 &&
            pbr.metallicRoughnessTexture.index < (int)m_texIdMap.size())
        {
            mcMat.metallicRoughnessTexture.textureId =
                m_texIdMap[pbr.metallicRoughnessTexture.index];
            mcMat.metallicRoughnessTexture.uvSet = pbr.metallicRoughnessTexture.texCoord;
        }

        // Normal texture
        if (gMat.normalTexture.index >= 0 &&
            gMat.normalTexture.index < (int)m_texIdMap.size())
        {
            mcMat.normalTexture.textureId = m_texIdMap[gMat.normalTexture.index];
            mcMat.normalTexture.uvSet     = gMat.normalTexture.texCoord;
        }

        // Emissive texture
        if (gMat.emissiveTexture.index >= 0 &&
            gMat.emissiveTexture.index < (int)m_texIdMap.size())
        {
            mcMat.emissiveTexture.textureId = m_texIdMap[gMat.emissiveTexture.index];
            mcMat.emissiveTexture.uvSet     = gMat.emissiveTexture.texCoord;
        }

        // Occlusion texture
        if (gMat.occlusionTexture.index >= 0 &&
            gMat.occlusionTexture.index < (int)m_texIdMap.size())
        {
            mcMat.occlusionTexture.textureId = m_texIdMap[gMat.occlusionTexture.index];
            mcMat.occlusionTexture.uvSet     = gMat.occlusionTexture.texCoord;
        }

        // Emissive factor
        if (gMat.emissiveFactor.size() == 3)
            mcMat.emissive = Vec3(
                (float)gMat.emissiveFactor[0],
                (float)gMat.emissiveFactor[1],
                (float)gMat.emissiveFactor[2]
            );

        // Alpha
        if (gMat.alphaMode == "MASK")  mcMat.alphaMode = AlphaMode::Mask;
        else if (gMat.alphaMode == "BLEND") mcMat.alphaMode = AlphaMode::Blend;
        else mcMat.alphaMode = AlphaMode::Opaque;
        mcMat.alphaCutoff = (float)gMat.alphaCutoff;
        mcMat.doubleSided = gMat.doubleSided;
    }
}

// ============================================================
// 辅助：读蒙皮权重
// ============================================================
static void ReadSkinWeights(const tinygltf::Model& model,
                             int jointsAcc, int weightsAcc,
                             uint32_t vertexBase, uint32_t indexCount,
                             Mesh& mcMesh)
{
    // 扩展 skinInfluences 到所有顶点
    size_t newSize = vertexBase + indexCount;
    if (mcMesh.skinInfluences.size() < newSize)
        mcMesh.skinInfluences.resize(newSize);

    // 读取 joints（uint8 或 uint16）
    const auto& jAcc = model.accessors[jointsAcc];
    const auto& jBv  = model.bufferViews[jAcc.bufferView];
    const auto& jBuf = model.buffers[jBv.buffer];
    const uint8_t* jBase = jBuf.data.data() + jBv.byteOffset + jAcc.byteOffset;
    size_t jStride = jBv.byteStride ? jBv.byteStride
                    : tinygltf::GetComponentSizeInBytes(jAcc.componentType) * 4; // VEC4

    // 读取 weights（float VEC4）
    auto wFloats = ReadFloatAccessor(model, weightsAcc);

    uint32_t vCount = (uint32_t)jAcc.count;
    for (uint32_t v = 0; v < vCount; ++v)
    {
        auto& infs = mcMesh.skinInfluences[vertexBase + v];
        infs.clear();
        for (int c = 0; c < 4; ++c)
        {
            // 读取 joint index
            const uint8_t* jPtr = jBase + v * jStride + c * tinygltf::GetComponentSizeInBytes(jAcc.componentType);
            uint16_t joint = 0;
            if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                std::memcpy(&joint, jPtr, 2);
            else if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                joint = *jPtr;

            // 读取 weight
            size_t wIdx = v * 4 + c;
            float weight = (wIdx < wFloats.size()) ? wFloats[wIdx] : 0.0f;

            if (weight > 0.0f)
                infs.push_back({joint, weight});
        }
    }
}

// ============================================================
// ConvertMeshes
// ============================================================
void GltfSceneConverter::ConvertMeshes(const tinygltf::Model& model, Scene& mcScene)
{
    for (const auto& gMesh : model.meshes)
    {
        Mesh& mcMesh = mcScene.AddMesh();
        mcMesh.name = gMesh.name;

        uint32_t indexOffset = 0;

        for (const auto& prim : gMesh.primitives)
        {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES) continue;

            uint32_t vertexBase = (uint32_t)mcMesh.positions.size();

            // Positions
            auto posIt = prim.attributes.find("POSITION");
            if (posIt == prim.attributes.end()) continue;
            {
                auto floats = ReadFloatAccessor(model, posIt->second);
                for (size_t j = 0; j + 2 < floats.size(); j += 3)
                    mcMesh.positions.push_back({floats[j], floats[j+1], floats[j+2]});
            }

            // Normals
            auto nrmIt = prim.attributes.find("NORMAL");
            if (nrmIt != prim.attributes.end())
            {
                auto floats = ReadFloatAccessor(model, nrmIt->second);
                for (size_t j = 0; j + 2 < floats.size(); j += 3)
                    mcMesh.normals.push_back({floats[j], floats[j+1], floats[j+2]});
            }

            // UV TEXCOORD_0
            auto uvIt = prim.attributes.find("TEXCOORD_0");
            if (uvIt != prim.attributes.end())
            {
                auto floats = ReadFloatAccessor(model, uvIt->second);
                if (mcMesh.uvs.empty()) mcMesh.uvs.emplace_back();
                for (size_t j = 0; j + 1 < floats.size(); j += 2)
                    mcMesh.uvs[0].push_back({floats[j], floats[j+1]});
            }

            // Indices
            uint32_t primIndexCount = 0;
            if (prim.indices >= 0)
            {
                auto idxs = ReadIndexAccessor(model, prim.indices);
                for (uint32_t idx : idxs)
                    mcMesh.indices.push_back(vertexBase + idx);
                primIndexCount = (uint32_t)idxs.size();
            }
            else
            {
                // Non-indexed: generate sequential indices
                uint32_t vCount = (uint32_t)(mcMesh.positions.size() - vertexBase);
                for (uint32_t k = 0; k < vCount; ++k)
                    mcMesh.indices.push_back(vertexBase + k);
                primIndexCount = vCount;
            }

            // Skin weights（JOINTS_0 + WEIGHTS_0）
            auto jtIt = prim.attributes.find("JOINTS_0");
            auto wtIt = prim.attributes.find("WEIGHTS_0");
            if (jtIt != prim.attributes.end() && wtIt != prim.attributes.end())
            {
                ReadSkinWeights(model, jtIt->second, wtIt->second,
                                vertexBase, primIndexCount, mcMesh);
            }

            // Section
            MeshSection sec;
            sec.indexOffset = indexOffset;
            sec.indexCount  = primIndexCount;
            if (prim.material >= 0 && prim.material < (int)m_matIdMap.size())
                sec.materialId = m_matIdMap[prim.material];
            mcMesh.sections.push_back(sec);

            indexOffset += primIndexCount;
        }
    }
}

// ============================================================
// ConvertNode（DFS）
// ============================================================
void GltfSceneConverter::ConvertNode(const tinygltf::Model& model,
                                      int nodeIdx,
                                      Scene& mcScene,
                                      mc::ObjectID parentId,
                                      const std::vector<mc::ObjectID>& meshIdMap)
{
    const auto& gNode = model.nodes[nodeIdx];

    mc::ObjectID nodeId = mcScene.AddNode().id;
    mc::Node* mcNode = mcScene.FindNode(nodeId);
    mcNode->name = gNode.name;

    // 记录 gltf node index -> mc ObjectID 映射，供动画转换使用
    if ((size_t)nodeIdx >= m_nodeIdMap.size())
        m_nodeIdMap.resize(nodeIdx + 1, INVALID_ID);
    m_nodeIdMap[nodeIdx] = nodeId;

    // Transform: matrix 优先，否则 TRS
    if (!gNode.matrix.empty())
    {
        const auto& m = gNode.matrix;
        mcNode->localMatrix = Matrix4(
            (float)m[0],  (float)m[1],  (float)m[2],  (float)m[3],
            (float)m[4],  (float)m[5],  (float)m[6],  (float)m[7],
            (float)m[8],  (float)m[9],  (float)m[10], (float)m[11],
            (float)m[12], (float)m[13], (float)m[14], (float)m[15]
        );
    }
    else
    {
        // 从 TRS 属性构建矩阵（GLTF 列主序）
        float tx = gNode.translation.size() >= 3 ? (float)gNode.translation[0] : 0.0f;
        float ty = gNode.translation.size() >= 3 ? (float)gNode.translation[1] : 0.0f;
        float tz = gNode.translation.size() >= 3 ? (float)gNode.translation[2] : 0.0f;

        float sx = gNode.scale.size() >= 3 ? (float)gNode.scale[0] : 1.0f;
        float sy = gNode.scale.size() >= 3 ? (float)gNode.scale[1] : 1.0f;
        float sz = gNode.scale.size() >= 3 ? (float)gNode.scale[2] : 1.0f;

        // Quaternion → 旋转矩阵（列主序）
        float qx = gNode.rotation.size() >= 4 ? (float)gNode.rotation[0] : 0.0f;
        float qy = gNode.rotation.size() >= 4 ? (float)gNode.rotation[1] : 0.0f;
        float qz = gNode.rotation.size() >= 4 ? (float)gNode.rotation[2] : 0.0f;
        float qw = gNode.rotation.size() >= 4 ? (float)gNode.rotation[3] : 1.0f;

        float xx = qx * qx, yy = qy * qy, zz = qz * qz;
        float xy = qx * qy, xz = qx * qz, yz = qy * qz;
        float wx = qw * qx, wy = qw * qy, wz = qw * qz;

        // 旋转矩阵列 (column-major) → 乘以 scale
        float r00 = (1.0f - 2.0f * (yy + zz)) * sx;
        float r10 = (2.0f * (xy + wz)) * sx;
        float r20 = (2.0f * (xz - wy)) * sx;

        float r01 = (2.0f * (xy - wz)) * sy;
        float r11 = (1.0f - 2.0f * (xx + zz)) * sy;
        float r21 = (2.0f * (yz + wx)) * sy;

        float r02 = (2.0f * (xz + wy)) * sz;
        float r12 = (2.0f * (yz - wx)) * sz;
        float r22 = (1.0f - 2.0f * (xx + yy)) * sz;

        mcNode->localMatrix = Matrix4(
            r00, r10, r20, 0.0f,
            r01, r11, r21, 0.0f,
            r02, r12, r22, 0.0f,
            tx,  ty,  tz,  1.0f
        );
    }

    // Mesh 引用
    if (gNode.mesh >= 0 && gNode.mesh < (int)meshIdMap.size())
        mcScene.FindNode(nodeId)->meshIds.push_back(meshIdMap[gNode.mesh]);

    // 挂载
    if (parentId != INVALID_ID)
    {
        mc::Node* parent = mcScene.FindNode(parentId);
        if (parent) parent->children.push_back(nodeId);
    }
    else
    {
        mcScene.rootNodes.push_back(nodeId);
    }

    for (int child : gNode.children)
        ConvertNode(model, child, mcScene, nodeId, meshIdMap);
}

// ============================================================
// ConvertSkins（Phase15）
// ============================================================
void GltfSceneConverter::ConvertSkins(const tinygltf::Model& model, Scene& mcScene)
{
    if (model.skins.empty()) return;

    // 第一步：创建 Skeleton
    std::vector<ObjectID> skelIdMap(model.skins.size(), INVALID_ID);
    for (size_t skinIdx = 0; skinIdx < model.skins.size(); ++skinIdx)
    {
        const auto& gSkin = model.skins[skinIdx];
        if (gSkin.joints.empty()) continue;

        Skeleton skel;
        skel.id   = mcScene.AllocateId();
        skel.name = gSkin.name.empty()
                    ? "Skeleton_" + std::to_string(skinIdx) : gSkin.name;

        std::vector<float> ibmFloats;
        if (gSkin.inverseBindMatrices >= 0)
            ibmFloats = ReadFloatAccessor(model, gSkin.inverseBindMatrices);

        for (size_t j = 0; j < gSkin.joints.size(); ++j)
        {
            int gNodeIdx = gSkin.joints[j];
            if (gNodeIdx < 0 || (size_t)gNodeIdx >= m_nodeIdMap.size()) continue;
            ObjectID nodeId = m_nodeIdMap[gNodeIdx];
            if (nodeId == INVALID_ID) continue;

            // 标记为骨骼节点
            Node* nd = mcScene.FindNode(nodeId);
            if (nd) nd->type = NodeType::Bone;

            Bone bone;
            bone.id   = mcScene.AllocateId();
            bone.name = nd ? nd->name : "";

            size_t base = j * 16;
            if (base + 15 < ibmFloats.size())
            {
                auto& f = ibmFloats;
                bone.inverseBindPose = Matrix4(
                    f[base+0],f[base+1],f[base+2],f[base+3],
                    f[base+4],f[base+5],f[base+6],f[base+7],
                    f[base+8],f[base+9],f[base+10],f[base+11],
                    f[base+12],f[base+13],f[base+14],f[base+15]);
            }
            skel.bones.push_back(std::move(bone));
        }

        skelIdMap[skinIdx] = skel.id;
        mcScene.skeletons.push_back(std::move(skel));
    }

    // 第二步：创建 Skin，关联 mesh
    for (const auto& gNode : model.nodes)
    {
        if (gNode.skin < 0 || gNode.mesh < 0) continue;
        size_t skinIdx = (size_t)gNode.skin;
        if (skinIdx >= skelIdMap.size() || skelIdMap[skinIdx] == INVALID_ID) continue;

        // 找 mc mesh ObjectID
        ObjectID mcMeshId = INVALID_ID;
        if ((size_t)gNode.mesh < model.meshes.size())
        {
            const auto& gMesh = model.meshes[gNode.mesh];
            for (const auto& mcMesh : mcScene.meshes)
            {
                if (mcMesh.name == gMesh.name) { mcMeshId = mcMesh.id; break; }
            }
        }
        if (mcMeshId == INVALID_ID) continue;

        Skin skin;
        skin.id         = mcScene.AllocateId();
        skin.skeletonId = skelIdMap[skinIdx];
        skin.meshId     = mcMeshId;
        mcScene.skins.push_back(std::move(skin));
    }

    Logger::Instance().LogInfo(
        "GltfSceneConverter: converted " + std::to_string(mcScene.skeletons.size()) +
        " skeleton(s), " + std::to_string(mcScene.skins.size()) + " skin(s).");
}

// ============================================================
// ApplySkinWeights（空实现，权重已在 ConvertMeshes 读取）
// ============================================================
void GltfSceneConverter::ApplySkinWeights(Scene& /*mcScene*/) {}

// ============================================================
// ConvertAnimations（Phase14）
// ============================================================
void GltfSceneConverter::ConvertAnimations(const tinygltf::Model& model, Scene& mcScene)
{
    for (const auto& gAnim : model.animations)
    {
        AnimationClip clip;
        clip.id   = mcScene.AllocateId();
        clip.name = gAnim.name;

        // 第一步：收集每个 sampler 的时间戳，用于确定 clip 的 startTime/endTime
        double animStart = std::numeric_limits<double>::max();
        double animEnd   = std::numeric_limits<double>::lowest();

        for (const auto& gSampler : gAnim.samplers)
        {
            auto times = ReadFloatAccessor(model, gSampler.input);
            if (!times.empty())
            {
                animStart = std::min(animStart, static_cast<double>(times.front()));
                animEnd   = std::max(animEnd,   static_cast<double>(times.back()));
            }
        }

        if (animStart >= animEnd)
        {
            Logger::Instance().LogWarn(
                "GltfSceneConverter: animation \"" + gAnim.name + "\" has no valid keyframes, skipped.");
            continue;
        }

        clip.startTime = animStart;
        clip.endTime   = animEnd;

        // 第二步：遍历 channels，构建 NodeAnimation / MorphAnimation
        // 按 (nodeId, path) 去重：同一个节点+路径可能被多个 channel 引用
        std::unordered_map<ObjectID, NodeAnimation>   nodeAnimMap;
        // Morph key: (meshId << 32) | morphIndex，确保同一 mesh 的不同 morph target 不互相覆盖
        std::unordered_map<uint64_t, MorphAnimation>  morphAnimMap;

        for (const auto& gChannel : gAnim.channels)
        {
            int samplerIdx = gChannel.sampler;
            int targetNode = gChannel.target_node;
            const std::string& path = gChannel.target_path;

            if (samplerIdx < 0 || samplerIdx >= (int)gAnim.samplers.size())
                continue;
            if (targetNode < 0 || targetNode >= (int)m_nodeIdMap.size())
                continue;

            ObjectID mcNodeId = m_nodeIdMap[targetNode];
            if (mcNodeId == INVALID_ID)
                continue;

            const auto& gSampler = gAnim.samplers[samplerIdx];

            // 解析插值模式
            AnimationInterpolation interp = AnimationInterpolation::Linear;
            if (gSampler.interpolation == "STEP")
                interp = AnimationInterpolation::Step;
            else if (gSampler.interpolation == "CUBICSPLINE")
                interp = AnimationInterpolation::CubicSpline;
            else
                interp = AnimationInterpolation::Linear;

            // 读取时间戳
            auto times = ReadFloatAccessor(model, gSampler.input);

            if (path == "translation" || path == "rotation" || path == "scale")
            {
                // ---- NodeAnimation TRS 通道 ----
                auto values = ReadFloatAccessor(model, gSampler.output);

                auto& nodeAnim = nodeAnimMap[mcNodeId];
                nodeAnim.nodeId = mcNodeId;

                if (path == "translation")
                {
                    TrackVec3& track = nodeAnim.translation;
                    track.interpolation = interp;

                    if (interp == AnimationInterpolation::CubicSpline)
                    {
                        // CUBICSPLINE: 每关键帧 3 个 vec3（inTan, value, outTan）= 9 floats
                        size_t frameCount = times.size();
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            size_t base = f * 9;
                            if (base + 8 >= values.size()) break;
                            KeyFrame<Vec3> kf;
                            kf.time   = times[f];
                            kf.inTan  = Vec3(values[base+0], values[base+1], values[base+2]);
                            kf.value  = Vec3(values[base+3], values[base+4], values[base+5]);
                            kf.outTan = Vec3(values[base+6], values[base+7], values[base+8]);
                            track.keys.push_back(kf);
                        }
                    }
                    else
                    {
                        // LINEAR / STEP: 每关键帧 1 个 vec3 = 3 floats
                        size_t frameCount = std::min(times.size(), values.size() / 3);
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            KeyFrame<Vec3> kf;
                            kf.time  = times[f];
                            kf.value = Vec3(values[f*3+0], values[f*3+1], values[f*3+2]);
                            track.keys.push_back(kf);
                        }
                    }
                }
                else if (path == "rotation")
                {
                    TrackQuat& track = nodeAnim.rotation;
                    track.interpolation = interp;

                    if (interp == AnimationInterpolation::CubicSpline)
                    {
                        // CUBICSPLINE: 每关键帧 3 个 quat（inTan, value, outTan）= 12 floats
                        size_t frameCount = times.size();
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            size_t base = f * 12;
                            if (base + 11 >= values.size()) break;
                            KeyFrame<Quaternion> kf;
                            kf.time   = times[f];
                            kf.inTan  = Quaternion(values[base+0], values[base+1], values[base+2], values[base+3]);
                            kf.value  = Quaternion(values[base+4], values[base+5], values[base+6], values[base+7]);
                            kf.outTan = Quaternion(values[base+8], values[base+9], values[base+10], values[base+11]);
                            track.keys.push_back(kf);
                        }
                    }
                    else
                    {
                        // LINEAR / STEP: 每关键帧 1 个 quat = 4 floats
                        size_t frameCount = std::min(times.size(), values.size() / 4);
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            KeyFrame<Quaternion> kf;
                            kf.time  = times[f];
                            kf.value = Quaternion(values[f*4+0], values[f*4+1], values[f*4+2], values[f*4+3]);
                            track.keys.push_back(kf);
                        }
                    }
                }
                else if (path == "scale")
                {
                    TrackVec3& track = nodeAnim.scale;
                    track.interpolation = interp;

                    if (interp == AnimationInterpolation::CubicSpline)
                    {
                        size_t frameCount = times.size();
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            size_t base = f * 9;
                            if (base + 8 >= values.size()) break;
                            KeyFrame<Vec3> kf;
                            kf.time   = times[f];
                            kf.inTan  = Vec3(values[base+0], values[base+1], values[base+2]);
                            kf.value  = Vec3(values[base+3], values[base+4], values[base+5]);
                            kf.outTan = Vec3(values[base+6], values[base+7], values[base+8]);
                            track.keys.push_back(kf);
                        }
                    }
                    else
                    {
                        size_t frameCount = std::min(times.size(), values.size() / 3);
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            KeyFrame<Vec3> kf;
                            kf.time  = times[f];
                            kf.value = Vec3(values[f*3+0], values[f*3+1], values[f*3+2]);
                            track.keys.push_back(kf);
                        }
                    }
                }
            }
            else if (path == "weights")
            {
                // ---- MorphAnimation 权重通道 ----
                auto values = ReadFloatAccessor(model, gSampler.output);

                // 获取该节点引用的 mesh
                if (targetNode < 0 || targetNode >= (int)model.nodes.size())
                    continue;

                const auto& gNode = model.nodes[targetNode];
                if (gNode.mesh < 0)
                    continue;

                // 遍历该 mesh 的所有 morph target，为每个创建一个 MorphAnimation 通道
                const auto& gMesh = model.meshes[gNode.mesh];
                size_t morphCount = gMesh.weights.size();
                if (morphCount == 0) continue;

                // 获取 mc mesh ID（通过 node 的 meshIds），提到循环外避免重复查找
                const Node* mcNode = mcScene.FindNode(mcNodeId);
                if (!mcNode || mcNode->meshIds.empty()) continue;
                ObjectID mcMeshId = mcNode->meshIds[0];  // GLTF 节点只有一个 mesh

                for (size_t mi = 0; mi < morphCount; ++mi)
                {
                    // 复合 key: (meshId, morphIndex)
                    uint64_t morphKey = (static_cast<uint64_t>(mcMeshId) << 32) | static_cast<uint64_t>(mi);

                    MorphAnimation& morphAnim = morphAnimMap[morphKey];
                    morphAnim.meshId     = mcMeshId;
                    morphAnim.morphIndex = static_cast<uint32_t>(mi);
                    TrackFloat& track    = morphAnim.weights;
                    track.interpolation  = interp;

                    if (interp == AnimationInterpolation::CubicSpline)
                    {
                        // CUBICSPLINE: 每关键帧 3 个 float（inTan, value, outTan）
                        size_t frameCount = times.size();
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            size_t base = f * morphCount * 3 + mi * 3;
                            if (base + 2 >= values.size()) break;
                            KeyFrame<float> kf;
                            kf.time   = times[f];
                            kf.inTan  = values[base + 0];
                            kf.value  = values[base + 1];
                            kf.outTan = values[base + 2];
                            track.keys.push_back(kf);
                        }
                    }
                    else
                    {
                        // LINEAR / STEP: 每关键帧 1 个 float
                        size_t frameCount = times.size();
                        for (size_t f = 0; f < frameCount; ++f)
                        {
                            size_t idx = f * morphCount + mi;
                            if (idx >= values.size()) break;
                            KeyFrame<float> kf;
                            kf.time  = times[f];
                            kf.value = values[idx];
                            track.keys.push_back(kf);
                        }
                    }
                }
            }
            else if (path == "visibility")
            {
                // GLTF 没有原生 visibility 通道，保留扩展点
                Logger::Instance().LogWarn(
                    "GltfSceneConverter: unsupported animation path \"" + path + "\"");
            }
        }

        // 第三步：将所有 node/morph 通道写入 clip
        for (auto& [nodeId, nodeAnim] : nodeAnimMap)
            clip.nodeChannels.push_back(std::move(nodeAnim));

        for (auto& [key, morphAnim] : morphAnimMap)
            clip.morphChannels.push_back(std::move(morphAnim));

        if (!clip.nodeChannels.empty() || !clip.morphChannels.empty())
            mcScene.animations.push_back(std::move(clip));
    }

    if (!model.animations.empty())
    {
        Logger::Instance().LogInfo(
            "GltfSceneConverter: converted " + std::to_string(mcScene.animations.size()) +
            " animation clip(s)."
        );
    }
}

// ============================================================
// Convert（主入口）
// ============================================================
VoidResult GltfSceneConverter::Convert(const tinygltf::Model& model,
                                        const std::string& baseDir,
                                        Scene& mcScene)
{
    VoidResult result;
    result.ok = true;

    // GLTF 默认：米、Y-Up、右手系
    mcScene.metadata.unit = "m";
    mcScene.metadata.unitScale = 1.0f;
    mcScene.metadata.upAxis = Axis::Y;
    mcScene.metadata.frontAxis = Axis::Z;
    mcScene.metadata.handedness = Handedness::Right;

    if (!model.asset.generator.empty())
        mcScene.metadata.asset.generator = model.asset.generator;
    if (!model.asset.copyright.empty())
        mcScene.metadata.asset.copyright = model.asset.copyright;
    if (!model.asset.version.empty())
        mcScene.metadata.custom["assetVersion"] = model.asset.version;
    if (mcScene.metadata.asset.sourceFormat.empty())
        mcScene.metadata.asset.sourceFormat = "gltf";

    ConvertTextures(model, baseDir, mcScene);
    ConvertMaterials(model, mcScene);

    size_t meshCountBefore = mcScene.meshes.size();
    ConvertMeshes(model, mcScene);

    std::vector<mc::ObjectID> meshIdMap;
    meshIdMap.reserve(model.meshes.size());
    for (size_t i = meshCountBefore; i < mcScene.meshes.size(); ++i)
        meshIdMap.push_back(mcScene.meshes[i].id);

    const auto& defaultScene = model.scenes.empty() ? tinygltf::Scene{}
                                                     : model.scenes[model.defaultScene >= 0
                                                                     ? model.defaultScene : 0];
    for (int rootIdx : defaultScene.nodes)
        ConvertNode(model, rootIdx, mcScene, INVALID_ID, meshIdMap);

    // Phase15: 骨骼蒙皮（须在节点树构建完成之后）
    ConvertSkins(model, mcScene);
    ApplySkinWeights(mcScene);

    // Phase14: 动画转换
    ConvertAnimations(model, mcScene);

    Logger::Instance().LogInfo(
        std::string("GltfSceneConverter: converted ") +
        std::to_string(model.meshes.size()) + " mesh(es), " +
        std::to_string(model.materials.size()) + " material(s), " +
        std::to_string(model.textures.size()) + " texture(s)."
    );

    return result;
}

} // namespace mc
