// ============================================================
// AnimationOptimizePass 单元测试
// ============================================================
// Phase14 验收标准：
//   - 空场景处理正确
//   - Step 插值冗余关键帧移除
//   - Linear 插值共线关键帧移除
//   - CubicSpline 不被修改

#include <gtest/gtest.h>
#include "mc/pipeline/AnimationOptimizePass.h"
#include "mc/pipeline/Pipeline.h"
#include "mc/core/Scene.h"
#include "mc/core/Animation.h"

using namespace mc;

// ============================================================
// 辅助
// ============================================================
static VoidResult RunOptimize(Scene& scene)
{
    AnimationOptimizePass pass;
    return pass.Execute(scene);
}

// ============================================================
// 空场景
// ============================================================

TEST(AnimationOptimizePassTest, EmptyScene)
{
    Scene scene;
    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok) << r.error;
}

TEST(AnimationOptimizePassTest, EmptyAnimationClip)
{
    Scene scene;
    AnimationClip clip;
    clip.id   = scene.AllocateId();
    clip.name = "EmptyClip";
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok) << r.error;
}

// ============================================================
// Step 插值：移除值相同的相邻关键帧
// ============================================================

TEST(AnimationOptimizePassTest, StepInterp_RemoveIdenticalFloatKeyframes)
{
    Scene scene;
    AnimationClip clip;
    clip.id       = scene.AllocateId();
    clip.name     = "StepTest";
    clip.endTime  = 3.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.visibility.interpolation = AnimationInterpolation::Step;

    // 添加重复值的关键帧
    nodeAnim.visibility.keys = {
        {0.0, 1.0f},  // 保留（首帧）
        {1.0, 1.0f},  // 应移除（值相同）
        {2.0, 1.0f},  // 应移除（值相同）
        {3.0, 0.0f},  // 保留（值不同）
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok) << r.error;

    ASSERT_EQ(scene.animations.size(), 1u);
    ASSERT_EQ(scene.animations[0].nodeChannels.size(), 1u);

    const auto& track = scene.animations[0].nodeChannels[0].visibility;
    EXPECT_EQ(track.keys.size(), 2u);  // 应从4个减为2个
    EXPECT_DOUBLE_EQ(track.keys[0].time, 0.0);
    EXPECT_FLOAT_EQ(track.keys[0].value, 1.0f);
    EXPECT_DOUBLE_EQ(track.keys[1].time, 3.0);
    EXPECT_FLOAT_EQ(track.keys[1].value, 0.0f);
}

TEST(AnimationOptimizePassTest, StepInterp_RemoveIdenticalVec3Keyframes)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.name    = "StepVec3Test";
    clip.endTime = 2.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.translation.interpolation = AnimationInterpolation::Step;

    Vec3 posA = {0.0f, 0.0f, 0.0f};
    Vec3 posB = {1.0f, 2.0f, 3.0f};

    nodeAnim.translation.keys = {
        {0.0, posA},
        {1.0, posA},  // 应移除（值相同）
        {2.0, posB},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].translation;
    EXPECT_EQ(track.keys.size(), 2u);
}

// ============================================================
// Step 插值：保留值不同的关键帧
// ============================================================

TEST(AnimationOptimizePassTest, StepInterp_KeepDifferentFloatValues)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 3.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.visibility.interpolation = AnimationInterpolation::Step;

    nodeAnim.visibility.keys = {
        {0.0, 0.0f},
        {1.0, 0.5f},
        {2.0, 1.0f},
        {3.0, 0.0f},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].visibility;
    EXPECT_EQ(track.keys.size(), 4u);  // 全部保留（值均不同）
}

// ============================================================
// Linear 插值：移除可被线性插值替代的中间关键帧
// ============================================================

TEST(AnimationOptimizePassTest, LinearInterp_RemoveCoplanarFloatKeyframes)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 2.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.visibility.interpolation = AnimationInterpolation::Linear;

    // 0→0.5→1.0 是完美线性的
    nodeAnim.visibility.keys = {
        {0.0, 0.0f},
        {1.0, 0.5f},  // 可被两端线性插值得到（0.0 + (1.0-0.0)*0.5 = 0.5）
        {2.0, 1.0f},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].visibility;
    EXPECT_EQ(track.keys.size(), 2u);  // 中间帧应被移除
}

TEST(AnimationOptimizePassTest, LinearInterp_KeepNonLinearFloatKeyframes)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 2.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.visibility.interpolation = AnimationInterpolation::Linear;

    // 0→0.8→1.0，0.8 不共线
    nodeAnim.visibility.keys = {
        {0.0, 0.0f},
        {1.0, 0.8f},  // 偏差 0.3（默认 tolerance=0.0001，不共线）
        {2.0, 1.0f},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].visibility;
    EXPECT_EQ(track.keys.size(), 3u);  // 全部保留
}

TEST(AnimationOptimizePassTest, LinearInterp_RemoveCollinearVec3Keyframes)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 2.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.translation.interpolation = AnimationInterpolation::Linear;

    Vec3 start = {0.0f, 0.0f, 0.0f};
    Vec3 mid   = {1.0f, 1.0f, 1.0f};  // 完美中点
    Vec3 end   = {2.0f, 2.0f, 2.0f};

    nodeAnim.translation.keys = {
        {0.0, start},
        {1.0, mid},
        {2.0, end},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].translation;
    EXPECT_EQ(track.keys.size(), 2u);  // 中间帧应被移除
}

// ============================================================
// CubicSpline：不移除关键帧
// ============================================================

TEST(AnimationOptimizePassTest, CubicSpline_KeepAllKeyframes)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 2.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.translation.interpolation = AnimationInterpolation::CubicSpline;

    // 即使共线，CubicSpline 也不移除
    Vec3 v0 = {0.0f, 0.0f, 0.0f};
    Vec3 v1 = {1.0f, 1.0f, 1.0f};
    Vec3 v2 = {2.0f, 2.0f, 2.0f};

    nodeAnim.translation.keys = {
        {0.0, v0, v0, v0},
        {1.0, v1, v1, v1},
        {2.0, v2, v2, v2},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].translation;
    EXPECT_EQ(track.keys.size(), 3u);  // 全部保留
}

// ============================================================
// 小轨道：<=2 个关键帧不处理
// ============================================================

TEST(AnimationOptimizePassTest, SingleKeyframe_Unchanged)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 0.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.translation.interpolation = AnimationInterpolation::Linear;
    nodeAnim.translation.keys = {{0.0, Vec3{1.0f, 2.0f, 3.0f}}};

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].translation;
    EXPECT_EQ(track.keys.size(), 1u);
}

// ============================================================
// Rotation 优化
// ============================================================

TEST(AnimationOptimizePassTest, LinearRotation_RemoveCollinear)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 2.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;
    nodeAnim.rotation.interpolation = AnimationInterpolation::Linear;

    Quaternion q0 = Quaternion::Identity();
    Quaternion q1 = Quaternion::Identity();  // 完全相同
    Quaternion q2 = {0.0f, 0.0f, 0.0f, 1.0f}; // 也是 identity

    nodeAnim.rotation.keys = {
        {0.0, q0},
        {1.0, q1},  // 应移除
        {2.0, q2},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& track = scene.animations[0].nodeChannels[0].rotation;
    EXPECT_EQ(track.keys.size(), 2u);  // 中间帧应被移除
}

// ============================================================
// 混合通道：T/R/S 各轨道独立优化
// ============================================================

TEST(AnimationOptimizePassTest, MixedChannels_IndependentOptimization)
{
    Scene scene;
    AnimationClip clip;
    clip.id      = scene.AllocateId();
    clip.endTime = 2.0;

    NodeAnimation nodeAnim;
    nodeAnim.nodeId = scene.AddNode().id;

    // Translation: 共线（中间帧可移除）
    nodeAnim.translation.interpolation = AnimationInterpolation::Linear;
    nodeAnim.translation.keys = {
        {0.0, Vec3{0.0f, 0.0f, 0.0f}},
        {1.0, Vec3{0.5f, 0.5f, 0.5f}},
        {2.0, Vec3{1.0f, 1.0f, 1.0f}},
    };

    // Rotation: 不共线（中间帧保留）
    nodeAnim.rotation.interpolation = AnimationInterpolation::Linear;
    nodeAnim.rotation.keys = {
        {0.0, Quaternion::Identity()},
        {1.0, Quaternion{0.0f, 0.5f, 0.0f, 0.866f}},  // 约60°绕Y
        {2.0, Quaternion{0.0f, 0.866f, 0.0f, 0.5f}},  // 约120°绕Y
    };

    // Scale: 共线
    nodeAnim.scale.interpolation = AnimationInterpolation::Linear;
    nodeAnim.scale.keys = {
        {0.0, Vec3{1.0f, 1.0f, 1.0f}},
        {1.0, Vec3{1.5f, 1.5f, 1.5f}},
        {2.0, Vec3{2.0f, 2.0f, 2.0f}},
    };

    clip.nodeChannels.push_back(std::move(nodeAnim));
    scene.animations.push_back(std::move(clip));

    auto r = RunOptimize(scene);
    EXPECT_TRUE(r.ok);

    const auto& ch = scene.animations[0].nodeChannels[0];
    EXPECT_EQ(ch.translation.keys.size(), 2u);  // 已优化
    EXPECT_EQ(ch.rotation.keys.size(),    3u);  // 保留
    EXPECT_EQ(ch.scale.keys.size(),       2u);  // 已优化
}

// ============================================================
// 通过 Pipeline 集成验证
// ============================================================

TEST(AnimationOptimizePassTest, InPipeline)
{
    Pipeline pipeline;
    pipeline.AddPass(std::make_unique<AnimationOptimizePass>());

    Scene scene;
    auto r = pipeline.Execute(scene);
    EXPECT_TRUE(r.ok) << r.error;
}
