#pragma once

#include "mc/pipeline/IPass.h"

namespace mc {

// ============================================================
// AxisConvertPass —— Phase13 坐标轴转换
// ============================================================
// 支持的转换：
//   YUp  → ZUp   （如 GLTF/OBJ → Blender/FBX）
//   ZUp  → YUp   （如 FBX/3DS  → GLTF/USD）
//   XUp  → YUp
//   任意到任意（通过 UpAxis 枚举）

enum class UpAxis { X, Y, Z };

class AxisConvertPass : public IPass
{
public:
    // fromAxis: 当前坐标系朝上轴
    // toAxis  : 目标坐标系朝上轴
    explicit AxisConvertPass(UpAxis fromAxis, UpAxis toAxis);

    std::string Name() const override;
    VoidResult  Execute(Scene& scene) override;

private:
    UpAxis m_from;
    UpAxis m_to;

    // 对单个 Vec3 执行轴重映射
    Vec3 RemapVec3(const Vec3& v) const;

    void TransformMeshPositions(Scene& scene) const;
    void TransformMeshNormals(Scene& scene) const;
    void TransformNodeMatrices(Scene& scene) const;
    void TransformMorphTargetDeltas(Scene& scene) const;
};

} // namespace mc
