#pragma once

#include "mc/common/Types.h"
#include "mc/common/Math.h"
#include <string>
#include <vector>
#include <cstdint>

namespace mc {

// ============================================================
// Mesh
// ============================================================
enum class PrimitiveType { Triangles, TriangleStrip, TriangleFan, Lines, Points, Polygons };

struct MeshSection
{
    uint32_t  indexOffset = 0;
    uint32_t  indexCount  = 0;
    ObjectID  materialId  = INVALID_ID;  // 引用 Scene::materials
};

struct MorphTarget
{
    std::string       name;
    std::vector<Vec3> positionDeltas;  // 与 positions 同长（Importer 负责把稀疏数据展开）
    std::vector<Vec3> normalDeltas;
    std::vector<Vec3> tangentDeltas;
};

// ---------- CustomAttribute（扩展槽） ----------
enum class AttrFormat
{
    F32, F32x2, F32x3, F32x4,
    U8,  U32,  I32
};

struct CustomAttribute
{
    std::string            name;  // "temperature", "intensity", "classification" ...
    AttrFormat             format;
    uint32_t               elementCount = 0;  // 通常等于 positions.size()
    std::vector<uint8_t>   data;  // 原始字节，按 format 解析
};
// 配套访问器（header-only）：
//   template <class T> span<const T> As(const CustomAttribute&)

// ---------- Skin（可变权重，不固定4关节） ----------
// Core 层允许任意数量的关节影响。工业模型（CAD / 扫描模型）常见 8+ 权重。
// 目标格式限制时由 LimitBoneInfluencePass 压缩为 N 权重（例如 GLTF 导出时压到 4）。
struct VertexInfluence
{
    uint16_t joint  = 0;  // Skeleton::bones 的 vector index（非 ObjectID，刻意的例外）
    // 原因：骨骼影响数据量大（每顶点多条），用 ObjectID 查找开销过高；
    // Skeleton::bones 在生命周期内不会重排，index 稳定。
    // LimitBoneInfluencePass 操作后 index 仍对应同一 Skeleton。
    float    weight = 0.0f;
};


// 仅负责几何，skinInfluences放在mesh合理：权重本来就是顶点属性。
struct Mesh
{
    ObjectID                                  id;
    std::string                               name;
    PrimitiveType                             primitiveType = PrimitiveType::Triangles;

    // ============== 标准属性（强类型） ==============
    std::vector<Vec3>                         positions;
    std::vector<Vec3>                         normals;
    std::vector<Vec4>                         tangents;

    std::vector<std::vector<Vec2>>            uvs;  // uvs[set][vtx]，不写死 uv0/uv1
    std::vector<std::vector<Color4>>          colors;// colors[set][vtx]

    std::vector<uint32_t>                     indices;
    std::vector<MeshSection>                  sections;// 多材质分段

    // 蒙皮：外层 index = vertex index；内层 = 该顶点的所有骨骼影响
    // skinInfluences[vertexIndex] = 该顶点的所有骨骼影响（joint+weight 对）
    // 每个顶点的 influences 数量可以不同；一般需要按 weight 排序后归一化。
    std::vector<std::vector<VertexInfluence>> skinInfluences;

    std::vector<MorphTarget>                  morphTargets;

    BoundingBox                               bbox;

     // ============== 扩展属性（第二阶段以后用） ==============
    std::vector<CustomAttribute>              customAttributes;
    // 预留下一代扩展槽：
    // std::vector<VertexAttribute>           vertexAttributes;   // 未来 USD-style Primvars
};

} // namespace mc