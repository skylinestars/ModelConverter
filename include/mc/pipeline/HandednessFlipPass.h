#pragma once

#include "mc/pipeline/IPass.h"

namespace mc {

// ============================================================
// HandednessFlipPass —— Left/Right Handedness 转换
// ============================================================
// 通过镜像 X 轴把坐标系手性翻转：
//   p' = S * p,  S = diag(-1, 1, 1)
// 为保持面朝向不变，会同步翻转三角形 winding。

class HandednessFlipPass : public IPass
{
public:
    HandednessFlipPass(Handedness from, Handedness to);

    std::string Name() const override;
    VoidResult  Execute(Scene& scene) override;

private:
    Handedness m_from;
    Handedness m_to;

    void FlipMeshPositions(Scene& scene) const;
    void FlipMeshNormals(Scene& scene) const;
    void FlipMeshWinding(Scene& scene) const;
    void FlipNodeMatrices(Scene& scene) const;
};

} // namespace mc
