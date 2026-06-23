// ============================================================
// Phase09 FbxPlugin 验收测试
// ============================================================
// 完全走插件边界：LoadLibrary → CreateImporter → IImporter::Import()
//
// 验收标准：
//   导入 elephant3.fbx → MeshCount >= 1，ValidatePass 通过
//   CanImport(".fbx") == true
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
class FbxPluginTest : public ::testing::Test
{
protected:
    HMODULE             m_hDll     = nullptr;
    mc::IImporter*      m_importer = nullptr;
    DestroyImporterFunc m_destroy  = nullptr;

    void SetUp() override
    {
        m_hDll = LoadLibraryA(PLUGIN_FBX_FILENAME);
        ASSERT_NE(m_hDll, nullptr)
            << "Cannot load " PLUGIN_FBX_FILENAME ". GetLastError=" << GetLastError();

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

TEST_F(FbxPluginTest, CanImport_Fbx)
{
    EXPECT_TRUE(m_importer->CanImport(".fbx"));
    EXPECT_TRUE(m_importer->CanImport(".FBX"));
}

TEST_F(FbxPluginTest, CanImport_Unsupported)
{
    EXPECT_FALSE(m_importer->CanImport(".obj"));
    EXPECT_FALSE(m_importer->CanImport(".gltf"));
    EXPECT_FALSE(m_importer->CanImport(".stl"));
}

// ============================================================
// 导入 elephant3.fbx（盆栽模型）
// ============================================================

TEST_F(FbxPluginTest, ImportLzFbx_Succeeds)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "lz.fbx", scene);
    ASSERT_TRUE(r.ok) << r.error;
}

TEST_F(FbxPluginTest, ImportLzFbx_MeshCountPositive)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "lz.fbx", scene);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_GE(scene.MeshCount(), 1u);
}

TEST_F(FbxPluginTest, ImportLzFbx_VertexCountPositive)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "lz.fbx", scene);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_GE(scene.MeshCount(), 1u);

    size_t totalVerts = 0;
    for (const auto& mesh : scene.meshes)
        totalVerts += mesh.positions.size();
    EXPECT_GT(totalVerts, 0u);
}

TEST_F(FbxPluginTest, ImportLzFbx_ValidatePassPasses)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "lz.fbx", scene);
    ASSERT_TRUE(r.ok) << r.error;

    mc::ValidatePass vp;
    auto vr = vp.Execute(scene);
    EXPECT_TRUE(vr.ok) << vr.error;
}

// ============================================================
// 错误路径
// ============================================================

TEST_F(FbxPluginTest, ImportNonexistentFile_Fails)
{
    mc::Scene scene;
    auto r = m_importer->Import(DataDir() + "nonexistent_99999.fbx", scene);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}
