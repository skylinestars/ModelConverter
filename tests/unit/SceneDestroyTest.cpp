#include <gtest/gtest.h>
#include "mc/core/Scene.h"

using namespace mc;

TEST(SceneDestroyTest, DestroyEmptyScene)
{
    // Just verify no crash/leak on empty scene destruction
    {
        Scene scene;
    }
    SUCCEED();
}

TEST(SceneDestroyTest, DestroySceneWithNodes)
{
    {
        Scene scene;
        for (int i = 0; i < 100; ++i)
        {
            scene.AddNode();
        }
    }
    SUCCEED();
}

TEST(SceneDestroyTest, DestroySceneWithMeshes)
{
    {
        Scene scene;
        for (int i = 0; i < 100; ++i)
        {
            Mesh& mesh = scene.AddMesh();
            mesh.positions.resize(1000);
            mesh.normals.resize(1000);
            mesh.indices.resize(3000);
        }
    }
    SUCCEED();
}

TEST(SceneDestroyTest, DestroySceneWithMaterials)
{
    {
        Scene scene;
        for (int i = 0; i < 100; ++i)
        {
            scene.AddMaterial();
        }
    }
    SUCCEED();
}

TEST(SceneDestroyTest, DestroySceneWithTextures)
{
    {
        Scene scene;
        for (int i = 0; i < 100; ++i)
        {
            Texture& tex = scene.AddTexture();
            // Simulate embedded data
            tex.embedded = true;
            tex.embeddedData.resize(1024 * 1024); // 1MB
        }
    }
    SUCCEED();
}

TEST(SceneDestroyTest, DestroySceneWithAllComponents)
{
    {
        Scene scene;

        // Nodes
        for (int i = 0; i < 50; ++i)
        {
            Node& node = scene.AddNode();
            node.name = "Node_" + std::to_string(i);
        }

        // Meshes with data
        for (int i = 0; i < 20; ++i)
        {
            Mesh& mesh = scene.AddMesh();
            mesh.positions.resize(500);
            mesh.normals.resize(500);
            mesh.indices.resize(1500);
            mesh.uvs.resize(1);
            mesh.uvs[0].resize(500);
        }

        // Materials
        for (int i = 0; i < 20; ++i)
        {
            scene.AddMaterial();
        }

        // Textures with embedded data
        for (int i = 0; i < 10; ++i)
        {
            Texture& tex = scene.AddTexture();
            tex.embedded = true;
            tex.embeddedData.resize(256 * 256 * 4); // RGBA
        }

        // Skeletons
        for (int i = 0; i < 5; ++i)
        {
            Skeleton skel;
            skel.id = scene.AllocateId();
            for (int j = 0; j < 20; ++j)
            {
                Bone bone;
                bone.id = scene.AllocateId();
                skel.bones.push_back(bone);
            }
            scene.skeletons.push_back(skel);
        }

        // Animations
        for (int i = 0; i < 5; ++i)
        {
            AnimationClip anim;
            anim.id = scene.AllocateId();
            anim.endTime = 2.0;
            NodeAnimation nodeAnim;
            nodeAnim.nodeId = 1;
            nodeAnim.translation.keys.push_back({0.0, Vec3(0, 0, 0)});
            nodeAnim.translation.keys.push_back({0.5, Vec3(1, 0, 0)});
            nodeAnim.translation.keys.push_back({1.0, Vec3(0, 0, 0)});
            anim.nodeChannels.push_back(nodeAnim);
            scene.animations.push_back(anim);
        }
    }
    SUCCEED();
}

TEST(SceneDestroyTest, DestroySceneMultipleTimes)
{
    for (int iter = 0; iter < 10; ++iter)
    {
        Scene scene;
        for (int i = 0; i < 50; ++i)
        {
            scene.AddNode();
            scene.AddMesh();
            scene.AddMaterial();
        }
    }
    SUCCEED();
}

TEST(SceneDestroyTest, MoveConstructedScene)
{
    Scene scene1;
    for (int i = 0; i < 50; ++i)
    {
        scene1.AddNode();
        scene1.AddMesh();
    }

    Scene scene2 = std::move(scene1);
    EXPECT_EQ(scene2.NodeCount(), 50);
    EXPECT_EQ(scene2.MeshCount(), 50);

    // scene1 should be in valid but unspecified state after move
}

TEST(SceneDestroyTest, MoveAssignedScene)
{
    Scene scene1;
    for (int i = 0; i < 50; ++i)
    {
        scene1.AddNode();
    }

    Scene scene2;
    scene2 = std::move(scene1);
    EXPECT_EQ(scene2.NodeCount(), 50);
}