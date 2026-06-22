#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>

namespace mc {

// ============================================================
// Material
// ============================================================

struct TextureTransform
{
    Vec2 offset  = {0,0};
    Vec2 scale   = {1,1};
    float rotation = 0.0f;
};

struct TextureRef
{
    ObjectID textureId = INVALID_ID;  // 引用 Scene::textures；INVALID_ID 表示"未使用"，ValidatePass 跳过
    int      uvSet     = 0;  // 使用哪一套 UV
    TextureTransform transform;
};

enum class AlphaMode { Opaque, Mask, Blend };

struct Material
{
    enum Workflow
    {
        MetallicRoughness,  // GLTF / USD 标准
        SpecularGlossiness,  // FBX 常见
        Phong,  // 传统 OBJ/FBX
        Unlit  // GLTF KHR_materials_unlit
    };

    ObjectID    id;
    std::string name;
    Workflow    workflow = MetallicRoughness;

    // MetallicRoughness
    // 注意：metallic=1.0, roughness=1.0 是 GLTF 2.0 规范默认值
    // 调试阶段：实际使用时建议在 Importer 层面改为 (metallic=0, roughness=0.5)，
    // 否则未设置材质的 Mesh 会渲染为"全金属高粗糙度"的深色外观
    Vec4   baseColor  = Vec4(1, 1, 1, 1);
    float  metallic   = 1.0f;
    float  roughness  = 1.0f;

    // SpecularGlossiness
    Vec3   specular   = Vec3(1, 1, 1);
    float  glossiness = 1.0f;

    // Phong（Legacy）
    Vec3   diffuse    = Vec3(1, 1, 1);
    Vec3   ambient    = Vec3(0, 0, 0);
    float  shininess  = 32.0f;

    // Common
    AlphaMode alphaMode = AlphaMode::Opaque;
    float  opacity      = 1.0f;
    float  alphaCutoff  = 0.0f;
    bool   doubleSided  = false;
    Vec3   emissive     = Vec3(0, 0, 0);

    // 贴图引用
    TextureRef baseColorTexture;
    TextureRef metallicRoughnessTexture;
    TextureRef normalTexture;
    TextureRef emissiveTexture;
    TextureRef occlusionTexture;
    TextureRef specularTexture;
    // ... 按需扩展
};

} // namespace mc