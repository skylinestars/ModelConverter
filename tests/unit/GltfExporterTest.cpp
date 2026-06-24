// ============================================================
// Phase11 GltfExporter 验收测试
// ============================================================
// 导入：Plugin_Fbx（FBX SDK）读取 FBX/OBJ 替代 Assimp，避免 $AssimpFbx$ 中间节点
// 验收标准：
//   FBX → Scene → GLB 导出成功（文件存在且非空）
//   Mesh 数量一致，导出的 GLB 可被 tinygltf 重新读取

#include <gtest/gtest.h>
#include "mc/exporter/IExporter.h"
#include "mc/importer/IImporter.h"
#include "mc/core/Scene.h"
#include "mc/pipeline/UnitConvertPass.h"
#include "mc/pipeline/AxisConvertPass.h"
#include "mc/pipeline/HandednessFlipPass.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <unordered_map>

using CreateImporterFunc  = mc::IImporter* (*)();
using DestroyImporterFunc = void (*)(mc::IImporter*);
using CreateExporterFunc  = mc::IExporter* (*)();
using DestroyExporterFunc = void (*)(mc::IExporter*);

namespace {

mc::UpAxis ToUpAxis(mc::Axis axis)
{
    switch (axis)
    {
        case mc::Axis::X: return mc::UpAxis::X;
        case mc::Axis::Y: return mc::UpAxis::Y;
        case mc::Axis::Z: return mc::UpAxis::Z;
        default: return mc::UpAxis::Y;
    }
}

void NormalizeSceneForGltf(mc::Scene& scene)
{
    if (scene.metadata.unitScale > 0.0f &&
        std::abs(scene.metadata.unitScale - 1.0f) > 1e-6f)
    {
        mc::UnitConvertPass unit(scene.metadata.unitScale);
        auto ur = unit.Execute(scene);
        EXPECT_TRUE(ur.ok) << ur.error;
    }

    if (scene.metadata.upAxis != mc::Axis::Y)
    {
        mc::AxisConvertPass axis(ToUpAxis(scene.metadata.upAxis), mc::UpAxis::Y);
        auto ar = axis.Execute(scene);
        EXPECT_TRUE(ar.ok) << ar.error;
    }

    if (scene.metadata.handedness != mc::Handedness::Right)
    {
        mc::HandednessFlipPass hand(scene.metadata.handedness, mc::Handedness::Right);
        auto hr = hand.Execute(scene);
        EXPECT_TRUE(hr.ok) << hr.error;
    }
}

float Vec3Len(const mc::Vec3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float Vec3Dot(const mc::Vec3& a, const mc::Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

std::unordered_map<std::string, const mc::Node*> BuildNodeNameMap(const mc::Scene& scene)
{
    std::unordered_map<std::string, const mc::Node*> m;
    for (const auto& n : scene.nodes)
    {
        if (!n.name.empty() && m.find(n.name) == m.end())
            m[n.name] = &n;
    }
    return m;
}

} // namespace

// ============================================================
// Fixture：Plugin_Fbx（FBX SDK 导入）+ Plugin_Gltf（导出+导入）
// ============================================================
class GltfExporterTest : public ::testing::Test
{
protected:
    // FBX importer (Plugin_Fbx)
    HMODULE             m_hFbx        = nullptr;
    mc::IImporter*      m_fbxImporter = nullptr;
    DestroyImporterFunc m_destroyFbxImp = nullptr;

    // Gltf exporter + importer (Plugin_Gltf)
    HMODULE             m_hGltf          = nullptr;
    mc::IExporter*      m_exporter       = nullptr;
    DestroyExporterFunc m_destroyExp     = nullptr;
    mc::IImporter*      m_gltfImporter   = nullptr;
    DestroyImporterFunc m_destroyGltfImp = nullptr;

    std::string m_tmpGlb;
    std::string m_tmpGltf;
    std::string m_outGlb;

    void SetUp() override
    {
        // --- Load Plugin_Fbx (FBX SDK importer) ---
        m_hFbx = LoadLibraryA(PLUGIN_FBX_FILENAME);
        ASSERT_NE(m_hFbx, nullptr)
            << "Cannot load " PLUGIN_FBX_FILENAME << ". err=" << GetLastError();
        auto cfi = reinterpret_cast<CreateImporterFunc>(GetProcAddress(m_hFbx, "CreateImporter"));
        ASSERT_NE(cfi, nullptr);
        m_destroyFbxImp = reinterpret_cast<DestroyImporterFunc>(GetProcAddress(m_hFbx, "DestroyImporter"));
        ASSERT_NE(m_destroyFbxImp, nullptr);
        m_fbxImporter = cfi();
        ASSERT_NE(m_fbxImporter, nullptr);

        // --- Load Plugin_Gltf (exporter + importer) ---
        m_hGltf = LoadLibraryA(PLUGIN_GLTF_FILENAME);
        ASSERT_NE(m_hGltf, nullptr)
            << "Cannot load " PLUGIN_GLTF_FILENAME << ". err=" << GetLastError();
        auto ce = reinterpret_cast<CreateExporterFunc>(GetProcAddress(m_hGltf, "CreateExporter"));
        ASSERT_NE(ce, nullptr);
        m_destroyExp = reinterpret_cast<DestroyExporterFunc>(GetProcAddress(m_hGltf, "DestroyExporter"));
        ASSERT_NE(m_destroyExp, nullptr);
        m_exporter = ce();
        ASSERT_NE(m_exporter, nullptr);

        auto cgi = reinterpret_cast<CreateImporterFunc>(GetProcAddress(m_hGltf, "CreateImporter"));
        ASSERT_NE(cgi, nullptr);
        m_destroyGltfImp = reinterpret_cast<DestroyImporterFunc>(GetProcAddress(m_hGltf, "DestroyImporter"));
        ASSERT_NE(m_destroyGltfImp, nullptr);
        m_gltfImporter = cgi();
        ASSERT_NE(m_gltfImporter, nullptr);

        m_tmpGlb = std::string(CMAKE_TESTS_DATA_DIR) + "/out_test.glb";
        m_tmpGltf = std::string(CMAKE_TESTS_DATA_DIR) + "/out_test.gltf";
        m_outGlb  = std::string(CMAKE_TESTS_DATA_DIR) + "/out.glb";
    }

    void TearDown() override
    {
        if (m_fbxImporter  && m_destroyFbxImp)  m_destroyFbxImp(m_fbxImporter);
        if (m_exporter     && m_destroyExp)      m_destroyExp(m_exporter);
        if (m_gltfImporter && m_destroyGltfImp) m_destroyGltfImp(m_gltfImporter);
        if (m_hFbx)  FreeLibrary(m_hFbx);
        if (m_hGltf) FreeLibrary(m_hGltf);
        std::filesystem::remove(m_tmpGlb);
        std::filesystem::remove(m_tmpGltf);
        std::filesystem::remove(std::string(CMAKE_TESTS_DATA_DIR) + "/out_test.bin");
    }

    static std::string DataDir() { return std::string(CMAKE_TESTS_DATA_DIR) + "/"; }
};
//
//// ============================================================
//// CanExport
//// ============================================================
//
//TEST_F(GltfExporterTest, CanExport_Gltf)
//{
//    EXPECT_TRUE(m_exporter->CanExport(".gltf"));
//    EXPECT_TRUE(m_exporter->CanExport(".glb"));
//    EXPECT_TRUE(m_exporter->CanExport(".GLTF"));
//}
//
//TEST_F(GltfExporterTest, CanExport_Unsupported)
//{
//    EXPECT_FALSE(m_exporter->CanExport(".obj"));
//    EXPECT_FALSE(m_exporter->CanExport(".fbx"));
//}

// ============================================================
// FBX → Scene → GLB
// ============================================================

TEST_F(GltfExporterTest, ExportFbx_ToGlb_Succeeds)
{
    // --- 导入 FBX ---
    mc::Scene scene;
    auto ir = m_fbxImporter->Import(DataDir() + "lz.fbx", scene);
    ASSERT_TRUE(ir.ok) << ir.error;
    ASSERT_GT(scene.MeshCount(), 0u) << "FBX should contain meshes";

    NormalizeSceneForGltf(scene);

    // 打印原始 FBX 场景信息
    std::cout << "\n[FBX Source] meshes=" << scene.MeshCount()
              << " materials=" << scene.MaterialCount()
              << " textures=" << scene.TextureCount()
              << " nodes=" << scene.NodeCount() << "\n";
    std::cout << "  metadata: unit=" << scene.metadata.unit
              << " unitScale=" << scene.metadata.unitScale
              << " upAxis=" << static_cast<int>(scene.metadata.upAxis)
              << " handedness=" << static_cast<int>(scene.metadata.handedness)
              << "\n";
    for (const auto& m : scene.meshes)
    {
        std::cout << "  mesh[" << m.name << "] verts=" << m.positions.size()
                  << " indices=" << m.indices.size()
                  << " sections=" << m.sections.size();
        for (size_t s = 0; s < m.sections.size(); ++s)
            std::cout << " sec" << s << "(off=" << m.sections[s].indexOffset
                      << ",cnt=" << m.sections[s].indexCount << ")";
        std::cout << "\n";
    }
    for (const auto& t : scene.textures)
        std::cout << "  tex[" << t.name << "] uri=" << t.uri
                  << " embedded=" << t.embedded
                  << " dataBytes=" << t.embeddedData.size() << "\n";

    // --- 导出 GLB ---
    mc::ExportContext ctx;
    ctx.outputPath = m_outGlb;
    ctx.options.embedTextures = false;
    auto er = m_exporter->Export(scene, ctx);
    EXPECT_TRUE(er.ok) << er.error;
    EXPECT_TRUE(std::filesystem::exists(m_outGlb));

    // 打印导出统计
    uintmax_t glbSize = std::filesystem::exists(m_outGlb)
                        ? std::filesystem::file_size(m_outGlb) : 0;
    std::cout << "[GLB Output] " << m_outGlb << "\n"
              << "  meshes=" << ctx.meshesExported
              << " materials=" << ctx.materialsExported
              << " textures=" << ctx.texturesExported
              << " nodes=" << ctx.nodesExported
              << " fileSize=" << glbSize << " bytes\n";
}

//TEST_F(GltfExporterTest, ExportFbx_MeshCountConsistent)
//{
//    mc::Scene srcScene;
//    auto ir = m_fbxImporter->Import(DataDir() + "lz.fbx", srcScene);
//    ASSERT_TRUE(ir.ok) << ir.error;
//    NormalizeSceneForGltf(srcScene);
//    size_t srcMeshCount = srcScene.MeshCount();
//    ASSERT_GT(srcMeshCount, 0u);
//
//    mc::ExportContext ctx;
//    ctx.outputPath = m_tmpGlb;
//    auto er = m_exporter->Export(srcScene, ctx);
//    ASSERT_TRUE(er.ok) << er.error;
//
//    mc::Scene dstScene;
//    auto reimport = m_gltfImporter->Import(m_tmpGlb, dstScene);
//    ASSERT_TRUE(reimport.ok) << reimport.error;
//    EXPECT_EQ(dstScene.MeshCount(), srcMeshCount);
//}
//
//TEST_F(GltfExporterTest, ExportFbx_VertexCountConsistent)
//{
//    mc::Scene srcScene;
//    auto ir = m_fbxImporter->Import(DataDir() + "lz.fbx", srcScene);
//    ASSERT_TRUE(ir.ok) << ir.error;
//    NormalizeSceneForGltf(srcScene);
//    ASSERT_GE(srcScene.MeshCount(), 1u);
//
//    mc::ExportContext ctx;
//    ctx.outputPath = m_tmpGlb;
//    m_exporter->Export(srcScene, ctx);
//
//    mc::Scene dstScene;
//    auto reimport = m_gltfImporter->Import(m_tmpGlb, dstScene);
//    ASSERT_TRUE(reimport.ok) << reimport.error;
//    ASSERT_GE(dstScene.MeshCount(), 1u);
//    EXPECT_EQ(srcScene.meshes[0].positions.size(),
//              dstScene.meshes[0].positions.size());
//}
//
//TEST_F(GltfExporterTest, ExportFbx_NormalsDirectionConsistent)
//{
//    mc::Scene srcScene;
//    auto ir = m_fbxImporter->Import(DataDir() + "lz.fbx", srcScene);
//    ASSERT_TRUE(ir.ok) << ir.error;
//    NormalizeSceneForGltf(srcScene);
//
//    mc::ExportContext ctx;
//    ctx.outputPath = m_tmpGlb;
//    auto er = m_exporter->Export(srcScene, ctx);
//    ASSERT_TRUE(er.ok) << er.error;
//
//    mc::Scene dstScene;
//    auto reimport = m_gltfImporter->Import(m_tmpGlb, dstScene);
//    ASSERT_TRUE(reimport.ok) << reimport.error;
//
//    ASSERT_EQ(srcScene.MeshCount(), dstScene.MeshCount());
//
//    for (size_t i = 0; i < srcScene.meshes.size(); ++i)
//    {
//        const auto& sm = srcScene.meshes[i];
//        const auto& dm = dstScene.meshes[i];
//
//        ASSERT_EQ(sm.normals.size(), dm.normals.size())
//            << "mesh[" << i << "] normals count mismatch";
//
//        if (sm.normals.empty()) continue;
//
//        float minDot = 1.0f;
//        float sumDot = 0.0f;
//        size_t compared = 0;
//        for (size_t v = 0; v < sm.normals.size(); ++v)
//        {
//            const auto& sn = sm.normals[v];
//            const auto& dn = dm.normals[v];
//            float ls = Vec3Len(sn);
//            float ld = Vec3Len(dn);
//            if (ls < 1e-8f || ld < 1e-8f) continue;
//
//            float dot = Vec3Dot(sn, dn) / (ls * ld);
//            minDot = std::min(minDot, dot);
//            sumDot += dot;
//            ++compared;
//        }
//
//        ASSERT_GT(compared, 0u) << "mesh[" << i << "] has no valid normals to compare";
//        float avgDot = sumDot / static_cast<float>(compared);
//
//        EXPECT_GT(minDot, 0.0f) << "mesh[" << i << "] has flipped normals";
//        EXPECT_GT(avgDot, 0.98f) << "mesh[" << i << "] normals drift too large";
//    }
//}
//
//TEST_F(GltfExporterTest, ExportFbx_NodeTransformsConsistent)
//{
//    mc::Scene srcScene;
//    auto ir = m_fbxImporter->Import(DataDir() + "lz.fbx", srcScene);
//    ASSERT_TRUE(ir.ok) << ir.error;
//    NormalizeSceneForGltf(srcScene);
//
//    mc::ExportContext ctx;
//    ctx.outputPath = m_tmpGlb;
//    auto er = m_exporter->Export(srcScene, ctx);
//    ASSERT_TRUE(er.ok) << er.error;
//
//    mc::Scene dstScene;
//    auto reimport = m_gltfImporter->Import(m_tmpGlb, dstScene);
//    ASSERT_TRUE(reimport.ok) << reimport.error;
//
//    auto dstByName = BuildNodeNameMap(dstScene);
//    size_t matched = 0;
//
//    for (const auto& srcNode : srcScene.nodes)
//    {
//        if (srcNode.name.empty()) continue;
//        auto it = dstByName.find(srcNode.name);
//        if (it == dstByName.end()) continue;
//
//        const mc::Node* dstNode = it->second;
//        ASSERT_NE(dstNode, nullptr);
//        ++matched;
//
//        const float* sm = srcNode.localMatrix.m;
//        const float* dm = dstNode->localMatrix.m;
//
//        EXPECT_NEAR(sm[12], dm[12], 1e-4f) << "node=" << srcNode.name << " tx mismatch";
//        EXPECT_NEAR(sm[13], dm[13], 1e-4f) << "node=" << srcNode.name << " ty mismatch";
//        EXPECT_NEAR(sm[14], dm[14], 1e-4f) << "node=" << srcNode.name << " tz mismatch";
//
//        for (int k = 0; k < 12; ++k)
//        {
//            if (k == 3 || k == 7 || k == 11) continue;
//            EXPECT_NEAR(sm[k], dm[k], 1e-4f)
//                << "node=" << srcNode.name << " m[" << k << "] mismatch";
//        }
//    }
//
//    EXPECT_GT(matched, 0u) << "no node name matched between src and dst";
//}

// ============================================================
// 错误路径：outputPath 为空
// ============================================================
//
//TEST_F(GltfExporterTest, Export_EmptyPath_Fails)
//{
//    mc::Scene scene;
//    m_fbxImporter->Import(DataDir() + "lz.fbx", scene);
//
//    mc::ExportContext ctx;
//    ctx.outputPath = "";
//    auto er = m_exporter->Export(scene, ctx);
//    EXPECT_FALSE(er.ok);
//    EXPECT_FALSE(er.error.empty());
//}