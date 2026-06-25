#include "FbxMeshHelper.h"

#include <fbxsdk.h>
#include <unordered_map>
#include <vector>

using namespace fbxsdk;

namespace mc {

namespace {

// 顶点去重 Key: (ctrlIdx << 32) | (polyIdx << 16) | localVertIdx
inline uint64_t PackKey(int ctrl, int poly, int vert)
{
    return (static_cast<uint64_t>(ctrl) << 32) |
           (static_cast<uint64_t>(poly) << 16) |
           static_cast<uint64_t>(vert);
}

// 从 FbxGeometryElement 读取法线数据
inline FbxVector4 ReadNormal(FbxGeometryElementNormal* normalElem,
                              int ctrlIdx, int vertexId)
{
    if (!normalElem) return FbxVector4(0, 1, 0, 0);

    int idx = (normalElem->GetMappingMode() == FbxGeometryElement::eByControlPoint)
              ? ctrlIdx : vertexId;
    if (normalElem->GetReferenceMode() == FbxGeometryElement::eIndexToDirect)
        idx = normalElem->GetIndexArray().GetAt(idx);
    return normalElem->GetDirectArray().GetAt(idx);
}

// 从 FbxGeometryElement 读取 UV 数据
inline FbxVector2 ReadUV(FbxGeometryElementUV* uvElem,
                          int ctrlIdx, int vertexId)
{
    if (!uvElem) return FbxVector2(0, 0);

    int idx = (uvElem->GetMappingMode() == FbxGeometryElement::eByControlPoint)
              ? ctrlIdx : vertexId;
    if (uvElem->GetReferenceMode() != FbxGeometryElement::eDirect)
        idx = uvElem->GetIndexArray().GetAt(idx);
    return uvElem->GetDirectArray().GetAt(idx);
}

} // namespace

// ============================================================
// FanTriangulateMesh
// ============================================================
VoidResult FanTriangulateMesh(FbxMesh* fbxMesh,
                               const FbxAMatrix& geoTransform,
                               Mesh& outMesh,
                               std::vector<std::vector<uint32_t>>* ctrlToOutputMap)
{
    if (!fbxMesh) return {false, "FbxMeshHelper: null mesh"};

    int ctrlCount = fbxMesh->GetControlPointsCount();
    int polyCount = fbxMesh->GetPolygonCount();
    if (ctrlCount == 0 || polyCount == 0)
        return {true, ""};  // 空 mesh，不是错误

    // 初始化控制点到输出顶点的映射（用于后续蒙皮权重传播）
    if (ctrlToOutputMap)
        ctrlToOutputMap->resize(ctrlCount);

    // 预计算多边形顶点偏移表（支持 ByPolygonVertex 映射模式）
    std::vector<int> polySizes(polyCount);
    std::vector<int> polyVertexOffsets(polyCount);
    int totalVerts = 0;
    for (int p = 0; p < polyCount; ++p)
    {
        polySizes[p] = fbxMesh->GetPolygonSize(p);
        polyVertexOffsets[p] = totalVerts;
        totalVerts += polySizes[p];
    }

    FbxVector4* ctrlPts = fbxMesh->GetControlPoints();
    FbxGeometryElementNormal* normalElem = fbxMesh->GetElementNormal(0);
    FbxGeometryElementUV*     uvElem     = fbxMesh->GetElementUV(0);
    if (uvElem) outMesh.uvs.emplace_back();

    std::unordered_map<uint64_t, uint32_t> vertexMap;

    // 扇形三角化：N 边形 → N-2 个三角形 (v0, v1, v2), (v0, v2, v3), ...
    for (int p = 0; p < polyCount; ++p)
    {
        int polySize = polySizes[p];
        int vertBase = polyVertexOffsets[p];
        if (polySize < 3) continue;  // 退化面，跳过

        // 读取多边形顶点控制点索引
        std::vector<int> verts(polySize);
        for (int vi = 0; vi < polySize; ++vi)
            verts[vi] = fbxMesh->GetPolygonVertex(p, vi);

        // 生成 N-2 个三角形
        for (int ti = 0; ti < polySize - 2; ++ti)
        {
            int localVerts[3] = { 0, ti + 1, ti + 2 };

            for (int tv = 0; tv < 3; ++tv)
            {
                int localIdx = localVerts[tv];
                int ctrlIdx  = verts[localIdx];
                int vertexId = vertBase + localIdx;

                uint64_t key = PackKey(ctrlIdx, p, localIdx);
                auto mapIt = vertexMap.find(key);

                uint32_t outIdx;
                if (mapIt != vertexMap.end())
                {
                    outIdx = mapIt->second;
                }
                else
                {
                    outIdx = static_cast<uint32_t>(outMesh.positions.size());

                    // 位置（几何变换）
                    FbxVector4 pt = geoTransform.MultT(ctrlPts[ctrlIdx]);
                    outMesh.positions.push_back(
                        {static_cast<float>(pt[0]),
                         static_cast<float>(pt[1]),
                         static_cast<float>(pt[2])});

                    // 法线（几何旋转变换 + 归一化）
                    FbxVector4 n = geoTransform.MultR(ReadNormal(normalElem, ctrlIdx, vertexId));
                    double nLen = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                    if (nLen > 1e-12) { n[0]/=nLen; n[1]/=nLen; n[2]/=nLen; }
                    outMesh.normals.push_back(
                        {static_cast<float>(n[0]),
                         static_cast<float>(n[1]),
                         static_cast<float>(n[2])});

                    // UV（V 轴翻转）
                    if (uvElem)
                    {
                        FbxVector2 uv = ReadUV(uvElem, ctrlIdx, vertexId);
                        outMesh.uvs[0].push_back(
                            {static_cast<float>(uv[0]),
                             static_cast<float>(1.0 - uv[1])});
                    }

                    vertexMap[key] = outIdx;

                    // 记录控制点到输出顶点的映射（用于蒙皮权重传播）
                    if (ctrlToOutputMap)
                        (*ctrlToOutputMap)[ctrlIdx].push_back(outIdx);
                }

                outMesh.indices.push_back(outIdx);
            }
        }
    }

    return {true, ""};
}

} // namespace mc
