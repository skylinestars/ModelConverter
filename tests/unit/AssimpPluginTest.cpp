// ============================================================
// Phase06 AssimpPlugin 验收测试
// ============================================================
// 完全走插件边界：LoadLibrary → CreateImporter → IImporter::Import()
// 不直接链接 assimp 或 AssimpSceneConverter。
//
// 验收标准：
//   导入 cube.obj  → meshCount == 1，ValidatePass 通过
//   导入 cube.stl  → vertexCount > 0，ValidatePass 通过

#include <gtest/gtest.h>
#include "mc/pipeline/ValidatePass.h"
#include "mc/core/Scene.h"
#include "mc/importer/IImporter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);

// ============================================================
// Fixture：加载/卸载 Plugin_Assimp.dll
// ============================================================
class AssimpPluginTest : public ::testing::Test
{
protected:
    HMODULE         m_hDll     = nullptr;
    mc::IImporter*  m_importer = nullptr;
    DestroyImporterFunc m_destroy = nullptr;

    void SetUp() override
    {
        m_hDll = LoadLibraryA(PLUGIN_ASSIMP_FILENAME);
        ASSERT_NE(m_hDll, nullptr)
            << "Cannot load " PLUGIN_ASSIMP_FILENAME ". GetLastError=" << GetLastError();

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
        if (m_importer && m_destroy)
            m_destroy(m_importer);
        if (m_hDll)
            FreeLibrary(m_hDll);
    }

    static std::string DataDir()
    {
        return std::string(CMAKE_TESTS_DATA_DIR) + "/";
    }
};

// ============================================================
// 验收：导入 cube.obj
// ============================================================

TEST_F(AssimpPluginTest, ImportCubeObj_MeshCountIsOne)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "cube.obj", scene);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(scene.MeshCount(), 1u);
}

TEST_F(AssimpPluginTest, ImportCubeObj_VertexCountPositive)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "cube.obj", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GE(scene.MeshCount(), 1u);
    EXPECT_GT(scene.meshes[0].positions.size(), 0u);
}

TEST_F(AssimpPluginTest, ImportCubeObj_ValidatePassPasses)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "cube.obj", scene);
    ASSERT_TRUE(r.ok) << r.error;

    mc::ValidatePass vp;
    auto vr = vp.Execute(scene);
    EXPECT_TRUE(vr.ok) << vr.error;
}

// ============================================================
// 验收：导入 cube.stl
// ============================================================

TEST_F(AssimpPluginTest, ImportCubeStl_VertexCountPositive)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "cube.stl", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GE(scene.MeshCount(), 1u);
    EXPECT_GT(scene.meshes[0].positions.size(), 0u);
}

TEST_F(AssimpPluginTest, ImportCubeStl_ValidatePassPasses)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "cube.stl", scene);
    ASSERT_TRUE(r.ok) << r.error;

    mc::ValidatePass vp;
    auto vr = vp.Execute(scene);
    EXPECT_TRUE(vr.ok) << vr.error;
}

// ============================================================
// CanImport 格式判断
// ============================================================

TEST_F(AssimpPluginTest, CanImport_SupportedFormats)
{
    EXPECT_TRUE(m_importer->CanImport(".obj"));
    EXPECT_TRUE(m_importer->CanImport(".stl"));
    EXPECT_TRUE(m_importer->CanImport(".dae"));
    EXPECT_TRUE(m_importer->CanImport(".3ds"));
    EXPECT_TRUE(m_importer->CanImport(".ply"));
    EXPECT_TRUE(m_importer->CanImport(".x"));
}

TEST_F(AssimpPluginTest, CanImport_UnsupportedFormats)
{
    EXPECT_FALSE(m_importer->CanImport(".fbx"));
    EXPECT_FALSE(m_importer->CanImport(".gltf"));
    EXPECT_FALSE(m_importer->CanImport(".glb"));
}

// ============================================================
// 错误路径：不存在的文件
// ============================================================

TEST_F(AssimpPluginTest, ImportNonexistentFile_Fails)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "nonexistent_12345.obj", scene);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}
