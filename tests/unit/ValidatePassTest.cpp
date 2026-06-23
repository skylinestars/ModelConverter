// ============================================================
// ValidatePass 单元测试
// ============================================================
// Phase05 验收标准：
//   - 非法 Mesh 引用   → Validate 失败
//   - 循环父子关系     → Validate 失败
//   - 重复 mc::ObjectID    → Validate 失败
//   - 非法索引         → Validate 失败
//   - 合法 Scene       → Validate 成功
//   - 共 20 个非法案例全部通过

#include <gtest/gtest.h>
#include "mc/pipeline/ValidatePass.h"
#include "mc/pipeline/Pipeline.h"
#include "mc/core/Scene.h"

// ============================================================
// 辅助：在已有 Scene 上跑 ValidatePass
// ============================================================
static mc::VoidResult RunValidate(mc::Scene& scene)
{
    mc::ValidatePass pass;
    return pass.Execute(scene);
}

// ============================================================
// 合法 Scene
// ============================================================

TEST(ValidatePassTest, ValidEmptyScene)
{
    mc::Scene scene;
    auto r = RunValidate(scene);
    EXPECT_TRUE(r.ok) << r.error;
}

TEST(ValidatePassTest, ValidSceneWithNodeAndMesh)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    mesh.indices   = {0,1,2};
    mc::MeshSection sec;
    sec.materialId = mc::INVALID_ID;
    mesh.sections.push_back(sec);

    mc::ObjectID idN = scene.AddNode().id;
    scene.FindNode(idN)->meshIds.push_back(mesh.id);
    scene.rootNodes.push_back(idN);

    auto r = RunValidate(scene);
    EXPECT_TRUE(r.ok) << r.error;
}

// ============================================================
// 非法案例组 1：重复 mc::ObjectID（检查1）
// ============================================================

// 案例01：nodes 中有重复 ID
TEST(ValidatePassTest, Case01_DuplicateNodeId)
{
    mc::Scene scene;
    scene.AddNode();
    scene.AddNode();
    // 直接操作 vector，避免 push_back 后引用失效
    mc::ObjectID firstId = scene.nodes[0].id;
    scene.nodes[1].id = firstId;  // 强制重复

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

// 案例02：meshes 中有重复 ID
TEST(ValidatePassTest, Case02_DuplicateMeshId)
{
    mc::Scene scene;
    scene.AddMesh();
    scene.AddMesh();
    mc::ObjectID firstId = scene.meshes[0].id;
    scene.meshes[1].id = firstId;  // 强制重复

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 2：rootNodes 引用不存在的 NodeID（检查2）
// ============================================================

// 案例03：rootNodes 包含不存在的 ID
TEST(ValidatePassTest, Case03_RootNodeUnknownId)
{
    mc::Scene scene;
    scene.rootNodes.push_back(9999);  // 根本不存在

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 3：循环父子关系（检查3）
// ============================================================

// 案例04：A->B->A 直接成环
TEST(ValidatePassTest, Case04_CycleTwoNodes)
{
    mc::Scene scene;
    mc::ObjectID idA = scene.AddNode().id;
    mc::ObjectID idB = scene.AddNode().id;
    // AddNode 可能导致 vector 扩容，之后必须通过 FindNode 访问
    scene.FindNode(idA)->children.push_back(idB);
    scene.FindNode(idB)->children.push_back(idA);  // 成环
    scene.rootNodes.push_back(idA);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// 案例05：A->B->C->A 三节点成环
TEST(ValidatePassTest, Case05_CycleThreeNodes)
{
    mc::Scene scene;
    mc::ObjectID idA = scene.AddNode().id;
    mc::ObjectID idB = scene.AddNode().id;
    mc::ObjectID idC = scene.AddNode().id;
    scene.FindNode(idA)->children.push_back(idB);
    scene.FindNode(idB)->children.push_back(idC);
    scene.FindNode(idC)->children.push_back(idA);  // 成环
    scene.rootNodes.push_back(idA);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// 案例06：节点自引用（A 是自己的孩子）
TEST(ValidatePassTest, Case06_SelfReferenceNode)
{
    mc::Scene scene;
    mc::ObjectID idA = scene.AddNode().id;
    scene.FindNode(idA)->children.push_back(idA);  // 自己引用自己
    scene.rootNodes.push_back(idA);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 4：Node::meshIds 引用不存在的 Mesh（检查4）
// ============================================================

// 案例07：Node 引用不存在的 MeshID
TEST(ValidatePassTest, Case07_NodeRefUnknownMesh)
{
    mc::Scene scene;
    mc::ObjectID idN = scene.AddNode().id;
    scene.FindNode(idN)->meshIds.push_back(9999);
    scene.rootNodes.push_back(idN);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 5：MeshSection 引用不存在的 Material（检查7）
// ============================================================

// 案例08：MeshSection 引用不存在的 MaterialID
TEST(ValidatePassTest, Case08_SectionRefUnknownMaterial)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mc::MeshSection sec;
    sec.materialId = 9999;  // 不存在
    mesh.sections.push_back(sec);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 6：索引越界（检查8）
// ============================================================

// 案例09：index 值超出 positions 范围
TEST(ValidatePassTest, Case09_IndexOutOfRange)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    mesh.indices   = {0, 1, 99};  // 99 越界

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// 案例10：positions 为空但有索引
TEST(ValidatePassTest, Case10_IndicesWithEmptyPositions)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mesh.positions = {};
    mesh.indices   = {0};  // positions 为空，任何索引都越界

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 7：UV/Color 通道长度不匹配（检查9）
// ============================================================

// 案例11：UV 通道长度与 positions 不符
TEST(ValidatePassTest, Case11_UvLengthMismatch)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    mesh.uvs.push_back({{0,0},{1,0}});  // 只有2个，应有3个

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// 案例12：Color 通道长度与 positions 不符
TEST(ValidatePassTest, Case12_ColorLengthMismatch)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    mesh.colors.push_back({{1,0,0,1}});  // 只有1个，应有3个

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 8：MorphTarget 长度不匹配（检查10）
// ============================================================

// 案例13：positionDeltas 长度与 positions 不符
TEST(ValidatePassTest, Case13_MorphDeltaLengthMismatch)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    mc::MorphTarget mt;
    mt.positionDeltas = {{0,0,1}};  // 只有1个，应有3个
    mesh.morphTargets.push_back(mt);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 9：TextureRef 引用不存在的 Texture（检查11）
// ============================================================

// 案例14：Material::baseColorTexture 引用不存在的 TextureID
TEST(ValidatePassTest, Case14_MaterialRefUnknownTexture)
{
    mc::Scene scene;
    auto& mat = scene.AddMaterial();
    mat.baseColorTexture.textureId = 9999;  // 不存在

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 10：Skin 引用不存在（检查12/13）
// ============================================================

// 案例15：Skin::skeletonId 引用不存在
TEST(ValidatePassTest, Case15_SkinRefUnknownSkeleton)
{
    mc::Scene scene;
    mc::Skin skin;
    skin.id         = scene.AllocateId();
    skin.skeletonId = 9999;
    scene.skins.push_back(skin);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// 案例16：Skin::meshId 引用不存在
TEST(ValidatePassTest, Case16_SkinRefUnknownMesh)
{
    mc::Scene scene;
    mc::Skin skin;
    skin.id     = scene.AllocateId();
    skin.meshId = 9999;
    scene.skins.push_back(skin);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 11：PointInstancer 引用不存在的 Node（检查14）
// ============================================================

// 案例17：prototypeNodeId 引用不存在
TEST(ValidatePassTest, Case17_PointInstancerRefUnknownNode)
{
    mc::Scene scene;
    mc::PointInstancer pi;
    pi.id              = scene.AllocateId();
    pi.prototypeNodeId = 9999;
    scene.pointInstancers.push_back(pi);

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 非法案例组 12：Animation 引用不存在（检查15/16）
// ============================================================

// 案例18：NodeAnimation::nodeId 引用不存在的 Node
TEST(ValidatePassTest, Case18_NodeAnimRefUnknownNode)
{
    mc::Scene scene;
    mc::AnimationClip clip;
    clip.id = scene.AllocateId();
    mc::NodeAnimation ch;
    ch.nodeId = 9999;
    clip.nodeChannels.push_back(ch);
    scene.animations.push_back(std::move(clip));

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// 案例19：MorphAnimation::meshId 引用不存在的 Mesh
TEST(ValidatePassTest, Case19_MorphAnimRefUnknownMesh)
{
    mc::Scene scene;
    mc::AnimationClip clip;
    clip.id = scene.AllocateId();
    mc::MorphAnimation ch;
    ch.meshId     = 9999;
    ch.morphIndex = 0;
    clip.morphChannels.push_back(ch);
    scene.animations.push_back(std::move(clip));

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// 案例20：MorphAnimation::morphIndex 超出 morphTargets 范围
TEST(ValidatePassTest, Case20_MorphAnimIndexOutOfRange)
{
    mc::Scene scene;
    auto& mesh = scene.AddMesh();
    // mesh 有1个 morphTarget
    mc::MorphTarget mt;
    mt.positionDeltas = {};
    mesh.morphTargets.push_back(mt);

    mc::AnimationClip clip;
    clip.id = scene.AllocateId();
    mc::MorphAnimation ch;
    ch.meshId     = mesh.id;
    ch.morphIndex = 99;  // 越界
    clip.morphChannels.push_back(ch);
    scene.animations.push_back(std::move(clip));

    auto r = RunValidate(scene);
    EXPECT_FALSE(r.ok);
}

// ============================================================
// 通过 Pipeline 集成验证
// ============================================================

TEST(ValidatePassTest, ValidatePassInPipeline)
{
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<mc::ValidatePass>());

    mc::Scene scene;
    auto r = pipeline.Execute(scene);
    EXPECT_TRUE(r.ok) << r.error;
}

TEST(ValidatePassTest, InvalidSceneFailsInPipeline)
{
    mc::Pipeline pipeline;
    pipeline.AddPass(std::make_unique<mc::ValidatePass>());

    mc::Scene scene;
    auto& node = scene.AddNode();
    node.meshIds.push_back(9999);  // 非法引用

    auto r = pipeline.Execute(scene);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("ValidatePass"), std::string::npos);
}

// ============================================================
// 验证合法的复杂场景通过
// ============================================================

TEST(ValidatePassTest, ValidComplexScene)
{
    mc::Scene scene;

    // 纹理
    auto& tex = scene.AddTexture();

    // 材质
    auto& mat = scene.AddMaterial();
    mat.baseColorTexture.textureId = tex.id;

    // Mesh
    auto& mesh = scene.AddMesh();
    mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    mesh.indices   = {0,1,2};
    mc::MeshSection sec;
    sec.materialId = mat.id;
    mesh.sections.push_back(sec);

    // Nodes
    mc::ObjectID idChild = scene.AddNode().id;
    scene.FindNode(idChild)->meshIds.push_back(mesh.id);

    mc::ObjectID idRoot = scene.AddNode().id;
    scene.FindNode(idRoot)->children.push_back(idChild);
    scene.rootNodes.push_back(idRoot);

    // Skeleton + Skin
    mc::Skeleton skel;
    skel.id = scene.AllocateId();
    scene.skeletons.push_back(skel);

    mc::Skin skin;
    skin.id         = scene.AllocateId();
    skin.skeletonId = skel.id;
    skin.meshId     = mesh.id;
    scene.skins.push_back(skin);

    auto r = RunValidate(scene);
    EXPECT_TRUE(r.ok) << r.error;
}
