#include "mc/pipeline/HandednessFlipPass.h"
#include "mc/common/Logger.h"
#include <utility>

namespace mc {

HandednessFlipPass::HandednessFlipPass(Handedness from, Handedness to)
    : m_from(from), m_to(to) {}

std::string HandednessFlipPass::Name() const { return "HandednessFlipPass"; }

void HandednessFlipPass::FlipMeshPositions(Scene& scene) const
{
    for (auto& mesh : scene.meshes)
        for (auto& p : mesh.positions)
            p.x = -p.x;
}

void HandednessFlipPass::FlipMeshNormals(Scene& scene) const
{
    for (auto& mesh : scene.meshes)
        for (auto& n : mesh.normals)
            n.x = -n.x;
}

void HandednessFlipPass::FlipMeshWinding(Scene& scene) const
{
    for (auto& mesh : scene.meshes)
    {
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

void HandednessFlipPass::FlipNodeMatrices(Scene& scene) const
{
    // M' = S * M * S，其中 S = diag(-1,1,1,1)
    // 对列主序矩阵可简化为：
    // - 左乘 S：row=0 的元素取反
    // - 右乘 S：col=0 的元素取反
    // 两者叠加后：
    //   m01,m02,m03,m10,m20,m30 取反
    for (auto& node : scene.nodes)
    {
        float* m = node.localMatrix.m;
        m[1]  = -m[1];
        m[2]  = -m[2];
        m[3]  = -m[3];
        m[4]  = -m[4];
        m[8]  = -m[8];
        m[12] = -m[12];
    }
}

VoidResult HandednessFlipPass::Execute(Scene& scene)
{
    if (m_from == m_to) return {true, ""};

    FlipMeshPositions(scene);
    FlipMeshNormals(scene);
    FlipMeshWinding(scene);
    FlipNodeMatrices(scene);

    scene.metadata.handedness = m_to;

    const char* handName[] = {"Left", "Right"};
    Logger::Instance().LogInfo(
        std::string("HandednessFlipPass: ") +
        handName[(int)m_from] + " -> " + handName[(int)m_to] +
        " (mirror X + fix winding)");

    return {true, ""};
}

} // namespace mc
