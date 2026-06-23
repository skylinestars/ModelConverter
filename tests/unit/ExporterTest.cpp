// ============================================================
// Phase10 Exporter框架验收测试
// ============================================================
// 完全走插件边界：LoadLibrary → CreateExporter → IExporter::Export()
//
// 验收标准：
//   DummyExporter 工作正常
//   将 Scene 基本信息输出到日志
//   outputPath 为空时返回错误

#include <gtest/gtest.h>
#include "mc/exporter/IExporter.h"
#include "mc/core/Scene.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using CreateExporterFunc  = mc::IExporter* (*)();
using DestroyExporterFunc = void (*)(mc::IExporter*);

// ============================================================
// Fixture
// ============================================================
class ExporterTest : public ::testing::Test
{
protected:
    HMODULE             m_hDll     = nullptr;
    mc::IExporter*      m_exporter = nullptr;
    DestroyExporterFunc m_destroy  = nullptr;

    void SetUp() override
    {
        m_hDll = LoadLibraryA(PLUGIN_DUMMY_EXPORTER_FILENAME);
        ASSERT_NE(m_hDll, nullptr)
            << "Cannot load " PLUGIN_DUMMY_EXPORTER_FILENAME
            << ". GetLastError=" << GetLastError();

        auto create = reinterpret_cast<CreateExporterFunc>(
            GetProcAddress(m_hDll, "CreateExporter"));
        ASSERT_NE(create, nullptr) << "CreateExporter export not found";

        m_destroy = reinterpret_cast<DestroyExporterFunc>(
            GetProcAddress(m_hDll, "DestroyExporter"));
        ASSERT_NE(m_destroy, nullptr) << "DestroyExporter export not found";

        m_exporter = create();
        ASSERT_NE(m_exporter, nullptr);
    }

    void TearDown() override
    {
        if (m_exporter && m_destroy) m_destroy(m_exporter);
        if (m_hDll) FreeLibrary(m_hDll);
    }

    // 构造一个最小有效 Scene（1节点，1网格，1材质）
    static mc::Scene MakeMinimalScene()
    {
        mc::Scene scene;
        auto& node = scene.AddNode();
        auto& mesh = scene.AddMesh();
        mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
        mesh.indices   = {0, 1, 2};
        mc::MeshSection sec; sec.indexOffset = 0; sec.indexCount = 3;
        mesh.sections.push_back(sec);
        node.meshIds.push_back(mesh.id);
        scene.rootNodes.push_back(node.id);
        auto& mat = scene.AddMaterial();
        (void)mat;
        return scene;
    }
};

// ============================================================
// CanExport
// ============================================================

TEST_F(ExporterTest, CanExport_DummyFormat)
{
    EXPECT_TRUE(m_exporter->CanExport(".dummy"));
    EXPECT_TRUE(m_exporter->CanExport(".DUMMY"));
}

TEST_F(ExporterTest, CanExport_Unsupported)
{
    EXPECT_FALSE(m_exporter->CanExport(".gltf"));
    EXPECT_FALSE(m_exporter->CanExport(".fbx"));
}

// ============================================================
// Export 成功路径
// ============================================================

TEST_F(ExporterTest, Export_Succeeds)
{
    mc::Scene scene = MakeMinimalScene();
    mc::ExportContext ctx;
    ctx.outputPath = "dummy_output.dummy";

    auto r = m_exporter->Export(scene, ctx);
    EXPECT_TRUE(r.ok) << r.error;
}

TEST_F(ExporterTest, Export_FillsContext)
{
    mc::Scene scene = MakeMinimalScene();
    mc::ExportContext ctx;
    ctx.outputPath = "dummy_output.dummy";

    m_exporter->Export(scene, ctx);
    EXPECT_EQ(ctx.nodesExported,     scene.NodeCount());
    EXPECT_EQ(ctx.meshesExported,    scene.MeshCount());
    EXPECT_EQ(ctx.materialsExported, scene.MaterialCount());
}

// ============================================================
// 错误路径：outputPath 为空
// ============================================================

TEST_F(ExporterTest, Export_EmptyPath_Fails)
{
    mc::Scene scene = MakeMinimalScene();
    mc::ExportContext ctx;
    ctx.outputPath = "";

    auto r = m_exporter->Export(scene, ctx);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}
