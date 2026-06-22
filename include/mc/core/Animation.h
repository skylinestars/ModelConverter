#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>
#include <vector>

namespace mc {

// ============================================================
// Animation
// 注意：Importer 把 FBX Bone Curve / USD SkelAnimation 转换成 NodeAnimation，Core 层不需要感知这个来源差异
// ============================================================

enum class AnimationInterpolation
{
    Step,        // GLTF: STEP, FBX: Constant, USD: Held
    Linear,      // GLTF: LINEAR, FBX: Linear, USD: Linear
    CubicSpline  // GLTF: CUBICSPLINE, FBX: Cubic
};

// ---- KeyFrame ----
// time 单位：秒（seconds）。FBX FbxTime / USD timeCodes 由各自 Importer 转换为秒后写入。
template <typename T>
struct KeyFrame
{
    double time   = 0.0;
    T      value  = {};
    T      inTan  = {};  // 仅 CubicSpline 时有效（入切线，与 value 同类型）
    T      outTan = {};  // 仅 CubicSpline 时有效（出切线，与 value 同类型）
};

// ---- AnimationTrack ----
template <typename T>
struct AnimationTrack
{
    AnimationInterpolation   interpolation = AnimationInterpolation::Linear;
    std::vector<KeyFrame<T>> keys;

    bool Empty() const
    {
        return keys.empty();
    }

    bool Valid() const
    {
        return keys.size() > 0;
    }
};

using TrackVec3  = AnimationTrack<Vec3>;
using TrackQuat  = AnimationTrack<Quaternion>;
using TrackFloat = AnimationTrack<float>;

// ---- NodeAnimation: TRS + Visibility ----
struct NodeAnimation
{
    ObjectID    nodeId = INVALID_ID;

    TrackVec3   translation;
    TrackQuat   rotation;
    TrackVec3   scale;

    // Visibility: 0.0 = hidden, 1.0 = visible (USD / FBX)
    TrackFloat  visibility;
};

// ---- MorphAnimation: BlendShape weight curve ----
// Controls one MorphTarget on one Mesh.
// morphIndex is the vector index into Mesh::morphTargets (not ObjectID),
// because MorphTarget is an embedded array, not a Scene-level object.
struct MorphAnimation
{
    ObjectID    meshId     = INVALID_ID;  // 引用 Scene::meshes
    // 注意：morphIndex 是 vector index 而非 ObjectID，这是刻意的例外。
    // 原因：MorphTarget 是 Mesh 的内嵌数组，不是 Scene 顶层对象，没有独立 ObjectID。
    // 实践中 MorphTarget 极少被删除或重排，常规操作下 index 稳定。
    uint32_t    morphIndex = 0;  // Mesh::morphTargets[morphIndex]
    TrackFloat  weights;
};

// ---- AnimationClip ----
struct AnimationClip
{
    ObjectID                    id;
    std::string                 name;

    //有些动画：100ms~200ms，所以不能用duration
    double startTime = 0.0;
    double endTime = 0.0;

    std::vector<NodeAnimation>  nodeChannels;
    std::vector<MorphAnimation> morphChannels;

    double Duration() const
    {
        return endTime - startTime;
    }
};

} // namespace mc