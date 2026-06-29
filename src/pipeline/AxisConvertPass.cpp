#include "mc/pipeline/AxisConvertPass.h"
#include "mc/common/Logger.h"
#include <cmath>
#include <string>

namespace mc {

static Axis ToAxis(UpAxis up)
{
    switch (up)
    {
        case UpAxis::X: return Axis::X;
        case UpAxis::Y: return Axis::Y;
        case UpAxis::Z: return Axis::Z;
        default: return Axis::Y;
    }
}

// ============================================================
// 轴重映射表 —— 把 (from,to) 映射为一个 3x3 旋转矩阵（行向量形式）
// 仅包含±90° 旋转，所有分量仅取 -1、0、1。
// ============================================================
// 结果向量 r 满足：r = M * v，其中 M 由 mapX/Y/Z 定义：
//   new_x = v[ srcX ] * signX
//   new_y = v[ srcY ] * signY
//   new_z = v[ srcZ ] * signZ

struct AxisRemap { int srcX, srcY, srcZ; float signX, signY, signZ; };

static void BuildBasisMatrix(const AxisRemap& r, float outR[16])
{
    for (int i = 0; i < 16; ++i) outR[i] = 0.0f;
    outR[15] = 1.0f;

    // 列主序存储 m[col*4+row]，令 R * v = RemapVec3(v)：
    //   row 0: R[0, srcX] = signX → 索引 srcX*4+0
    //   row 1: R[1, srcY] = signY → 索引 srcY*4+1
    //   row 2: R[2, srcZ] = signZ → 索引 srcZ*4+2
    outR[r.srcX * 4 + 0] = r.signX;
    outR[r.srcY * 4 + 1] = r.signY;
    outR[r.srcZ * 4 + 2] = r.signZ;
}

static void Transpose4x4(const float inM[16], float outM[16])
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            outM[col * 4 + row] = inM[row * 4 + col];
}

static void Mul4x4(const float a[16], const float b[16], float outM[16])
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[col * 4 + k];
            outM[col * 4 + row] = sum;
        }
    }
}

static AxisRemap GetRemap(UpAxis from, UpAxis to)
{
    if (from == to) return {0, 1, 2, 1.f, 1.f, 1.f};   // identity
    if (from == UpAxis::Y && to == UpAxis::Z) return {0, 2, 1, 1.f,-1.f, 1.f}; // YUp→ZUp: (x,-z,y)  旧+Y(上)→新+Z(上)
    if (from == UpAxis::Z && to == UpAxis::Y) return {0, 2, 1, 1.f, 1.f,-1.f}; // ZUp→YUp: (x,z,-y)  旧+Z(上)→新+Y(上)
    if (from == UpAxis::X && to == UpAxis::Y) return {1, 0, 2,-1.f, 1.f, 1.f}; // XUp→YUp
    if (from == UpAxis::Y && to == UpAxis::X) return {1, 0, 2, 1.f,-1.f, 1.f}; // YUp→XUp
    if (from == UpAxis::X && to == UpAxis::Z) return {1, 2, 0,-1.f, 1.f, 1.f}; // XUp→ZUp
    if (from == UpAxis::Z && to == UpAxis::X) return {2, 1, 0, 1.f,-1.f, 1.f}; // ZUp→XUp
    return {0, 1, 2, 1.f, 1.f, 1.f};
}

// ============================================================
// AxisConvertPass
// ============================================================

AxisConvertPass::AxisConvertPass(UpAxis fromAxis, UpAxis toAxis)
    : m_from(fromAxis), m_to(toAxis) {}

std::string AxisConvertPass::Name() const { return "AxisConvertPass"; }

Vec3 AxisConvertPass::RemapVec3(const Vec3& v) const
{
    AxisRemap r = GetRemap(m_from, m_to);
    const float src[3] = {v.x, v.y, v.z};
    return Vec3{src[r.srcX] * r.signX,
                src[r.srcY] * r.signY,
                src[r.srcZ] * r.signZ};
}

void AxisConvertPass::TransformMeshPositions(Scene& scene) const
{
    for (auto& mesh : scene.meshes)
        for (auto& p : mesh.positions) p = RemapVec3(p);
}

void AxisConvertPass::TransformMeshNormals(Scene& scene) const
{
    for (auto& mesh : scene.meshes)
        for (auto& n : mesh.normals) n = RemapVec3(n);
}

void AxisConvertPass::TransformNodeMatrices(Scene& scene) const
{
    AxisRemap r = GetRemap(m_from, m_to);
    float R[16], Rt[16];
    BuildBasisMatrix(r, R);
    Transpose4x4(R, Rt);

    // 对每个节点的 localMatrix 做相似变换：M' = R * M * R^T
    // 其中 R 为坐标轴基变换矩阵（正交矩阵）
    for (auto& node : scene.nodes)
    {
        float tmp[16], dst[16];
        Mul4x4(R, node.localMatrix.m, tmp);
        Mul4x4(tmp, Rt, dst);
        for (int i = 0; i < 16; ++i) node.localMatrix.m[i] = dst[i];
    }
}

void AxisConvertPass::TransformMorphTargetDeltas(Scene& scene) const
{
    for (auto& mesh : scene.meshes)
        for (auto& mt : mesh.morphTargets)
            for (auto& d : mt.positionDeltas)
                d = RemapVec3(d);
}

VoidResult AxisConvertPass::Execute(Scene& scene)
{
    if (m_from == m_to) return {true, ""};

    TransformMeshPositions(scene);
    TransformMeshNormals(scene);
    TransformNodeMatrices(scene);
    TransformMorphTargetDeltas(scene);

    scene.metadata.upAxis = ToAxis(m_to);

    const char* axisName[] = {"X", "Y", "Z"};
    Logger::Instance().LogInfo(
        std::string("AxisConvertPass: ") +
        axisName[(int)m_from] + "Up -> " + axisName[(int)m_to] + "Up");
    return {true, ""};
}

} // namespace mc
