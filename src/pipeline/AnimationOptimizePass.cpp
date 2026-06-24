#include "mc/pipeline/AnimationOptimizePass.h"
#include "mc/core/Scene.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"

#include <cmath>
#include <sstream>

namespace mc {

namespace {

// ============================================================
// Vec3 距离计算
// ============================================================
inline float Vec3Dist(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ============================================================
// Quaternion 角度差（弧度，取最小弧）
// ============================================================
inline float QuatAngleDiff(const Quaternion& a, const Quaternion& b)
{
    // dot = |a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w|
    float dotVal = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    // 钳制到 [-1, 1] 避免浮点误差
    if (dotVal > 1.0f) dotVal = 1.0f;
    return 2.0f * std::acos(dotVal);
}

// ============================================================
// Vec3 线性插值
// ============================================================
inline Vec3 LerpVec3(const Vec3& a, const Vec3& b, float t)
{
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

// ============================================================
// Quaternion 球面线性插值（NLerp 近似）
// ============================================================
inline Quaternion SlerpQuat(const Quaternion& a, const Quaternion& b, float t)
{
    // 简化的 NLerp（对于小角度差足够精确）
    float dotVal = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    // 取最短弧
    float sign = (dotVal >= 0.0f) ? 1.0f : -1.0f;
    float t1 = 1.0f - t;
    float t2 = sign * t;

    float nx = t1 * a.x + t2 * b.x;
    float ny = t1 * a.y + t2 * b.y;
    float nz = t1 * a.z + t2 * b.z;
    float nw = t1 * a.w + t2 * b.w;

    float len = std::sqrt(nx * nx + ny * ny + nz * nz + nw * nw);
    if (len < 1e-12f) return a;

    return {nx / len, ny / len, nz / len, nw / len};
}

// ============================================================
// 优化 Vec3 track（translation / scale）
// ============================================================
size_t OptimizeVec3Track(TrackVec3& track, float tolerance)
{
    if (track.keys.size() <= 2) return 0;

    size_t removed = 0;
    std::vector<KeyFrame<Vec3>> optimized;
    optimized.reserve(track.keys.size());

    // 始终保留第一个关键帧
    optimized.push_back(track.keys.front());

    if (track.interpolation == AnimationInterpolation::Step)
    {
        // Step：移除相邻值相同的关键帧
        for (size_t i = 1; i < track.keys.size(); ++i)
        {
            const auto& prev = optimized.back();
            const auto& curr = track.keys[i];
            float dist = Vec3Dist(prev.value, curr.value);
            if (dist > tolerance)
                optimized.push_back(curr);
            else
                ++removed;
        }
    }
    else if (track.interpolation == AnimationInterpolation::Linear)
    {
        // Linear：检查中间关键帧是否可由两端线性插值得到
        for (size_t i = 1; i < track.keys.size() - 1; ++i)
        {
            const auto& prev = optimized.back();
            const auto& curr = track.keys[i];
            const auto& next = track.keys[i + 1];

            // 计算 curr 在 prev→next 时间轴上的 t 值
            double totalDur = next.time - prev.time;
            if (totalDur <= 1e-12)
            {
                // 时间戳相同，保留（异常情况）
                optimized.push_back(curr);
                continue;
            }
            double t = (curr.time - prev.time) / totalDur;

            // 线性插值预期值
            Vec3 expected = LerpVec3(prev.value, next.value, static_cast<float>(t));
            float dist = Vec3Dist(curr.value, expected);

            if (dist > tolerance)
                optimized.push_back(curr);
            else
                ++removed;
        }
        // 始终保留最后一个关键帧
        optimized.push_back(track.keys.back());
    }
    else
    {
        // CubicSpline：不移除关键帧
        for (size_t i = 1; i < track.keys.size(); ++i)
            optimized.push_back(track.keys[i]);
    }

    if (removed > 0)
        track.keys = std::move(optimized);

    return removed;
}

// ============================================================
// 优化 Quaternion track（rotation）
// ============================================================
size_t OptimizeQuatTrack(TrackQuat& track, float tolerance)
{
    if (track.keys.size() <= 2) return 0;

    size_t removed = 0;
    std::vector<KeyFrame<Quaternion>> optimized;
    optimized.reserve(track.keys.size());

    optimized.push_back(track.keys.front());

    if (track.interpolation == AnimationInterpolation::Step)
    {
        for (size_t i = 1; i < track.keys.size(); ++i)
        {
            const auto& prev = optimized.back();
            const auto& curr = track.keys[i];
            float angleDiff = QuatAngleDiff(prev.value, curr.value);
            if (angleDiff > tolerance)
                optimized.push_back(curr);
            else
                ++removed;
        }
    }
    else if (track.interpolation == AnimationInterpolation::Linear)
    {
        for (size_t i = 1; i < track.keys.size() - 1; ++i)
        {
            const auto& prev = optimized.back();
            const auto& curr = track.keys[i];
            const auto& next = track.keys[i + 1];

            double totalDur = next.time - prev.time;
            if (totalDur <= 1e-12)
            {
                optimized.push_back(curr);
                continue;
            }
            double t = (curr.time - prev.time) / totalDur;

            Quaternion expected = SlerpQuat(prev.value, next.value, static_cast<float>(t));
            float angleDiff = QuatAngleDiff(curr.value, expected);

            if (angleDiff > tolerance)
                optimized.push_back(curr);
            else
                ++removed;
        }
        optimized.push_back(track.keys.back());
    }
    else
    {
        // CubicSpline：保守处理
        for (size_t i = 1; i < track.keys.size(); ++i)
            optimized.push_back(track.keys[i]);
    }

    if (removed > 0)
        track.keys = std::move(optimized);

    return removed;
}

// ============================================================
// 优化 float track（visibility / weights）
// ============================================================
size_t OptimizeFloatTrack(TrackFloat& track, float tolerance)
{
    if (track.keys.size() <= 2) return 0;

    size_t removed = 0;
    std::vector<KeyFrame<float>> optimized;
    optimized.reserve(track.keys.size());

    optimized.push_back(track.keys.front());

    if (track.interpolation == AnimationInterpolation::Step)
    {
        for (size_t i = 1; i < track.keys.size(); ++i)
        {
            const auto& prev = optimized.back();
            const auto& curr = track.keys[i];
            float diff = std::abs(prev.value - curr.value);
            if (diff > tolerance)
                optimized.push_back(curr);
            else
                ++removed;
        }
    }
    else if (track.interpolation == AnimationInterpolation::Linear)
    {
        for (size_t i = 1; i < track.keys.size() - 1; ++i)
        {
            const auto& prev = optimized.back();
            const auto& curr = track.keys[i];
            const auto& next = track.keys[i + 1];

            double totalDur = next.time - prev.time;
            if (totalDur <= 1e-12)
            {
                optimized.push_back(curr);
                continue;
            }
            double t = (curr.time - prev.time) / totalDur;

            float expected = prev.value + (next.value - prev.value) * static_cast<float>(t);
            float diff = std::abs(curr.value - expected);

            if (diff > tolerance)
                optimized.push_back(curr);
            else
                ++removed;
        }
        optimized.push_back(track.keys.back());
    }
    else
    {
        for (size_t i = 1; i < track.keys.size(); ++i)
            optimized.push_back(track.keys[i]);
    }

    if (removed > 0)
        track.keys = std::move(optimized);

    return removed;
}

} // namespace

// ============================================================
// AnimationOptimizePass
// ============================================================

AnimationOptimizePass::AnimationOptimizePass() = default;

std::string AnimationOptimizePass::Name() const
{
    return "AnimationOptimizePass";
}

VoidResult AnimationOptimizePass::Execute(Scene& scene)
{
    VoidResult result;
    result.ok = true;

    m_removedKeyframes = 0;
    size_t totalKeyframesBefore = 0;
    size_t totalKeyframesAfter  = 0;

    for (auto& clip : scene.animations)
    {
        // 优化 NodeAnimation 通道
        for (auto& nodeAnim : clip.nodeChannels)
        {
            totalKeyframesBefore += nodeAnim.translation.keys.size();
            totalKeyframesBefore += nodeAnim.rotation.keys.size();
            totalKeyframesBefore += nodeAnim.scale.keys.size();
            totalKeyframesBefore += nodeAnim.visibility.keys.size();

            m_removedKeyframes += OptimizeVec3Track(nodeAnim.translation, translationTolerance);
            m_removedKeyframes += OptimizeQuatTrack(nodeAnim.rotation, rotationTolerance);
            m_removedKeyframes += OptimizeVec3Track(nodeAnim.scale, scaleTolerance);
            m_removedKeyframes += OptimizeFloatTrack(nodeAnim.visibility, weightTolerance);
        }

        // 优化 MorphAnimation 通道
        for (auto& morphAnim : clip.morphChannels)
        {
            totalKeyframesBefore += morphAnim.weights.keys.size();
            m_removedKeyframes += OptimizeFloatTrack(morphAnim.weights, weightTolerance);
        }

        // 统计优化后关键帧数
        for (auto& nodeAnim : clip.nodeChannels)
        {
            totalKeyframesAfter += nodeAnim.translation.keys.size();
            totalKeyframesAfter += nodeAnim.rotation.keys.size();
            totalKeyframesAfter += nodeAnim.scale.keys.size();
            totalKeyframesAfter += nodeAnim.visibility.keys.size();
        }
        for (auto& morphAnim : clip.morphChannels)
        {
            totalKeyframesAfter += morphAnim.weights.keys.size();
        }
    }

    if (m_removedKeyframes > 0)
    {
        std::ostringstream oss;
        oss << "AnimationOptimizePass: removed " << m_removedKeyframes
            << " redundant keyframe(s) ("
            << totalKeyframesBefore << " -> " << totalKeyframesAfter << ")";
        Logger::Instance().LogInfo(oss.str());
    }
    else
    {
        Logger::Instance().LogInfo("AnimationOptimizePass: no redundant keyframes found.");
    }

    return result;
}

} // namespace mc
