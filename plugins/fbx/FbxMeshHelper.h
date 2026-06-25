#pragma once

#include "mc/common/Types.h"
#include "mc/core/Mesh.h"

namespace fbxsdk {
class FbxMesh;
class FbxAMatrix;
} // namespace fbxsdk

namespace mc {

// ============================================================
// FbxMeshHelper —— FbxMesh 扇形三角化工具
// ============================================================
// 将 FbxMesh 的任意多边形（四边形/N 边形）扇形三角化，
// 同时处理法线/UV 解包、坐标变换、顶点去重，写入 mc::Mesh。
//
// 职责边界：
//   - 输入：FbxMesh + GeometricTransform
//   - 输出：mc::Mesh 的 positions/normals/uvs/indices
//   - 可选输出：ctrlToOutputMap[ctrlIdx] = 该控制点对应的所有输出顶点索引列表
//     （用于 ConvertSkeleton 将蒙皮权重正确传播到去重后的顶点）
//   - 不处理：material、section 分配（由 Converter 负责）

VoidResult FanTriangulateMesh(fbxsdk::FbxMesh* fbxMesh,
                               const fbxsdk::FbxAMatrix& geoTransform,
                               Mesh& outMesh,
                               std::vector<std::vector<uint32_t>>* ctrlToOutputMap = nullptr);

} // namespace mc
