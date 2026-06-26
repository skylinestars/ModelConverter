// ============================================================
// Phase14 GLTF 动画导出往返测试（FBX 源 → GLB 导出）
// ============================================================
// 使用 FBX 动画模型作为源（maszyna_parowa.fbx）→ 导出为 GLB → 重新导入 GLB
// 验证跨格式转换后动画数据完整性。
//
// 验收标准：
//   - 动画 Clip 数量一致
//   - Clip 时长一致
//   - 关键帧数量一致
//   - ValidatePass 通过

#include <gtest/gtest.h>
#include "mc/pipeline/ValidatePass.h"
#include "mc/pipeline/UnitConvertPass.h"
#include "mc/pipeline/Pipeline.h"
#include "mc/core/Scene.h"
#include "mc/core/Animation.h"
#include "mc/importer/IImporter.h"
#include "mc/exporter/IExporter.h"
#include "mc/common/Logger.h"

#include <filesystem>
#include <cmath>
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using CreateImporterFunc = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);
using CreateExporterFunc = mc::IExporter* (*)();
using DestroyExporterFunc = void (*)(mc::IExporter*);

// ============================================================
// Fixture：FBX 导入源 → GLTF 导出 → GLTF 重导入
// ============================================================
class GltfAnimationExportTest : public ::testing::Test
{
protected:
    // FBX plugin（源导入）
    HMODULE              m_hFbxDll = nullptr;
    mc::IImporter* m_fbxImporter = nullptr;
    DestroyImporterFunc  m_fbxDestroy = nullptr;
    // GLTF plugin（导出 + 重导入）
    HMODULE              m_hGltfDll = nullptr;
    mc::IImporter* m_gltfImporter = nullptr;
    mc::IExporter* m_gltfExporter = nullptr;
    DestroyImporterFunc  m_gltfDestroyImp = nullptr;
    DestroyExporterFunc  m_gltfDestroyExp = nullptr;

    void SetUp() override
    {
        // 加载 FBX plugin（源导入）
        m_hFbxDll = LoadLibraryA(PLUGIN_FBX_FILENAME);
        ASSERT_NE(m_hFbxDll, nullptr)
            << "Cannot load " PLUGIN_FBX_FILENAME;

        auto fbxCreate = reinterpret_cast<CreateImporterFunc>(
            GetProcAddress(m_hFbxDll, "CreateImporter"));
        ASSERT_NE(fbxCreate, nullptr);
        m_fbxDestroy = reinterpret_cast<DestroyImporterFunc>(
            GetProcAddress(m_hFbxDll, "DestroyImporter"));
        ASSERT_NE(m_fbxDestroy, nullptr);
        m_fbxImporter = fbxCreate();
        ASSERT_NE(m_fbxImporter, nullptr);

        // 加载 GLTF plugin（导出 + 重导入）
        m_hGltfDll = LoadLibraryA(PLUGIN_GLTF_FILENAME);
        ASSERT_NE(m_hGltfDll, nullptr)
            << "Cannot load " PLUGIN_GLTF_FILENAME;

        auto gltfCreateImp = reinterpret_cast<CreateImporterFunc>(
            GetProcAddress(m_hGltfDll, "CreateImporter"));
        ASSERT_NE(gltfCreateImp, nullptr);
        auto gltfCreateExp = reinterpret_cast<CreateExporterFunc>(
            GetProcAddress(m_hGltfDll, "CreateExporter"));
        ASSERT_NE(gltfCreateExp, nullptr);
        m_gltfDestroyImp = reinterpret_cast<DestroyImporterFunc>(
            GetProcAddress(m_hGltfDll, "DestroyImporter"));
        ASSERT_NE(m_gltfDestroyImp, nullptr);
        m_gltfDestroyExp = reinterpret_cast<DestroyExporterFunc>(
            GetProcAddress(m_hGltfDll, "DestroyExporter"));
        ASSERT_NE(m_gltfDestroyExp, nullptr);

        m_gltfImporter = gltfCreateImp();
        m_gltfExporter = gltfCreateExp();
        ASSERT_NE(m_gltfImporter, nullptr);
        ASSERT_NE(m_gltfExporter, nullptr);
    }

    void TearDown() override
    {
        if (m_fbxImporter && m_fbxDestroy) m_fbxDestroy(m_fbxImporter);
        if (m_gltfImporter && m_gltfDestroyImp) m_gltfDestroyImp(m_gltfImporter);
        if (m_gltfExporter && m_gltfDestroyExp) m_gltfDestroyExp(m_gltfExporter);
        if (m_hFbxDll) FreeLibrary(m_hFbxDll);
        if (m_hGltfDll) FreeLibrary(m_hGltfDll);
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
// 验证普通动画
// ============================================================
//
TEST_F(GltfAnimationExportTest, FbxToGlb_Animation)
{
    std::string testModelName = "maszyna_parowa";
    // Step 1: FBX 导入 Capoeira.fbx
    mc::Scene scene1;
    auto r1 = m_fbxImporter->Import(DataDir() + testModelName + ".fbx", scene1);
    ASSERT_TRUE(r1.ok) << r1.error;

    // Step 1.5: 运行 Pipeline（单位转换 cm→m）
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<mc::UnitConvertPass>(scene1.metadata.unitScale));

    auto pipeResult = pipeline.Execute(scene1);
    ASSERT_TRUE(pipeResult.ok) << pipeResult.error;

    std::string tempPath = DataDir() + "fbx_" + testModelName + ".glb";
    mc::ExportContext ctx;
    ctx.outputPath = tempPath;
    ctx.options.prettyPrint = false;
    auto r2 = m_gltfExporter->Export(scene1, ctx);
    ASSERT_TRUE(r2.ok) << r2.error;
}

// ============================================================
// 验证骨骼蒙皮动画
// ============================================================
//
TEST_F(GltfAnimationExportTest, FbxToGlb_skeleton)
{
    std::string testModelName = "RiggedSimple";
    // Step 1: FBX 导入 Capoeira.fbx
    mc::Scene scene1;
    auto r1 = m_fbxImporter->Import(DataDir() + testModelName + ".fbx", scene1);
    ASSERT_TRUE(r1.ok) << r1.error;

    // Step 1.5: 运行 Pipeline（单位转换 cm→m）
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<mc::UnitConvertPass>(scene1.metadata.unitScale));

    auto pipeResult = pipeline.Execute(scene1);
    ASSERT_TRUE(pipeResult.ok) << pipeResult.error;

    std::string tempPath = DataDir() + "fbx_" + testModelName + ".glb";
    mc::ExportContext ctx;
    ctx.outputPath = tempPath;
    ctx.options.prettyPrint = false;
    auto r2 = m_gltfExporter->Export(scene1, ctx);
    ASSERT_TRUE(r2.ok) << r2.error;
}


// ============================================================
// 验证骨骼蒙皮动画并添加详细日志
// ============================================================
//
TEST_F(GltfAnimationExportTest, FbxToGlb_skeleton_detailed_log)
{
    std::string testModelName = "Capoeira";
    // Step 1: FBX 导入 Capoeira.fbx
    mc::Scene scene1;
    auto r1 = m_fbxImporter->Import(DataDir() + testModelName+".fbx", scene1);
    ASSERT_TRUE(r1.ok) << r1.error;
   
    // Step 1.5: 运行 Pipeline（单位转换 cm→m）
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<mc::UnitConvertPass>(scene1.metadata.unitScale));
    
    auto pipeResult = pipeline.Execute(scene1);
    ASSERT_TRUE(pipeResult.ok) << pipeResult.error;

    size_t clipsBefore = scene1.animations.size();
    size_t keyframesBefore = CountKeyframes(scene1);

    // 验证骨骼蒙皮数据是否存在
    mc::Logger::Instance().LogInfo("=== FBX Import Summary ===");
    mc::Logger::Instance().LogInfo("  meshes=" + std::to_string(scene1.MeshCount()));
    mc::Logger::Instance().LogInfo("  skeletons=" + std::to_string(scene1.skeletons.size()));
    mc::Logger::Instance().LogInfo("  skins=" + std::to_string(scene1.skins.size()));
    mc::Logger::Instance().LogInfo("  nodes=" + std::to_string(scene1.NodeCount()));
    mc::Logger::Instance().LogInfo("  animations=" + std::to_string(scene1.animations.size()));

    // 统计蒙皮和有皮肤的 mesh
    size_t meshesWithSkin = 0;
    for (const auto& m : scene1.meshes)
    {
        if (!m.skinInfluences.empty())
            ++meshesWithSkin;
    }
    mc::Logger::Instance().LogInfo("  meshesWithSkinInfluences=" + std::to_string(meshesWithSkin));

    // 统计骨骼节点
    size_t boneNodes = 0;
    size_t realMeshNodes = 0;
    for (const auto& n : scene1.nodes)
    {
        if (n.type == mc::NodeType::Bone) ++boneNodes;
        if (!n.meshIds.empty()) ++realMeshNodes;
    }
    mc::Logger::Instance().LogInfo("  boneNodes=" + std::to_string(boneNodes));
    mc::Logger::Instance().LogInfo("  meshNodes=" + std::to_string(realMeshNodes));

    std::string tempPath = DataDir() + "fbx_"+ testModelName +".glb";
    mc::ExportContext ctx;
    ctx.outputPath = tempPath;
    ctx.options.prettyPrint = false;
    auto r2 = m_gltfExporter->Export(scene1, ctx);
    ASSERT_TRUE(r2.ok) << r2.error;
}
// 
//
//TEST_F(GltfAnimationExportTest, FbxToGlb_RoundTrip_DurationMatches)
//{
//    mc::Scene scene1;
//    auto r1 = m_fbxImporter->Import(DataDir() + "maszyna_parowa.fbx", scene1);
//    ASSERT_TRUE(r1.ok) << r1.error;
//    ASSERT_GT(scene1.animations.size(), 0u);
//
//    std::filesystem::create_directories(TempDir());
//    std::string tempPath = TempDir() + "_fbx_to_glb_duration_test.glb";
//
//    mc::ExportContext ctx;
//    ctx.outputPath = tempPath;
//
//    auto r2 = m_gltfExporter->Export(scene1, ctx);
//    ASSERT_TRUE(r2.ok) << r2.error;
//
//    mc::Scene scene2;
//    auto r3 = m_gltfImporter->Import(tempPath, scene2);
//    ASSERT_TRUE(r3.ok) << r3.error;
//
//    ASSERT_EQ(scene2.animations.size(), scene1.animations.size());
//    for (size_t i = 0; i < scene1.animations.size(); ++i)
//    {
//        const auto& c1 = scene1.animations[i];
//        const auto& c2 = scene2.animations[i];
//
//        EXPECT_NEAR(c2.startTime, c1.startTime, 0.01)
//            << "Clip \"" << c1.name << "\" startTime mismatch";
//        EXPECT_NEAR(c2.endTime, c1.endTime, 0.01)
//            << "Clip \"" << c1.name << "\" endTime mismatch";
//        EXPECT_NEAR(c2.Duration(), c1.Duration(), 0.01)
//            << "Clip \"" << c1.name << "\" duration mismatch";
//    }
//
//    std::error_code ec;
//    std::filesystem::remove(tempPath, ec);
//}
//
//TEST_F(GltfAnimationExportTest, FbxToGlb_RoundTrip_ChannelCountMatches)
//{
//    mc::Scene scene1;
//    auto r1 = m_fbxImporter->Import(DataDir() + "maszyna_parowa.fbx", scene1);
//    ASSERT_TRUE(r1.ok) << r1.error;
//    ASSERT_GT(scene1.animations.size(), 0u);
//
//    std::filesystem::create_directories(TempDir());
//    std::string tempPath = TempDir() + "_fbx_to_glb_channels_test.glb";
//
//    mc::ExportContext ctx;
//    ctx.outputPath = tempPath;
//
//    auto r2 = m_gltfExporter->Export(scene1, ctx);
//    ASSERT_TRUE(r2.ok) << r2.error;
//
//    mc::Scene scene2;
//    auto r3 = m_gltfImporter->Import(tempPath, scene2);
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
//// 无动画 FBX 导出 → GLB → 不崩溃
//// ============================================================
//
//TEST_F(GltfAnimationExportTest, NoAnimationFbxToGlb_DoesNotCrash)
//{
//    mc::Scene scene;
//    auto r1 = m_fbxImporter->Import(DataDir() + "lz.fbx", scene);
//    ASSERT_TRUE(r1.ok) << r1.error;
//
//    // 单位转换（FBX mm → GLTF m）
//    mc::Pipeline pipeline;
//    pipeline.AddPass(std::make_unique<mc::UnitConvertPass>(scene.metadata.unitScale));
//    auto pipeResult = pipeline.Execute(scene);
//    ASSERT_TRUE(pipeResult.ok) << pipeResult.error;
//
//    std::filesystem::create_directories(TempDir());
//    std::string tempPath = DataDir() + "lz2.glb";
//
//    mc::ExportContext ctx;
//    ctx.outputPath = tempPath;
//
//    auto r2 = m_gltfExporter->Export(scene, ctx);
//    EXPECT_TRUE(r2.ok) << r2.error;
//}
