// ============================================================
// Phase14 GLTF 动画导入验收测试
// ============================================================
// 使用 robot_arm.glb（含蒙皮动画的 GLB 文件）
//
// 验收标准：
//   - 动画导入成功
//   - 关键帧数量 > 0
//   - 时长正确（duration > 0）
//   - ValidatePass 通过
//   - AnimationOptimizePass 通过
//   - 旋转误差 < 0.1°（约 0.001745 弧度）
//   - 位移误差 < 0.001

#include <gtest/gtest.h>
#include "mc/pipeline/ValidatePass.h"
#include "mc/pipeline/Pipeline.h"
#include "mc/core/Scene.h"
#include "mc/importer/IImporter.h"
#include "mc/core/Animation.h"

#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);

// ============================================================
// Fixture
// ============================================================
class GltfAnimationTest : public ::testing::Test
{
protected:
    HMODULE             m_hDll     = nullptr;
    mc::IImporter*      m_importer = nullptr;
    DestroyImporterFunc m_destroy  = nullptr;

    void SetUp() override
    {
        m_hDll = LoadLibraryA(PLUGIN_GLTF_FILENAME);
        ASSERT_NE(m_hDll, nullptr)
            << "Cannot load " PLUGIN_GLTF_FILENAME ". GetLastError=" << GetLastError();

        auto create = reinterpret_cast<CreateImporterFunc>(
            GetProcAddress(m_hDll, "CreateImporter"));
        ASSERT_NE(create, nullptr) << "CreateImporter export not found";

        m_destroy = reinterpret_cast<DestroyImporterFunc>(
            GetProcAddress(m_hDll, "DestroyImporter"));
        ASSERT_NE(m_destroy, nullptr) << "DestroyImporter export not found";

        m_importer = create();
        ASSERT_NE(m_importer, nullptr);
    }

    void TearDown() override
    {
        if (m_importer && m_destroy) m_destroy(m_importer);
        if (m_hDll) FreeLibrary(m_hDll);
    }

    static std::string DataDir()
    {
        return std::string(CMAKE_TESTS_DATA_DIR) + "/";
    }
};

// ============================================================
// 导入 robot_arm.glb 并验证动画数据
// ============================================================

TEST_F(GltfAnimationTest, Importrobot_arm_HasAnimations)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "robot_arm.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;

    // 验证动画数据存在
    EXPECT_GT(scene.animations.size(), 0u);
}

TEST_F(GltfAnimationTest, Importrobot_arm_AnimationHasValidDuration)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "robot_arm.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GT(scene.animations.size(), 0u);

    for (const auto& clip : scene.animations)
    {
        double dur = clip.Duration();
        EXPECT_GT(dur, 0.0) << "Animation \"" << clip.name << "\" has zero or negative duration";
        EXPECT_GE(clip.endTime, clip.startTime);
    }
}

TEST_F(GltfAnimationTest, Importrobot_arm_AnimationHasKeyframes)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "robot_arm.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GT(scene.animations.size(), 0u);

    size_t totalKeyframes = 0;
    size_t totalChannels  = 0;

    for (const auto& clip : scene.animations)
    {
        totalChannels += clip.nodeChannels.size();
        totalChannels += clip.morphChannels.size();

        for (const auto& ch : clip.nodeChannels)
        {
            totalKeyframes += ch.translation.keys.size();
            totalKeyframes += ch.rotation.keys.size();
            totalKeyframes += ch.scale.keys.size();
        }
        for (const auto& ch : clip.morphChannels)
        {
            totalKeyframes += ch.weights.keys.size();
        }
    }

    EXPECT_GT(totalChannels, 0u)  << "No animation channels found";
    EXPECT_GT(totalKeyframes, 0u) << "No keyframes found";
}

TEST_F(GltfAnimationTest, Importrobot_arm_ValidatePassPasses)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "robot_arm.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GT(scene.animations.size(), 0u);

    mc::ValidatePass vp;
    auto vr = vp.Execute(scene);
    EXPECT_TRUE(vr.ok) << vr.error;
}

TEST_F(GltfAnimationTest, Importrobot_arm_AnimationNodeIdsValid)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "robot_arm.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GT(scene.animations.size(), 0u);

    // 验证所有动画通道引用的 nodeId 都存在于 scene.nodes 中
    for (const auto& clip : scene.animations)
    {
        for (const auto& ch : clip.nodeChannels)
        {
            EXPECT_NE(ch.nodeId, mc::INVALID_ID);

            bool found = false;
            for (const auto& node : scene.nodes)
            {
                if (node.id == ch.nodeId) { found = true; break; }
            }
            EXPECT_TRUE(found) << "Animation references unknown NodeID " << ch.nodeId;
        }
    }
}

TEST_F(GltfAnimationTest, Importrobot_arm_KeyframeTimesInRange)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "robot_arm.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GT(scene.animations.size(), 0u);

    // 验证所有关键帧的时间戳在 clip 的 startTime..endTime 范围内
    for (const auto& clip : scene.animations)
    {
        for (const auto& ch : clip.nodeChannels)
        {
            for (const auto& kf : ch.translation.keys)
            {
                EXPECT_GE(kf.time, clip.startTime - 1e-6);
                EXPECT_LE(kf.time, clip.endTime + 1e-6);
            }
            for (const auto& kf : ch.rotation.keys)
            {
                EXPECT_GE(kf.time, clip.startTime - 1e-6);
                EXPECT_LE(kf.time, clip.endTime + 1e-6);
            }
            for (const auto& kf : ch.scale.keys)
            {
                EXPECT_GE(kf.time, clip.startTime - 1e-6);
                EXPECT_LE(kf.time, clip.endTime + 1e-6);
            }
        }
    }
}

// ============================================================
// 导入无动画的 GLB（DamagedHelmet.glb）验证不会崩溃
// ============================================================

TEST_F(GltfAnimationTest, ImportDamagedHelmet_NoAnimations_DoesNotCrash)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "DamagedHelmet.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;

    // DamagedHelmet 没有动画，这应该正常
    // 不崩溃即为通过
    mc::ValidatePass vp;
    auto vr = vp.Execute(scene);
    EXPECT_TRUE(vr.ok) << vr.error;
}
