// ============================================================
// Phase08 GltfPlugin 验收测试
// ============================================================
// 完全走插件边界：LoadLibrary → CreateImporter → IImporter::Import()
//
// 验收标准：
//   导入 CesiumMan.glb → meshCount >= 1，ValidatePass 通过
//   导入 triangle.gltf → meshCount >= 1，ValidatePass 通过
//   CanImport(".gltf"/".glb") == true
//   CanImport(".obj") == false

#include <gtest/gtest.h>
#include "mc/pipeline/ValidatePass.h"
#include "mc/core/Scene.h"
#include "mc/importer/IImporter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);

// ============================================================
// Fixture
// ============================================================
class GltfPluginTest : public ::testing::Test
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
// CanImport
// ============================================================

TEST_F(GltfPluginTest, CanImport_SupportedFormats)
{
    EXPECT_TRUE(m_importer->CanImport(".gltf"));
    EXPECT_TRUE(m_importer->CanImport(".glb"));
    EXPECT_TRUE(m_importer->CanImport(".GLTF"));
    EXPECT_TRUE(m_importer->CanImport(".GLB"));
}

TEST_F(GltfPluginTest, CanImport_UnsupportedFormats)
{
    EXPECT_FALSE(m_importer->CanImport(".obj"));
    EXPECT_FALSE(m_importer->CanImport(".fbx"));
    EXPECT_FALSE(m_importer->CanImport(".stl"));
}

// ============================================================
// 导入 CesiumMan.glb（最小 GLB，内嵌顶点）
// ============================================================

TEST_F(GltfPluginTest, ImportTriangleGlb_Succeeds)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "DamagedHelmet.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
}

TEST_F(GltfPluginTest, ImportTriangleGlb_MeshCountPositive)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "DamagedHelmet.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_GE(scene.MeshCount(), 1u);
}

TEST_F(GltfPluginTest, ImportTriangleGlb_VertexCountPositive)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "DamagedHelmet.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GE(scene.MeshCount(), 1u);
    EXPECT_GT(scene.meshes[0].positions.size(), 0u);
}

TEST_F(GltfPluginTest, ImportTriangleGlb_ValidatePassPasses)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "DamagedHelmet.glb", scene);
    ASSERT_TRUE(r.ok) << r.error;

    mc::ValidatePass vp;
    auto vr = vp.Execute(scene);
    EXPECT_TRUE(vr.ok) << vr.error;
}

// ============================================================
// 错误路径
// ============================================================

TEST_F(GltfPluginTest, ImportNonexistentFile_Fails)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "nonexistent_99999.glb", scene);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}
