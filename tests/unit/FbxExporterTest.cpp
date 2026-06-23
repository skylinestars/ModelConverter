// ============================================================
// Phase12 FbxExporter 验收测试
// ============================================================
// 验收标准：
//   OBJ → Scene → FBX 导出成功（文件存在且非空）
//   导出的 FBX 可被 FBX SDK 重新读回
//   Mesh 数量一致，Material 数量一致

#include <gtest/gtest.h>
#include "mc/exporter/IExporter.h"
#include "mc/importer/IImporter.h"
#include "mc/core/Scene.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);
using CreateExporterFunc  = mc::IExporter* (*)();
using DestroyExporterFunc = void (*)(mc::IExporter*);

// ============================================================
// Fixture
// ============================================================
class FbxExporterTest : public ::testing::Test
{
protected:
    HMODULE             m_hGltf      = nullptr;
    mc::IImporter*      m_importer   = nullptr;
    DestroyImporterFunc m_destroyImp = nullptr;

    HMODULE             m_hFbx       = nullptr;
    mc::IExporter*      m_exporter   = nullptr;
    DestroyExporterFunc m_destroyExp = nullptr;
    mc::IImporter*      m_fbxImp     = nullptr;
    DestroyImporterFunc m_destroyFbxImp = nullptr;

    std::string m_tmpFbx;
    std::string m_viewFbx;

    void SetUp() override
    {
        m_hGltf = LoadLibraryA(PLUGIN_GLTF_FILENAME);
        ASSERT_NE(m_hGltf, nullptr) << "Cannot load " PLUGIN_GLTF_FILENAME;
        auto ci = reinterpret_cast<CreateImporterFunc>(GetProcAddress(m_hGltf, "CreateImporter"));
        ASSERT_NE(ci, nullptr);
        m_destroyImp = reinterpret_cast<DestroyImporterFunc>(GetProcAddress(m_hGltf, "DestroyImporter"));
        ASSERT_NE(m_destroyImp, nullptr);
        m_importer = ci();
        ASSERT_NE(m_importer, nullptr);

        m_hFbx = LoadLibraryA(PLUGIN_FBX_FILENAME);
        ASSERT_NE(m_hFbx, nullptr) << "Cannot load " PLUGIN_FBX_FILENAME;
        auto ce = reinterpret_cast<CreateExporterFunc>(GetProcAddress(m_hFbx, "CreateExporter"));
        ASSERT_NE(ce, nullptr);
        m_destroyExp = reinterpret_cast<DestroyExporterFunc>(GetProcAddress(m_hFbx, "DestroyExporter"));
        ASSERT_NE(m_destroyExp, nullptr);
        m_exporter = ce();
        ASSERT_NE(m_exporter, nullptr);

        auto cfi = reinterpret_cast<CreateImporterFunc>(GetProcAddress(m_hFbx, "CreateImporter"));
        ASSERT_NE(cfi, nullptr);
        m_destroyFbxImp = reinterpret_cast<DestroyImporterFunc>(GetProcAddress(m_hFbx, "DestroyImporter"));
        ASSERT_NE(m_destroyFbxImp, nullptr);
        m_fbxImp = cfi();
        ASSERT_NE(m_fbxImp, nullptr);

        m_tmpFbx = std::string(CMAKE_TESTS_DATA_DIR) + "/out_test_export.fbx";
        m_viewFbx = std::string(CMAKE_TESTS_DATA_DIR) + "/out_test_view.fbx";
    }

    void TearDown() override
    {
        if (m_importer    && m_destroyImp)    m_destroyImp(m_importer);
        if (m_exporter    && m_destroyExp)    m_destroyExp(m_exporter);
        if (m_fbxImp      && m_destroyFbxImp) m_destroyFbxImp(m_fbxImp);
        if (m_hGltf)  FreeLibrary(m_hGltf);
        if (m_hFbx)    FreeLibrary(m_hFbx);
        std::filesystem::remove(m_tmpFbx);
    }

    std::string DataDir() const { return std::string(CMAKE_TESTS_DATA_DIR) + "/"; }
};

// ============================================================
// CanExport
// ============================================================

//TEST_F(FbxExporterTest, CanExport_Fbx)
//{
//    EXPECT_TRUE(m_exporter->CanExport(".fbx"));
//    EXPECT_TRUE(m_exporter->CanExport(".FBX"));
//}
//
//TEST_F(FbxExporterTest, CanExport_Unsupported)
//{
//    EXPECT_FALSE(m_exporter->CanExport(".gltf"));
//    EXPECT_FALSE(m_exporter->CanExport(".obj"));
//}

// ============================================================
// OBJ → Scene → FBX
// ============================================================

TEST_F(FbxExporterTest, ExportCubeObj_ToFbx_Succeeds)
{
    mc::Scene scene;
    auto ir = m_importer->Import(DataDir() + "lz.glb", scene);
    ASSERT_TRUE(ir.ok) << ir.error;

    mc::ExportContext ctx;
    ctx.outputPath = m_viewFbx;
    auto er = m_exporter->Export(scene, ctx);
    EXPECT_TRUE(er.ok) << er.error;
    EXPECT_TRUE(std::filesystem::exists(m_viewFbx));
}
//
//TEST_F(FbxExporterTest, ExportCubeObj_ToFbx_FileNonEmpty)
//{
//    mc::Scene scene;
//    m_importer->Import(DataDir() + "cube.obj", scene);
//
//    mc::ExportContext ctx;
//    ctx.outputPath = m_tmpFbx;
//    m_exporter->Export(scene, ctx);
//    EXPECT_GT(std::filesystem::file_size(m_tmpFbx), 0u);
//}
//
//TEST_F(FbxExporterTest, ExportCubeObj_MeshCountConsistent)
//{
//    mc::Scene srcScene;
//    m_importer->Import(DataDir() + "cube.obj", srcScene);
//    size_t srcMeshCount = srcScene.MeshCount();
//
//    mc::ExportContext ctx;
//    ctx.outputPath = m_tmpFbx;
//    auto er = m_exporter->Export(srcScene, ctx);
//    ASSERT_TRUE(er.ok) << er.error;
//
//    mc::Scene dstScene;
//    auto ir = m_fbxImp->Import(m_tmpFbx, dstScene);
//    ASSERT_TRUE(ir.ok) << ir.error;
//    EXPECT_EQ(dstScene.MeshCount(), srcMeshCount);
//}
//
//// ============================================================
//// 错误路径：outputPath 为空
//// ============================================================
//
//TEST_F(FbxExporterTest, Export_EmptyPath_Fails)
//{
//    mc::Scene scene;
//    m_importer->Import(DataDir() + "cube.obj", scene);
//
//    mc::ExportContext ctx;
//    ctx.outputPath = "";
//    auto er = m_exporter->Export(scene, ctx);
//    EXPECT_FALSE(er.ok);
//    EXPECT_FALSE(er.error.empty());
//}
