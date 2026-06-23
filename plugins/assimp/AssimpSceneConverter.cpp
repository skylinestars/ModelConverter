#include "AssimpSceneConverter.h"
#include "mc/core/Mesh.h"
#include "mc/core/Material.h"
#include "mc/core/Node.h"
#include "mc/common/Logger.h"

#include <assimp/scene.h>
#include <assimp/mesh.h>
#include <assimp/material.h>
#include <assimp/metadata.h>
#include <assimp/commonMetaData.h>

#include <sstream>
#include <cmath>
#include <string>

namespace mc {

static Axis ToAxisFromIndex(int axis)
{
    switch (axis)
    {
        case 0: return Axis::X;
        case 1: return Axis::Y;
        case 2: return Axis::Z;
        default: return Axis::Y;
    }
}

static std::string UnitNameFromScale(double scaleToCm)
{
    if (std::abs(scaleToCm - 100.0) < 1e-6) return "m";
    if (std::abs(scaleToCm - 1.0) < 1e-6) return "cm";
    if (std::abs(scaleToCm - 0.1) < 1e-6) return "mm";
    if (std::abs(scaleToCm - 2.54) < 1e-6) return "inch";
    return "custom";
}

static void FillMetadataFromAssimp(const aiScene* aiSrc, Scene& mcScene)
{
    auto& meta = mcScene.metadata;
    const aiMetadata* md = aiSrc->mMetaData;
    if (!md) return;

    float scaleToCm = 0.0f;
    if (md->Get("UnitScaleFactor", scaleToCm))
    {
        meta.unitScale = scaleToCm * 0.01f; // cm -> m
        meta.unit = UnitNameFromScale(scaleToCm);
    }

    int upAxis = -1;
    if (md->Get("UpAxis", upAxis))
        meta.upAxis = ToAxisFromIndex(upAxis);

    int frontAxis = -1;
    if (md->Get("FrontAxis", frontAxis))
        meta.frontAxis = ToAxisFromIndex(frontAxis);

    int coordAxisSign = 0;
    if (md->Get("CoordAxisSign", coordAxisSign))
        meta.handedness = coordAxisSign < 0 ? Handedness::Left : Handedness::Right;

    int upAxisSign = 0;
    if (md->Get("UpAxisSign", upAxisSign))
        meta.custom["upAxisSign"] = std::to_string(upAxisSign);

    int frontAxisSign = 0;
    if (md->Get("FrontAxisSign", frontAxisSign))
        meta.custom["frontAxisSign"] = std::to_string(frontAxisSign);

    aiString value;
    if (md->Get(AI_METADATA_SOURCE_GENERATOR, value))
        meta.asset.generator = value.C_Str();
    if (md->Get(AI_METADATA_SOURCE_FORMAT, value) && meta.asset.sourceFormat.empty())
        meta.asset.sourceFormat = value.C_Str();
    if (md->Get(AI_METADATA_SOURCE_COPYRIGHT, value))
        meta.asset.copyright = value.C_Str();
}

// ============================================================
// ConvertMaterials
// ============================================================
void AssimpSceneConverter::ConvertMaterials(const aiScene* aiSrc, Scene& mcScene)
{
    m_matIdMap.resize(aiSrc->mNumMaterials, INVALID_ID);

    for (unsigned int i = 0; i < aiSrc->mNumMaterials; ++i)
    {
        const aiMaterial* aiMat = aiSrc->mMaterials[i];
        Material& mcMat = mcScene.AddMaterial();
        m_matIdMap[i] = mcMat.id;

        aiColor4D color;
        if (AI_SUCCESS == aiMat->Get(AI_MATKEY_COLOR_DIFFUSE, color))
        {
            mcMat.baseColor = Vec4(color.r, color.g, color.b, color.a);
        }

        float opacity = 1.0f;
        if (AI_SUCCESS == aiMat->Get(AI_MATKEY_OPACITY, opacity))
        {
            mcMat.opacity = opacity;
        }

        aiString matName;
        if (AI_SUCCESS == aiMat->Get(AI_MATKEY_NAME, matName))
        {
            mcMat.name = matName.C_Str();
        }

        mcMat.workflow = Material::Phong;
    }
}

// ============================================================
// ConvertMeshes
// ============================================================
void AssimpSceneConverter::ConvertMeshes(const aiScene* aiSrc, Scene& mcScene)
{
    for (unsigned int i = 0; i < aiSrc->mNumMeshes; ++i)
    {
        const aiMesh* aiM = aiSrc->mMeshes[i];
        Mesh& mcMesh = mcScene.AddMesh();
        mcMesh.name = aiM->mName.C_Str();

        // Positions
        mcMesh.positions.reserve(aiM->mNumVertices);
        for (unsigned int v = 0; v < aiM->mNumVertices; ++v)
        {
            const auto& p = aiM->mVertices[v];
            mcMesh.positions.push_back({p.x, p.y, p.z});
        }

        // Normals
        if (aiM->HasNormals())
        {
            mcMesh.normals.reserve(aiM->mNumVertices);
            for (unsigned int v = 0; v < aiM->mNumVertices; ++v)
            {
                const auto& n = aiM->mNormals[v];
                mcMesh.normals.push_back({n.x, n.y, n.z});
            }
        }

        // UV channel 0
        if (aiM->HasTextureCoords(0))
        {
            std::vector<Vec2> uv0;
            uv0.reserve(aiM->mNumVertices);
            for (unsigned int v = 0; v < aiM->mNumVertices; ++v)
            {
                const auto& tc = aiM->mTextureCoords[0][v];
                uv0.push_back({tc.x, tc.y});
            }
            mcMesh.uvs.push_back(std::move(uv0));
        }

        // Indices（只处理三角形面）
        mcMesh.indices.reserve(aiM->mNumFaces * 3);
        for (unsigned int f = 0; f < aiM->mNumFaces; ++f)
        {
            const aiFace& face = aiM->mFaces[f];
            if (face.mNumIndices == 3)
            {
                mcMesh.indices.push_back(face.mIndices[0]);
                mcMesh.indices.push_back(face.mIndices[1]);
                mcMesh.indices.push_back(face.mIndices[2]);
            }
        }

        // Section（单 material）
        MeshSection sec;
        sec.indexOffset = 0;
        sec.indexCount  = static_cast<uint32_t>(mcMesh.indices.size());
        if (aiM->mMaterialIndex < m_matIdMap.size())
            sec.materialId = m_matIdMap[aiM->mMaterialIndex];
        mcMesh.sections.push_back(sec);
    }
}

// ============================================================
// ConvertNode（DFS 递归）
// ============================================================
void AssimpSceneConverter::ConvertNode(const aiNode* aiNode, Scene& mcScene,
                                        mc::ObjectID parentId,
                                        const std::vector<mc::ObjectID>& meshIdMap)
{
    mc::ObjectID nodeId = mcScene.AddNode().id;
    mc::Node* mcNode = mcScene.FindNode(nodeId);
    mcNode->name = aiNode->mName.C_Str();

    // 局部变换（行主序转列主序）
    const aiMatrix4x4& m = aiNode->mTransformation;
    mcNode->localMatrix = Matrix4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4
    );

    // Mesh 引用
    for (unsigned int i = 0; i < aiNode->mNumMeshes; ++i)
    {
        unsigned int meshIdx = aiNode->mMeshes[i];
        if (meshIdx < meshIdMap.size())
            mcScene.FindNode(nodeId)->meshIds.push_back(meshIdMap[meshIdx]);
    }

    // 挂载到父节点
    if (parentId != INVALID_ID)
    {
        mc::Node* parent = mcScene.FindNode(parentId);
        if (parent)
            parent->children.push_back(nodeId);
    }
    else
    {
        mcScene.rootNodes.push_back(nodeId);
    }

    // 递归子节点
    for (unsigned int i = 0; i < aiNode->mNumChildren; ++i)
        ConvertNode(aiNode->mChildren[i], mcScene, nodeId, meshIdMap);
}

// ============================================================
// Convert（主入口）
// ============================================================
VoidResult AssimpSceneConverter::Convert(const aiScene* aiSrc, Scene& mcScene)
{
    VoidResult result;
    result.ok = true;

    if (!aiSrc)
    {
        result.ok    = false;
        result.error = "AssimpSceneConverter: aiScene is null";
        return result;
    }

    FillMetadataFromAssimp(aiSrc, mcScene);

    ConvertMaterials(aiSrc, mcScene);

    // 先收集 mesh ID 映射（ConvertMeshes 之后）
    size_t meshCountBefore = mcScene.meshes.size();
    ConvertMeshes(aiSrc, mcScene);

    // 构建 assimp mesh index -> mc ObjectID 的映射
    std::vector<mc::ObjectID> meshIdMap;
    meshIdMap.reserve(aiSrc->mNumMeshes);
    for (size_t i = meshCountBefore; i < mcScene.meshes.size(); ++i)
        meshIdMap.push_back(mcScene.meshes[i].id);

    // 递归转换节点树
    if (aiSrc->mRootNode)
        ConvertNode(aiSrc->mRootNode, mcScene, INVALID_ID, meshIdMap);

    Logger::Instance().LogInfo(
        std::string("AssimpSceneConverter: converted ") +
        std::to_string(aiSrc->mNumMeshes) + " mesh(es), " +
        std::to_string(aiSrc->mNumMaterials) + " material(s)."
    );

    return result;
}

} // namespace mc
