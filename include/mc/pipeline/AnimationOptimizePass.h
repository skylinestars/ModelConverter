#pragma once

#include "mc/pipeline/IPass.h"

namespace mc {

// ============================================================
// AnimationOptimizePass —— Phase14 动画关键帧优化
// ============================================================
// 移除冗余关键帧，减小动画数据量，同时保持动画精度。
//
// 优化策略：
//   - Step 插值：移除相邻值相同的冗余关键帧
//   - Linear 插值：移除共线的中间关键帧（值可由两端线性插值得到）
//   - CubicSpline：保守处理，不移除关键帧（切线数据难以简单判断冗余）
//
// 可配置的容差阈值：
//   - translationTolerance：平移向量欧氏距离阈值
//   - rotationTolerance：旋转角度差阈值（弧度）
//   - scaleTolerance：缩放向量欧氏距离阈值
//   - weightTolerance：浮点权重阈值

class AnimationOptimizePass : public IPass
{
public:
    AnimationOptimizePass();

    std::string Name() const override;
    VoidResult  Execute(Scene& scene) override;

    // 可配置的容差（默认值兼顾精度与压缩率）
    float translationTolerance = 0.0001f;   // 平移容差（世界单位）
    float rotationTolerance    = 0.001745f; // 旋转容差（弧度，约 0.1°）
    float scaleTolerance       = 0.0001f;   // 缩放容差
    float weightTolerance      = 0.0001f;   // 权重容差

private:
    // 状态记录
    size_t m_removedKeyframes = 0;
};

} // namespace mc
