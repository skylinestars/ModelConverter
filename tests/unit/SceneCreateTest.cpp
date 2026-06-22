#include <gtest/gtest.h>
#include "mc/core/Scene.h"

using namespace mc;

TEST(SceneCreateTest, CreateEmptyScene)
{
    Scene scene;
    EXPECT_EQ(scene.NodeCount(), 0);
    EXPECT_EQ(scene.MeshCount(), 0);
    EXPECT_EQ(scene.MaterialCount(), 0);
    EXPECT_EQ(scene.TextureCount(), 0);
    EXPECT_TRUE(scene.rootNodes.empty());
    EXPECT_TRUE(scene.skeletons.empty());
    EXPECT_TRUE(scene.skins.empty());
    EXPECT_TRUE(scene.animations.empty());
}

TEST(SceneCreateTest, AddSingleNode)
{
    Scene scene;
    Node& node = scene.AddNode();
    EXPECT_NE(node.id, INVALID_ID);
    EXPECT_EQ(scene.NodeCount(), 1);
    EXPECT_EQ(scene.FindNode(node.id), &node);
}

TEST(SceneCreateTest, AddMultipleNodes)
{
    Scene scene;
    for (int i = 0; i < 10; ++i)
    {
        scene.AddNode();
    }
    EXPECT_EQ(scene.NodeCount(), 10);
}

TEST(SceneCreateTest, NodeDefaultValues)
{
    Scene scene;
    Node& node = scene.AddNode();

    EXPECT_EQ(node.parent, INVALID_ID);
    EXPECT_TRUE(node.children.empty());
    EXPECT_TRUE(node.meshIds.empty());
    EXPECT_EQ(node.cameraId, INVALID_ID);
    EXPECT_EQ(node.lightId, INVALID_ID);
    EXPECT_EQ(node.instanceOfNodeId, INVALID_ID);
    EXPECT_EQ(node.type, NodeType::Empty);
}

TEST(SceneCreateTest, NodeParentChildRelation)
{
    Scene scene;
    ObjectID parentId = scene.AddNode().id;
    ObjectID childId = scene.AddNode().id;

    scene.FindNode(childId)->parent = parentId;
    scene.FindNode(parentId)->children.push_back(childId);

    EXPECT_EQ(scene.FindNode(childId)->parent, parentId);
    EXPECT_EQ(scene.FindNode(parentId)->children.size(), 1);
    EXPECT_EQ(scene.FindNode(parentId)->children[0], childId);
}

TEST(SceneCreateTest, AddMesh)
{
    Scene scene;
    Mesh& mesh = scene.AddMesh();
    EXPECT_NE(mesh.id, INVALID_ID);
    EXPECT_EQ(scene.MeshCount(), 1);
    EXPECT_EQ(scene.FindMesh(mesh.id), &mesh);
}

TEST(SceneCreateTest, MeshDefaultValues)
{
    Scene scene;
    Mesh& mesh = scene.AddMesh();

    EXPECT_EQ(mesh.primitiveType, PrimitiveType::Triangles);
    EXPECT_TRUE(mesh.positions.empty());
    EXPECT_TRUE(mesh.normals.empty());
    EXPECT_TRUE(mesh.indices.empty());
    EXPECT_TRUE(mesh.sections.empty());
    EXPECT_TRUE(mesh.morphTargets.empty());
}

TEST(SceneCreateTest, MeshWithSections)
{
    Scene scene;
    Mesh& mesh = scene.AddMesh();
    mesh.sections.push_back({0, 100, INVALID_ID});
    mesh.sections.push_back({100, 50, INVALID_ID});

    EXPECT_EQ(mesh.sections.size(), 2);
    EXPECT_EQ(mesh.sections[0].indexOffset, 0);
    EXPECT_EQ(mesh.sections[0].indexCount, 100);
}

TEST(SceneCreateTest, AddMaterial)
{
    Scene scene;
    Material& mat = scene.AddMaterial();
    EXPECT_NE(mat.id, INVALID_ID);
    EXPECT_EQ(scene.MaterialCount(), 1);
    EXPECT_EQ(scene.FindMaterial(mat.id), &mat);
}

TEST(SceneCreateTest, MaterialDefaultValues)
{
    Scene scene;
    Material& mat = scene.AddMaterial();

    EXPECT_EQ(mat.workflow, Material::MetallicRoughness);
    EXPECT_FLOAT_EQ(mat.opacity, 1.0f);
    EXPECT_FLOAT_EQ(mat.metallic, 1.0f);
    EXPECT_FLOAT_EQ(mat.roughness, 1.0f);
    EXPECT_FALSE(mat.doubleSided);
    EXPECT_EQ(mat.baseColorTexture.textureId, INVALID_ID);
}

TEST(SceneCreateTest, AddTexture)
{
    Scene scene;
    Texture& tex = scene.AddTexture();
    EXPECT_NE(tex.id, INVALID_ID);
    EXPECT_EQ(scene.TextureCount(), 1);
    EXPECT_EQ(scene.FindTexture(tex.id), &tex);
}

TEST(SceneCreateTest, AddSkeleton)
{
    Scene scene;
    Skeleton skel;
    skel.id = scene.AllocateId();
    skel.name = "TestSkeleton";
    Bone bone;
    bone.id = scene.AllocateId();
    bone.name = "RootBone";
    skel.bones.push_back(bone);
    scene.skeletons.push_back(skel);

    EXPECT_EQ(scene.skeletons.size(), 1);
    EXPECT_EQ(scene.skeletons[0].name, "TestSkeleton");
    EXPECT_EQ(scene.skeletons[0].bones.size(), 1);
}

TEST(SceneCreateTest, AddSkin)
{
    Scene scene;
    Skin skin;
    skin.id = scene.AllocateId();
    skin.name = "TestSkin";
    skin.skeletonId = 42;
    scene.skins.push_back(skin);

    EXPECT_EQ(scene.skins.size(), 1);
    EXPECT_EQ(scene.skins[0].skeletonId, 42);
}

TEST(SceneCreateTest, AddAnimation)
{
    Scene scene;
    AnimationClip anim;
    anim.id = scene.AllocateId();
    anim.name = "TestAnim";
    anim.endTime = 1.0;
    scene.animations.push_back(anim);

    EXPECT_EQ(scene.animations.size(), 1);
    EXPECT_DOUBLE_EQ(scene.animations[0].Duration(), 1.0);
}

TEST(SceneCreateTest, SceneMetadataDefaults)
{
    Scene scene;
    EXPECT_EQ(scene.metadata.unit, "m");
    EXPECT_FLOAT_EQ(scene.metadata.unitScale, 1.0f);
    EXPECT_EQ(scene.metadata.upAxis, Axis::Y);
    EXPECT_EQ(scene.metadata.handedness, Handedness::Right);
}

TEST(SceneCreateTest, IdUniqueness)
{
    Scene scene;
    ObjectID id1 = scene.AddNode().id;
    ObjectID id2 = scene.AddNode().id;
    ObjectID id3 = scene.AddMesh().id;
    ObjectID id4 = scene.AddMaterial().id;

    EXPECT_NE(id1, id2);
    EXPECT_NE(id1, id3);
    EXPECT_NE(id1, id4);
    EXPECT_NE(id2, id3);
}

TEST(SceneCreateTest, RootNodes)
{
    Scene scene;
    Node& root1 = scene.AddNode();
    Node& root2 = scene.AddNode();
    scene.rootNodes.push_back(root1.id);
    scene.rootNodes.push_back(root2.id);

    EXPECT_EQ(scene.rootNodes.size(), 2);
    EXPECT_EQ(scene.rootNodes[0], root1.id);
    EXPECT_EQ(scene.rootNodes[1], root2.id);
}

TEST(SceneCreateTest, Camera)
{
    Scene scene;
    Camera cam;
    cam.id = scene.AllocateId();
    cam.type = CameraType::Perspective;
    cam.yfov = 1.2f;
    scene.cameras.push_back(cam);

    EXPECT_EQ(scene.cameras.size(), 1);
    EXPECT_FLOAT_EQ(scene.cameras[0].yfov, 1.2f);
}

TEST(SceneCreateTest, Light)
{
    Scene scene;
    Light light;
    light.id = scene.AllocateId();
    light.type = LightType::Directional;
    light.intensity = 2.0f;
    scene.lights.push_back(light);

    EXPECT_EQ(scene.lights.size(), 1);
    EXPECT_FLOAT_EQ(scene.lights[0].intensity, 2.0f);
}

TEST(SceneCreateTest, PointInstancer)
{
    Scene scene;
    PointInstancer pi;
    pi.id = scene.AllocateId();
    pi.positions.push_back({1, 0, 0});
    pi.orientations.push_back(Quaternion::Identity());
    scene.pointInstancers.push_back(pi);

    EXPECT_EQ(scene.pointInstancers.size(), 1);
    EXPECT_EQ(scene.pointInstancers[0].positions.size(), 1);
}

TEST(SceneCreateTest, CompositeScene)
{
    Scene scene;
    scene.metadata.unit = "cm";
    scene.metadata.unitScale = 0.01f;

    // Add nodes
    for (int i = 0; i < 5; ++i) scene.AddNode();

    // Add meshes
    for (int i = 0; i < 3; ++i) scene.AddMesh();

    // Add materials
    for (int i = 0; i < 2; ++i) scene.AddMaterial();

    // Add textures
    for (int i = 0; i < 4; ++i) scene.AddTexture();

    EXPECT_EQ(scene.NodeCount(), 5);
    EXPECT_EQ(scene.MeshCount(), 3);
    EXPECT_EQ(scene.MaterialCount(), 2);
    EXPECT_EQ(scene.TextureCount(), 4);
    EXPECT_EQ(scene.metadata.unit, "cm");
}