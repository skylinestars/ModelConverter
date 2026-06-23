#include "mc/pipeline/ValidatePass.h"
#include "mc/core/Scene.h"
#include "mc/core/Material.h"

#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <sstream>

namespace mc {

// ============================================================
// 内部辅助
// ============================================================

namespace {

// 构建 id -> 是否存在 的查找集合
template <typename Container>
std::unordered_set<ObjectID> BuildIdSet(const Container& items)
{
    std::unordered_set<ObjectID> s;
    for (const auto& item : items)
        s.insert(item.id);
    return s;
}

// 收集 TextureRef 中所有被引用的 textureId（非 INVALID_ID）
static void CollectTextureRefs(const Material& mat, std::vector<ObjectID>& out)
{
    auto add = [&](ObjectID id) { if (id != INVALID_ID) out.push_back(id); };

    add(mat.baseColorTexture.textureId);
    add(mat.metallicRoughnessTexture.textureId);
    add(mat.normalTexture.textureId);
    add(mat.occlusionTexture.textureId);
    add(mat.emissiveTexture.textureId);
    add(mat.specularTexture.textureId);
}

} // namespace

// ============================================================
// ValidatePass::Execute
// ============================================================
VoidResult ValidatePass::Execute(Scene& scene)
{
    VoidResult result;
    result.ok = true;

    std::vector<std::string>& warns  = result.warnings;
    auto fail = [&](const std::string& msg) {
        result.ok    = false;
        result.error = msg;
    };

    // ----------------------------------------------------------
    // 预先构建 ID 查找集合
    // ----------------------------------------------------------
    auto nodeIds      = BuildIdSet(scene.nodes);
    auto meshIds      = BuildIdSet(scene.meshes);
    auto materialIds  = BuildIdSet(scene.materials);
    auto textureIds   = BuildIdSet(scene.textures);
    auto skeletonIds  = BuildIdSet(scene.skeletons);
    auto cameraIds    = BuildIdSet(scene.cameras);
    auto lightIds     = BuildIdSet(scene.lights);

    // ----------------------------------------------------------
    // 检查 1：所有顶层对象 ObjectID 唯一（跨类型分别检查）
    // ----------------------------------------------------------
    {
        auto checkUnique = [&](const std::unordered_set<ObjectID>& s, const char* typeName,
                                size_t vectorSize) {
            if (s.size() != vectorSize)
            {
                std::ostringstream oss;
                oss << "Duplicate ObjectID detected in " << typeName
                    << " (expected " << vectorSize << " unique IDs, got " << s.size() << ")";
                return fail(oss.str()), false;
            }
            return true;
        };

        if (!checkUnique(nodeIds,     "nodes",     scene.nodes.size()))     return result;
        if (!checkUnique(meshIds,     "meshes",    scene.meshes.size()))    return result;
        if (!checkUnique(materialIds, "materials", scene.materials.size())) return result;
        if (!checkUnique(textureIds,  "textures",  scene.textures.size()))  return result;
        if (!checkUnique(skeletonIds, "skeletons", scene.skeletons.size())) return result;
        if (!checkUnique(cameraIds,   "cameras",   scene.cameras.size()))   return result;
        if (!checkUnique(lightIds,    "lights",    scene.lights.size()))    return result;
    }

    // ----------------------------------------------------------
    // 检查 2：rootNodes 中的 ID 均能在 nodes 中找到
    // ----------------------------------------------------------
    for (ObjectID rid : scene.rootNodes)
    {
        if (nodeIds.find(rid) == nodeIds.end())
        {
            std::ostringstream oss;
            oss << "rootNodes contains unknown NodeID " << rid;
            return fail(oss.str()), result;
        }
    }

    // ----------------------------------------------------------
    // 检查 3：父子图无循环引用（从每个根出发 DFS，标记已访问）
    // ----------------------------------------------------------
    {
        // 构建 id -> Node* 映射
        std::unordered_map<ObjectID, const Node*> nodeMap;
        for (const auto& n : scene.nodes)
            nodeMap[n.id] = &n;

        std::unordered_set<ObjectID> visited;
        std::unordered_set<ObjectID> inStack;
        bool cycleFound = false;
        ObjectID cycleNode = INVALID_ID;

        std::function<void(ObjectID)> dfs = [&](ObjectID id) {
            if (cycleFound) return;
            if (inStack.count(id))
            {
                cycleFound = true;
                cycleNode  = id;
                return;
            }
            if (visited.count(id)) return;

            visited.insert(id);
            inStack.insert(id);

            auto it = nodeMap.find(id);
            if (it != nodeMap.end())
            {
                for (ObjectID child : it->second->children)
                    dfs(child);
            }
            inStack.erase(id);
        };

        for (ObjectID rid : scene.rootNodes)
            dfs(rid);

        // 也检查不在 rootNodes 中但 parent==INVALID_ID 的孤立节点
        for (const auto& n : scene.nodes)
        {
            if (!visited.count(n.id))
                dfs(n.id);
        }

        if (cycleFound)
        {
            std::ostringstream oss;
            oss << "Cycle detected in node hierarchy at NodeID " << cycleNode;
            return fail(oss.str()), result;
        }
    }

    // ----------------------------------------------------------
    // 检查 4：Node::meshIds 中的 ID 均能在 meshes 中找到
    // ----------------------------------------------------------
    for (const auto& node : scene.nodes)
    {
        for (ObjectID mid : node.meshIds)
        {
            if (meshIds.find(mid) == meshIds.end())
            {
                std::ostringstream oss;
                oss << "Node " << node.id << " references unknown MeshID " << mid;
                return fail(oss.str()), result;
            }
        }
    }

    // ----------------------------------------------------------
    // 检查 5：Node::cameraId
    // ----------------------------------------------------------
    for (const auto& node : scene.nodes)
    {
        if (node.cameraId != INVALID_ID && cameraIds.find(node.cameraId) == cameraIds.end())
        {
            std::ostringstream oss;
            oss << "Node " << node.id << " references unknown CameraID " << node.cameraId;
            return fail(oss.str()), result;
        }
    }

    // ----------------------------------------------------------
    // 检查 6：Node::lightId
    // ----------------------------------------------------------
    for (const auto& node : scene.nodes)
    {
        if (node.lightId != INVALID_ID && lightIds.find(node.lightId) == lightIds.end())
        {
            std::ostringstream oss;
            oss << "Node " << node.id << " references unknown LightID " << node.lightId;
            return fail(oss.str()), result;
        }
    }

    // ----------------------------------------------------------
    // 检查 7：MeshSection::materialId
    // ----------------------------------------------------------
    for (const auto& mesh : scene.meshes)
    {
        for (const auto& sec : mesh.sections)
        {
            if (sec.materialId != INVALID_ID && materialIds.find(sec.materialId) == materialIds.end())
            {
                std::ostringstream oss;
                oss << "Mesh " << mesh.id << " section references unknown MaterialID " << sec.materialId;
                return fail(oss.str()), result;
            }
        }
    }

    // ----------------------------------------------------------
    // 检查 8：indices 最大值 < positions.size()
    // ----------------------------------------------------------
    for (const auto& mesh : scene.meshes)
    {
        if (!mesh.indices.empty())
        {
            uint32_t maxIdx = *std::max_element(mesh.indices.begin(), mesh.indices.end());
            if (mesh.positions.empty() || static_cast<size_t>(maxIdx) >= mesh.positions.size())
            {
                std::ostringstream oss;
                oss << "Mesh " << mesh.id << " index " << maxIdx
                    << " out of range (positions.size=" << mesh.positions.size() << ")";
                return fail(oss.str()), result;
            }
        }
    }

    // ----------------------------------------------------------
    // 检查 9：UV/Color 通道长度 == positions.size()
    // ----------------------------------------------------------
    for (const auto& mesh : scene.meshes)
    {
        const size_t vtxCount = mesh.positions.size();
        for (size_t si = 0; si < mesh.uvs.size(); ++si)
        {
            if (!mesh.uvs[si].empty() && mesh.uvs[si].size() != vtxCount)
            {
                std::ostringstream oss;
                oss << "Mesh " << mesh.id << " uvs[" << si << "].size=" << mesh.uvs[si].size()
                    << " != positions.size=" << vtxCount;
                return fail(oss.str()), result;
            }
        }
        for (size_t ci = 0; ci < mesh.colors.size(); ++ci)
        {
            if (!mesh.colors[ci].empty() && mesh.colors[ci].size() != vtxCount)
            {
                std::ostringstream oss;
                oss << "Mesh " << mesh.id << " colors[" << ci << "].size=" << mesh.colors[ci].size()
                    << " != positions.size=" << vtxCount;
                return fail(oss.str()), result;
            }
        }
    }

    // ----------------------------------------------------------
    // 检查 10：MorphTarget::positionDeltas.size() == positions.size()
    // ----------------------------------------------------------
    for (const auto& mesh : scene.meshes)
    {
        const size_t vtxCount = mesh.positions.size();
        for (size_t ti = 0; ti < mesh.morphTargets.size(); ++ti)
        {
            const auto& mt = mesh.morphTargets[ti];
            if (!mt.positionDeltas.empty() && mt.positionDeltas.size() != vtxCount)
            {
                std::ostringstream oss;
                oss << "Mesh " << mesh.id << " morphTargets[" << ti
                    << "].positionDeltas.size=" << mt.positionDeltas.size()
                    << " != positions.size=" << vtxCount;
                return fail(oss.str()), result;
            }
        }
    }

    // ----------------------------------------------------------
    // 检查 11：TextureRef::textureId
    // ----------------------------------------------------------
    for (const auto& mat : scene.materials)
    {
        std::vector<ObjectID> refs;
        CollectTextureRefs(mat, refs);
        for (ObjectID tid : refs)
        {
            if (textureIds.find(tid) == textureIds.end())
            {
                std::ostringstream oss;
                oss << "Material " << mat.id << " references unknown TextureID " << tid;
                return fail(oss.str()), result;
            }
        }
    }

    // ----------------------------------------------------------
    // 检查 12：Skin::skeletonId
    // ----------------------------------------------------------
    for (const auto& skin : scene.skins)
    {
        if (skin.skeletonId != INVALID_ID && skeletonIds.find(skin.skeletonId) == skeletonIds.end())
        {
            std::ostringstream oss;
            oss << "Skin " << skin.id << " references unknown SkeletonID " << skin.skeletonId;
            return fail(oss.str()), result;
        }
    }

    // ----------------------------------------------------------
    // 检查 13：Skin::meshId
    // ----------------------------------------------------------
    for (const auto& skin : scene.skins)
    {
        if (skin.meshId != INVALID_ID && meshIds.find(skin.meshId) == meshIds.end())
        {
            std::ostringstream oss;
            oss << "Skin " << skin.id << " references unknown MeshID " << skin.meshId;
            return fail(oss.str()), result;
        }
    }

    // ----------------------------------------------------------
    // 检查 14：PointInstancer::prototypeNodeId
    // ----------------------------------------------------------
    for (const auto& pi : scene.pointInstancers)
    {
        if (pi.prototypeNodeId != INVALID_ID && nodeIds.find(pi.prototypeNodeId) == nodeIds.end())
        {
            std::ostringstream oss;
            oss << "PointInstancer " << pi.id << " references unknown prototypeNodeId " << pi.prototypeNodeId;
            return fail(oss.str()), result;
        }
    }

    // ----------------------------------------------------------
    // 检查 15：NodeAnimation::nodeId
    // ----------------------------------------------------------
    for (const auto& clip : scene.animations)
    {
        for (const auto& ch : clip.nodeChannels)
        {
            if (ch.nodeId == INVALID_ID || nodeIds.find(ch.nodeId) == nodeIds.end())
            {
                std::ostringstream oss;
                oss << "AnimationClip " << clip.id
                    << " NodeAnimation references unknown NodeID " << ch.nodeId;
                return fail(oss.str()), result;
            }
        }
    }

    // ----------------------------------------------------------
    // 检查 16：MorphAnimation::meshId + morphIndex 范围
    // ----------------------------------------------------------
    // 构建 meshId -> mesh* 的快速映射
    std::unordered_map<ObjectID, const Mesh*> meshMap;
    for (const auto& m : scene.meshes)
        meshMap[m.id] = &m;

    for (const auto& clip : scene.animations)
    {
        for (const auto& ch : clip.morphChannels)
        {
            auto it = meshMap.find(ch.meshId);
            if (it == meshMap.end())
            {
                std::ostringstream oss;
                oss << "AnimationClip " << clip.id
                    << " MorphAnimation references unknown MeshID " << ch.meshId;
                return fail(oss.str()), result;
            }
            if (ch.morphIndex >= it->second->morphTargets.size())
            {
                std::ostringstream oss;
                oss << "AnimationClip " << clip.id
                    << " MorphAnimation morphIndex=" << ch.morphIndex
                    << " out of range (morphTargets.size="
                    << it->second->morphTargets.size() << ")";
                return fail(oss.str()), result;
            }
        }
    }

    return result;
}

} // namespace mc
