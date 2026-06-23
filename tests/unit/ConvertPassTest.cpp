// ============================================================
// Phase13 AxisConvertPass + UnitConvertPass 验收测试
// ============================================================
// 验收标准：
//   cm→m 误差 < 1e-6
//   YUp→ZUp 结果正确
//   ZUp→YUp 结果正确
//   identity（同轴）无修改
//   factor<=0 返回错误

#include <gtest/gtest.h>
#include "mc/pipeline/AxisConvertPass.h"
#include "mc/pipeline/UnitConvertPass.h"
#include "mc/pipeline/HandednessFlipPass.h"
#include "mc/core/Scene.h"
#include <cmath>

namespace {

mc::Scene MakeSceneWithVertex(mc::Vec3 pos)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mesh.positions.push_back(pos);
    mesh.indices = {0, 0, 0};
    mc::MeshSection sec; sec.indexOffset = 0; sec.indexCount = 3;
    mesh.sections.push_back(sec);
    auto& node = scene.AddNode();
    node.meshIds.push_back(mesh.id);
    node.localMatrix.m[12] = pos.x;
    node.localMatrix.m[13] = pos.y;
    node.localMatrix.m[14] = pos.z;
    scene.rootNodes.push_back(node.id);
    return scene;
}

float Err(float a, float b) { return std::abs(a - b); }

} // namespace

// ============================================================
// UnitConvertPass
// ============================================================

TEST(UnitConvertPassTest, CmToM_VertexScaled)
{
    auto scene = MakeSceneWithVertex({100.f, 200.f, 300.f});
    mc::UnitConvertPass pass(0.01f);
    auto r = pass.Execute(scene);
    ASSERT_TRUE(r.ok);
    EXPECT_LT(Err(scene.meshes[0].positions[0].x, 1.0f), 1e-6f);
    EXPECT_LT(Err(scene.meshes[0].positions[0].y, 2.0f), 1e-6f);
    EXPECT_LT(Err(scene.meshes[0].positions[0].z, 3.0f), 1e-6f);
}

TEST(UnitConvertPassTest, CmToM_NodeTranslationScaled)
{
    auto scene = MakeSceneWithVertex({50.f, 0.f, 0.f});
    mc::UnitConvertPass pass(0.01f);
    pass.Execute(scene);
    EXPECT_LT(Err(scene.nodes[0].localMatrix.m[12], 0.5f), 1e-6f);
    EXPECT_EQ(scene.metadata.unit, "cm");
    EXPECT_LT(Err(scene.metadata.unitScale, 0.01f), 1e-9f);
}

TEST(UnitConvertPassTest, IdentityFactor_NoChange)
{
    auto scene = MakeSceneWithVertex({1.f, 2.f, 3.f});
    mc::UnitConvertPass pass(1.0f);
    pass.Execute(scene);
    EXPECT_LT(Err(scene.meshes[0].positions[0].x, 1.f), 1e-6f);
}

TEST(UnitConvertPassTest, InvalidFactor_ReturnsError)
{
    auto scene = MakeSceneWithVertex({1.f, 0.f, 0.f});
    mc::UnitConvertPass pass(0.0f);
    auto r = pass.Execute(scene);
    EXPECT_FALSE(r.ok);
}

TEST(UnitConvertPassTest, NegativeFactor_ReturnsError)
{
    auto scene = MakeSceneWithVertex({1.f, 0.f, 0.f});
    mc::UnitConvertPass pass(-1.0f);
    auto r = pass.Execute(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// AxisConvertPass
// ============================================================

TEST(AxisConvertPassTest, Identity_NoChange)
{
    auto scene = MakeSceneWithVertex({1.f, 2.f, 3.f});
    mc::AxisConvertPass pass(mc::UpAxis::Y, mc::UpAxis::Y);
    pass.Execute(scene);
    auto& p = scene.meshes[0].positions[0];
    EXPECT_LT(Err(p.x, 1.f), 1e-6f);
    EXPECT_LT(Err(p.y, 2.f), 1e-6f);
    EXPECT_LT(Err(p.z, 3.f), 1e-6f);
}

TEST(AxisConvertPassTest, YUpToZUp_Correct)
{
    // YUp→ZUp: new = (x, z, -y)
    auto scene = MakeSceneWithVertex({1.f, 2.f, 3.f});
    mc::AxisConvertPass pass(mc::UpAxis::Y, mc::UpAxis::Z);
    auto r = pass.Execute(scene);
    ASSERT_TRUE(r.ok);
    auto& p = scene.meshes[0].positions[0];
    EXPECT_LT(Err(p.x,  1.f), 1e-6f);
    EXPECT_LT(Err(p.y,  3.f), 1e-6f);   // old z
    EXPECT_LT(Err(p.z, -2.f), 1e-6f);   // -old y
}

TEST(AxisConvertPassTest, ZUpToYUp_Correct)
{
    // ZUp→YUp: new = (x, -z, y)
    auto scene = MakeSceneWithVertex({1.f, 2.f, 3.f});
    mc::AxisConvertPass pass(mc::UpAxis::Z, mc::UpAxis::Y);
    auto r = pass.Execute(scene);
    ASSERT_TRUE(r.ok);
    auto& p = scene.meshes[0].positions[0];
    EXPECT_LT(Err(p.x,  1.f), 1e-6f);
    EXPECT_LT(Err(p.y, -3.f), 1e-6f);   // -old z  (ZUp src[1]=2 -> signY=-1)
    EXPECT_LT(Err(p.z,  2.f), 1e-6f);   // old y
}

TEST(AxisConvertPassTest, YUpToZUp_NodeTranslation)
{
    auto scene = MakeSceneWithVertex({4.f, 5.f, 6.f});
    scene.metadata.upAxis = mc::Axis::Y;
    mc::AxisConvertPass pass(mc::UpAxis::Y, mc::UpAxis::Z);
    pass.Execute(scene);
    float* m = scene.nodes[0].localMatrix.m;
    EXPECT_LT(Err(m[12],  4.f), 1e-6f);
    EXPECT_LT(Err(m[13],  6.f), 1e-6f);   // old z
    EXPECT_LT(Err(m[14], -5.f), 1e-6f);   // -old y
    EXPECT_EQ(scene.metadata.upAxis, mc::Axis::Z);
}

TEST(AxisConvertPassTest, RoundTrip_YZY)
{
    // YUp→ZUp→YUp 应该还原
    auto scene = MakeSceneWithVertex({1.f, 2.f, 3.f});
    mc::AxisConvertPass toZ(mc::UpAxis::Y, mc::UpAxis::Z);
    mc::AxisConvertPass toY(mc::UpAxis::Z, mc::UpAxis::Y);
    toZ.Execute(scene);
    toY.Execute(scene);
    auto& p = scene.meshes[0].positions[0];
    EXPECT_LT(Err(p.x, 1.f), 1e-5f);
    EXPECT_LT(Err(p.y, 2.f), 1e-5f);
    EXPECT_LT(Err(p.z, 3.f), 1e-5f);
}

TEST(AxisConvertPassTest, Name_IsAxisConvertPass)
{
    mc::AxisConvertPass pass(mc::UpAxis::Y, mc::UpAxis::Z);
    EXPECT_EQ(pass.Name(), "AxisConvertPass");
}

TEST(UnitConvertPassTest, Name_IsUnitConvertPass)
{
    mc::UnitConvertPass pass(0.01f);
    EXPECT_EQ(pass.Name(), "UnitConvertPass");
}

TEST(HandednessFlipPassTest, LeftToRight_FlipsPositionNormalAndWinding)
{
    mc::Scene scene;

    auto& mesh = scene.AddMesh();
    mesh.positions = {{1.f, 2.f, 3.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
    mesh.normals = {{1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}};
    mesh.indices = {0, 1, 2};
    mc::MeshSection sec; sec.indexOffset = 0; sec.indexCount = 3;
    mesh.sections.push_back(sec);

    auto& node = scene.AddNode();
    node.localMatrix.m[12] = 2.f;
    scene.rootNodes.push_back(node.id);

    scene.metadata.handedness = mc::Handedness::Left;

    mc::HandednessFlipPass pass(mc::Handedness::Left, mc::Handedness::Right);
    auto r = pass.Execute(scene);
    ASSERT_TRUE(r.ok);

    EXPECT_LT(Err(scene.meshes[0].positions[0].x, -1.f), 1e-6f);
    EXPECT_LT(Err(scene.meshes[0].positions[0].y,  2.f), 1e-6f);
    EXPECT_LT(Err(scene.meshes[0].positions[0].z,  3.f), 1e-6f);

    EXPECT_LT(Err(scene.meshes[0].normals[0].x, -1.f), 1e-6f);
    EXPECT_LT(Err(scene.meshes[0].normals[0].y,  0.f), 1e-6f);
    EXPECT_LT(Err(scene.meshes[0].normals[0].z,  0.f), 1e-6f);

    EXPECT_EQ(scene.meshes[0].indices[0], 0u);
    EXPECT_EQ(scene.meshes[0].indices[1], 2u);
    EXPECT_EQ(scene.meshes[0].indices[2], 1u);

    EXPECT_LT(Err(scene.nodes[0].localMatrix.m[12], -2.f), 1e-6f);
    EXPECT_EQ(scene.metadata.handedness, mc::Handedness::Right);
}
