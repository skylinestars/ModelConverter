#pragma once

#include "mc/common/Types.h"
#include "mc/core/SceneMetadata.h"
#include "mc/core/Node.h"
#include "mc/core/Mesh.h"
#include "mc/core/Texture.h"
#include "mc/core/Material.h"
#include "mc/core/Skeleton.h"
#include "mc/core/Skin.h"
#include "mc/core/Animation.h"
#include "mc/core/Camera.h"
#include "mc/core/Light.h"
#include "mc/core/PointInstancer.h"

#include <vector>
#include <unordered_map>
#include <cassert>

namespace mc {

// ============================================================
// Scene
// ============================================================
class Scene
{
public:
    Scene() = default;
    ~Scene() = default;

    // Non-copyable, movable
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = default;
    Scene& operator=(Scene&&) = default;

    // ---- ID allocation ----
    ObjectID AllocateId()
    {
        return m_nextId++;
    }

    // ---- Node helpers ----
    Node* FindNode(ObjectID id)
    {
        auto it = m_nodeIndex.find(id);
        if (it != m_nodeIndex.end()) return &nodes[it->second];
        return nullptr;
    }

    const Node* FindNode(ObjectID id) const
    {
        auto it = m_nodeIndex.find(id);
        if (it != m_nodeIndex.end()) return &nodes[it->second];
        return nullptr;
    }

    Node& AddNode()
    {
        Node node;
        node.id = AllocateId();
        m_nodeIndex[node.id] = nodes.size();
        nodes.push_back(std::move(node));
        return nodes.back();
    }

    void RemoveNode(ObjectID id)
    {
        auto it = m_nodeIndex.find(id);
        if (it != m_nodeIndex.end())
        {
            m_nodeIndex.erase(it);
            // Null out the node (keep vector index stable for simplicity)
            // In production, you'd swap+pop and update indices
        }
    }

    // ---- Mesh helpers ----
    Mesh* FindMesh(ObjectID id)
    {
        auto it = m_meshIndex.find(id);
        if (it != m_meshIndex.end()) return &meshes[it->second];
        return nullptr;
    }

    Mesh& AddMesh()
    {
        Mesh mesh;
        mesh.id = AllocateId();
        m_meshIndex[mesh.id] = meshes.size();
        meshes.push_back(std::move(mesh));
        return meshes.back();
    }

    // ---- Material helpers ----
    Material* FindMaterial(ObjectID id)
    {
        auto it = m_materialIndex.find(id);
        if (it != m_materialIndex.end()) return &materials[it->second];
        return nullptr;
    }

    Material& AddMaterial()
    {
        Material mat;
        mat.id = AllocateId();
        m_materialIndex[mat.id] = materials.size();
        materials.push_back(std::move(mat));
        return materials.back();
    }

    // ---- Texture helpers ----
    Texture* FindTexture(ObjectID id)
    {
        auto it = m_textureIndex.find(id);
        if (it != m_textureIndex.end()) return &textures[it->second];
        return nullptr;
    }

    Texture& AddTexture()
    {
        Texture tex;
        tex.id = AllocateId();
        m_textureIndex[tex.id] = textures.size();
        textures.push_back(std::move(tex));
        return textures.back();
    }

    // ---- Count helpers ----
    size_t NodeCount() const     { return nodes.size(); }
    size_t MeshCount() const     { return meshes.size(); }
    size_t MaterialCount() const { return materials.size(); }
    size_t TextureCount() const  { return textures.size(); }

    // ---- Data ----
    SceneMetadata                        metadata;
    
    // SceneGraph（森林）
    std::vector<ObjectID>               rootNodes;    // 多个根节点
    std::vector<Node>                    nodes;       // 所有节点平铺存储
    
    // Geometry（与 SceneGraph 分离，Node 只存 ObjectID 引用）
    std::vector<Mesh>                    meshes;
    
    // Material / Texture
    std::vector<Material>                materials;
    std::vector<Texture>                 textures;

    // Skeleton / Skin（三分离：Node ≠ Bone ≠ Skin）
    std::vector<Skeleton>               skeletons;
    std::vector<Skin>                    skins;

    // Animation
    std::vector<AnimationClip>           animations;
    
    // 实例化（USD PointInstancer / FBX Instance）
    std::vector<PointInstancer>         pointInstancers;
    
    // 其他
    std::vector<Camera>                  cameras;
    std::vector<Light>                   lights;

private:
    ObjectID m_nextId = 1;

    std::unordered_map<ObjectID, size_t> m_nodeIndex;
    std::unordered_map<ObjectID, size_t> m_meshIndex;
    std::unordered_map<ObjectID, size_t> m_materialIndex;
    std::unordered_map<ObjectID, size_t> m_textureIndex;
};

} // namespace mc