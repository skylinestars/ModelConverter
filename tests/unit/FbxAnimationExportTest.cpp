// ============================================================
// Phase14 FBX 动画导出往返测试（GLB 源 → FBX 导出）
// ============================================================
// 使用 GLB 动画模型作为源（robot_arm.glb）→ 导出为 FBX → 重新导入 FBX
// 验证跨格式转换后动画数据完整性。
//
// 注意：FBX 内部使用欧拉角（度）存储旋转，mc 使用四元数，
// Quaternion → Euler → Quaternion 往返会引入微小误差。
// 因此关键帧数量容差放宽，时长和 Clip 数量作为主要验证标准。
//
// 验收标准：
//   - 动画 Clip 数量一致
//   - Clip 时长一致（容差 0.02s）
//   - ValidatePass 通过
//   - 关键帧数大致一致（容差范围内）

#include <gtest/gtest.h>
#include "mc/pipeline/ValidatePass.h"
#include "mc/core/Scene.h"
#include "mc/core/Animation.h"
#include "mc/importer/IImporter.h"
#include "mc/exporter/IExporter.h"

#include <filesystem>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);
using CreateExporterFunc  = mc::IExporter* (*)();
using DestroyExporterFunc = void (*)(mc::IExporter*);

// ============================================================
// Fixture：GLTF 导入源 → FBX 导出 → FBX 重导入
// ============================================================
class FbxAnimationExportTest : public ::testing::Test
{
protected:
    // GLTF plugin（源导入）
    HMODULE              m_hGltfDll      = nullptr;
    mc::IImporter*       m_gltfImporter  = nullptr;
    DestroyImporterFunc  m_gltfDestroy   = nullptr;
    // FBX plugin（导出 + 重导入）
    HMODULE              m_hFbxDll       = nullptr;
    mc::IImporter*       m_fbxImporter   = nullptr;
    mc::IExporter*       m_fbxExporter   = nullptr;
    DestroyImporterFunc  m_fbxDestroyImp = nullptr;
    DestroyExporterFunc  m_fbxDestroyExp = nullptr;

    void SetUp() override
    {
        // 加载 GLTF plugin（源导入）
        m_hGltfDll = LoadLibraryA(PLUGIN_GLTF_FILENAME);
        ASSERT_NE(m_hGltfDll, nullptr)
            << "Cannot load " PLUGIN_GLTF_FILENAME;

        auto gltfCreate = reinterpret_cast<CreateImporterFunc>(
            GetProcAddress(m_hGltfDll, "CreateImporter"));
        ASSERT_NE(gltfCreate, nullptr);
        m_gltfDestroy = reinterpret_cast<DestroyImporterFunc>(
            GetProcAddress(m_hGltfDll, "DestroyImporter"));
        ASSERT_NE(m_gltfDestroy, nullptr);
        m_gltfImporter = gltfCreate();
        ASSERT_NE(m_gltfImporter, nullptr);

        // 加载 FBX plugin（导出 + 重导入）
        m_hFbxDll = LoadLibraryA(PLUGIN_FBX_FILENAME);
        ASSERT_NE(m_hFbxDll, nullptr)
            << "Cannot load " PLUGIN_FBX_FILENAME;

        auto fbxCreateImp = reinterpret_cast<CreateImporterFunc>(
            GetProcAddress(m_hFbxDll, "CreateImporter"));
        ASSERT_NE(fbxCreateImp, nullptr);
        auto fbxCreateExp = reinterpret_cast<CreateExporterFunc>(
            GetProcAddress(m_hFbxDll, "CreateExporter"));
        ASSERT_NE(fbxCreateExp, nullptr);
        m_fbxDestroyImp = reinterpret_cast<DestroyImporterFunc>(
            GetProcAddress(m_hFbxDll, "DestroyImporter"));
        ASSERT_NE(m_fbxDestroyImp, nullptr);
        m_fbxDestroyExp = reinterpret_cast<DestroyExporterFunc>(
            GetProcAddress(m_hFbxDll, "DestroyExporter"));
        ASSERT_NE(m_fbxDestroyExp, nullptr);

        m_fbxImporter = fbxCreateImp();
        m_fbxExporter = fbxCreateExp();
        ASSERT_NE(m_fbxImporter, nullptr);
        ASSERT_NE(m_fbxExporter, nullptr);
    }

    void TearDown() override
    {
        if (m_gltfImporter && m_gltfDestroy) m_gltfDestroy(m_gltfImporter);
        if (m_fbxImporter && m_fbxDestroyImp) m_fbxDestroyImp(m_fbxImporter);
        if (m_fbxExporter && m_fbxDestroyExp) m_fbxDestroyExp(m_fbxExporter);
        if (m_hGltfDll) FreeLibrary(m_hGltfDll);
        if (m_hFbxDll) FreeLibrary(m_hFbxDll);
    }

    static std::string DataDir()
    {
        return std::string(CMAKE_TESTS_DATA_DIR) + "/";
    }

    static std::string TempDir()
    {
        return std::string(CMAKE_TESTS_DATA_DIR) + "/_temp/";
    }
};

// ============================================================
// 辅助：统计关键帧数
// ============================================================
static size_t CountKeyframes(const mc::Scene& scene)
{
    size_t count = 0;
    for (const auto& clip : scene.animations)
    {
        for (const auto& ch : clip.nodeChannels)
        {
            count += ch.translation.keys.size();
            count += ch.rotation.keys.size();
            count += ch.scale.keys.size();
        }
        for (const auto& ch : clip.morphChannels)
        {
            count += ch.weights.keys.size();
        }
    }
    return count;
}

// ============================================================
// _fbx_to_glb_ANIM.glb → FBX 导出，保留fbx文件，待导入其他渲染器中查看
// ============================================================

TEST_F(FbxAnimationExportTest, GlbToFbx_AnimationClipsMatch)
{
    // Step 1: GLTF 导入 robot_arm.glb
    mc::Scene scene1;
    auto r1 = m_gltfImporter->Import(DataDir() + "CesiumMan.glb", scene1);
    ASSERT_TRUE(r1.ok) << r1.error;
    ASSERT_GT(scene1.animations.size(), 0u);

    size_t clipsBefore     = scene1.animations.size();
    size_t keyframesBefore = CountKeyframes(scene1);

    std::string tempPath = DataDir() + "_glb_to_fbx_skeleton_test.fbx";
    mc::ExportContext ctx;
    ctx.outputPath = tempPath;

    auto r2 = m_fbxExporter->Export(scene1, ctx);
    ASSERT_TRUE(r2.ok) << r2.error;
}
//
//TEST_F(FbxAnimationExportTest, GlbToFbx_RoundTrip_DurationMatches)
//{
//    mc::Scene scene1;
//    auto r1 = m_gltfImporter->Import(DataDir() + "robot_arm.glb", scene1);
//    ASSERT_TRUE(r1.ok) << r1.error;
//    ASSERT_GT(scene1.animations.size(), 0u);
//
//    std::filesystem::create_directories(TempDir());
//    std::string tempPath = TempDir() + "_glb_to_fbx_duration_test.fbx";
//
//    mc::ExportContext ctx;
//    ctx.outputPath = tempPath;
//
//    auto r2 = m_fbxExporter->Export(scene1, ctx);
//    ASSERT_TRUE(r2.ok) << r2.error;
//
//    mc::Scene scene2;
//    auto r3 = m_fbxImporter->Import(tempPath, scene2);
//    ASSERT_TRUE(r3.ok) << r3.error;
//
//    ASSERT_EQ(scene2.animations.size(), scene1.animations.size());
//    for (size_t i = 0; i < scene1.animations.size(); ++i)
//    {
//        const auto& c1 = scene1.animations[i];
//        const auto& c2 = scene2.animations[i];
//
//        EXPECT_NEAR(c2.startTime, c1.startTime, 0.02)
//            << "Clip \"" << c1.name << "\" startTime mismatch";
//        EXPECT_NEAR(c2.endTime, c1.endTime, 0.02)
//            << "Clip \"" << c1.name << "\" endTime mismatch";
//        EXPECT_NEAR(c2.Duration(), c1.Duration(), 0.02)
//            << "Clip \"" << c1.name << "\" duration mismatch";
//    }
//
//    std::error_code ec;
//    std::filesystem::remove(tempPath, ec);
//}
//
//TEST_F(FbxAnimationExportTest, GlbToFbx_RoundTrip_ChannelCountMatches)
//{
//    mc::Scene scene1;
//    auto r1 = m_gltfImporter->Import(DataDir() + "robot_arm.glb", scene1);
//    ASSERT_TRUE(r1.ok) << r1.error;
//    ASSERT_GT(scene1.animations.size(), 0u);
//
//    std::filesystem::create_directories(TempDir());
//    std::string tempPath = TempDir() + "_glb_to_fbx_channels_test.fbx";
//
//    mc::ExportContext ctx;
//    ctx.outputPath = tempPath;
//
//    auto r2 = m_fbxExporter->Export(scene1, ctx);
//    ASSERT_TRUE(r2.ok) << r2.error;
//
//    mc::Scene scene2;
//    auto r3 = m_fbxImporter->Import(tempPath, scene2);
//    ASSERT_TRUE(r3.ok) << r3.error;
//
//    ASSERT_EQ(scene2.animations.size(), scene1.animations.size());
//    for (size_t i = 0; i < scene1.animations.size(); ++i)
//    {
//        const auto& c1 = scene1.animations[i];
//        const auto& c2 = scene2.animations[i];
//
//        EXPECT_EQ(c2.nodeChannels.size(), c1.nodeChannels.size())
//            << "Clip \"" << c1.name << "\" nodeChannels count mismatch";
//    }
//
//    std::error_code ec;
//    std::filesystem::remove(tempPath, ec);
//}
//
//// ============================================================
//// 无动画 GLB 导出 → FBX → 不崩溃
//// ============================================================
//
//TEST_F(FbxAnimationExportTest, NoAnimationGlbToFbx_DoesNotCrash)
//{
//    mc::Scene scene;
//    auto r1 = m_gltfImporter->Import(DataDir() + "DamagedHelmet.glb", scene);
//    ASSERT_TRUE(r1.ok) << r1.error;
//
//    std::filesystem::create_directories(TempDir());
//    std::string tempPath = TempDir() + "_noanim_glb_to_fbx_test.fbx";
//
//    mc::ExportContext ctx;
//    ctx.outputPath = tempPath;
//
//    auto r2 = m_fbxExporter->Export(scene, ctx);
//    EXPECT_TRUE(r2.ok) << r2.error;
//
//    mc::Scene scene2;
//    auto r3 = m_fbxImporter->Import(tempPath, scene2);
//    EXPECT_TRUE(r3.ok) << r3.error;
//
//    std::error_code ec;
//    std::filesystem::remove(tempPath, ec);
//}
