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
#include "mc/common/Logger.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <sstream>

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
                mcTex.uri      = p;
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

    // Transform: TRS 或 matrix
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
    // else: TRS 留给后续 Phase 处理，此 Phase 先用单位矩阵

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

    Logger::Instance().LogInfo(
        std::string("GltfSceneConverter: converted ") +
        std::to_string(model.meshes.size()) + " mesh(es), " +
        std::to_string(model.materials.size()) + " material(s), " +
        std::to_string(model.textures.size()) + " texture(s)."
    );

    return result;
}

} // namespace mc
