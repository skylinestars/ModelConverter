#include <gtest/gtest.h>
#include "mc/core/Scene.h"

using namespace mc;

// ============================================================
// Acceptance criteria: 10000 Node, 100 Mesh, 100 Material
// Program exits without crash, no memory leak.
// ============================================================

TEST(SceneStressTest, Create10000Nodes)
{
    Scene scene;
    for (int i = 0; i < 10000; ++i)
    {
        Node& node = scene.AddNode();
        node.name = "Node_" + std::to_string(i);
    }
    EXPECT_EQ(scene.NodeCount(), 10000);
}

TEST(SceneStressTest, Create100Meshes)
{
    Scene scene;
    for (int i = 0; i < 100; ++i)
    {
        Mesh& mesh = scene.AddMesh();
        mesh.positions.resize(1000);
        mesh.normals.resize(1000);
        mesh.indices.resize(3000);
        mesh.uvs.resize(1);
        mesh.uvs[0].resize(1000);
        mesh.name = "Mesh_" + std::to_string(i);
    }
    EXPECT_EQ(scene.MeshCount(), 100);
}

TEST(SceneStressTest, Create100Materials)
{
    Scene scene;
    for (int i = 0; i < 100; ++i)
    {
        Material& mat = scene.AddMaterial();
        mat.name = "Material_" + std::to_string(i);
        mat.baseColor = Vec4(0.5f, 0.5f, 0.5f, 1.0f);
        mat.metallic = 0.0f;
        mat.roughness = 0.5f;
    }
    EXPECT_EQ(scene.MaterialCount(), 100);
}

TEST(SceneStressTest, FullStressTest)
{
    Scene scene;

    // 10000 Nodes
    for (int i = 0; i < 10000; ++i)
    {
        scene.AddNode();
    }

    // 100 Meshes
    for (int i = 0; i < 100; ++i)
    {
        Mesh& mesh = scene.AddMesh();
        mesh.positions.resize(1000);
        mesh.normals.resize(1000);
        mesh.indices.resize(3000);
    }

    // 100 Materials
    for (int i = 0; i < 100; ++i)
    {
        scene.AddMaterial();
    }

    EXPECT_EQ(scene.NodeCount(), 10000);
    EXPECT_EQ(scene.MeshCount(), 100);
    EXPECT_EQ(scene.MaterialCount(), 100);

    // Program exits without crash at scope end
}

TEST(SceneStressTest, RepeatedCreateDestroy)
{
    // Create and destroy large scenes multiple times
    for (int iter = 0; iter < 5; ++iter)
    {
        Scene scene;
        for (int i = 0; i < 2000; ++i)
        {
            scene.AddNode();
        }
        for (int i = 0; i < 50; ++i)
        {
            Mesh& mesh = scene.AddMesh();
            mesh.positions.resize(500);
            mesh.indices.resize(1500);
        }
        for (int i = 0; i < 50; ++i)
        {
            scene.AddMaterial();
        }
    }
    SUCCEED();
}

TEST(SceneStressTest, LargeMeshVertexData)
{
    Scene scene;
    Mesh& mesh = scene.AddMesh();

    const int vertexCount = 100000;
    mesh.positions.resize(vertexCount);
    mesh.normals.resize(vertexCount);
    mesh.indices.resize(vertexCount * 3);

    EXPECT_EQ(mesh.positions.size(), vertexCount);
    EXPECT_EQ(mesh.normals.size(), vertexCount);
    EXPECT_EQ(mesh.indices.size(), vertexCount * 3);
}

TEST(SceneStressTest, DeepNodeHierarchy)
{
    Scene scene;

    // Create a deep chain: 1000 levels
    ObjectID rootId = scene.AddNode().id;
    scene.rootNodes.push_back(rootId);

    ObjectID prevId = rootId;
    for (int i = 0; i < 1000; ++i)
    {
        ObjectID childId = scene.AddNode().id;
        scene.FindNode(childId)->parent = prevId;
        scene.FindNode(prevId)->children.push_back(childId);
        prevId = childId;
    }

    EXPECT_EQ(scene.NodeCount(), 1001);

    // Verify the chain
    ObjectID currentId = rootId;
    int depth = 0;
    while (true)
    {
        const Node* node = scene.FindNode(currentId);
        if (node->children.empty()) break;
        currentId = node->children[0];
        ++depth;
    }
    EXPECT_EQ(depth, 1000);
}

TEST(SceneStressTest, WideNodeHierarchy)
{
    Scene scene;

    // Reserve to avoid reallocation
    scene.nodes.reserve(2001);

    ObjectID rootId = scene.AddNode().id;
    scene.rootNodes.push_back(rootId);

    const int childCount = 2000;
    for (int i = 0; i < childCount; ++i)
    {
        ObjectID childId = scene.AddNode().id;
        scene.FindNode(childId)->parent = rootId;
        scene.FindNode(rootId)->children.push_back(childId);
    }

    EXPECT_EQ(scene.NodeCount(), childCount + 1);
    EXPECT_EQ(scene.FindNode(rootId)->children.size(), childCount);
}

TEST(SceneStressTest, IdLookupPerformance)
{
    Scene scene;
    std::vector<ObjectID> ids;

    for (int i = 0; i < 5000; ++i)
    {
        ids.push_back(scene.AddNode().id);
    }

    // Verify all lookups work
    for (ObjectID id : ids)
    {
        EXPECT_NE(scene.FindNode(id), nullptr);
    }

    // Verify invalid lookup returns null
    EXPECT_EQ(scene.FindNode(INVALID_ID), nullptr);
    EXPECT_EQ(scene.FindNode(999999), nullptr);
}