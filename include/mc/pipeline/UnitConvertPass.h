#pragma once

#include "mc/pipeline/IPass.h"

namespace mc {

// ============================================================
// UnitConvertPass —— Phase13 单位转换
// ============================================================
// 对 Scene 中所有顶点坐标、节点平移分量乘以 scaleFactor。
// 典型用例：cm→m（factor=0.01），mm→m（factor=0.001），in→m（factor=0.0254）。

class UnitConvertPass : public IPass
{
public:
    // factor: 目标单位 / 源单位，例如 0.01f 表示 cm→m
    explicit UnitConvertPass(float factor);

    std::string Name() const override;
    VoidResult  Execute(Scene& scene) override;

    float Factor() const { return m_factor; }

private:
    float m_factor;

    void ScaleMeshPositions(Scene& scene) const;
    void ScaleNodeTranslations(Scene& scene) const;
};

} // namespace mc
