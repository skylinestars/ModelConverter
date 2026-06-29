#include "FbxMeshHelper.h"

#include <fbxsdk.h>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cmath>

using namespace fbxsdk;

namespace mc {

namespace {

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

// ============================================================
// 顶点去重 Key：基于实际数据（ctrlIdx + 变换后法线 + UV bit 模式）
// 两个顶点满足"相同位置 + 相同法线 + 相同UV"才共享输出顶点。
// 使用 memcpy 将 float 转为 uint32 比特模式，避免 NaN/浮点比较歧义。
// ============================================================
struct VertexKey
{
    int      ctrlIdx;
    uint32_t nx, ny, nz;   // 变换后法线 float → uint32 bit 模式
    uint32_t uvU, uvV;     // UV float → uint32 bit 模式（无UV时为 0）

    bool operator==(const VertexKey& o) const noexcept
    {
        return ctrlIdx == o.ctrlIdx &&
               nx == o.nx && ny == o.ny && nz == o.nz &&
               uvU == o.uvU && uvV == o.uvV;
    }
};

struct VertexKeyHash
{
    size_t operator()(const VertexKey& k) const noexcept
    {
        // FNV-1a 混合所有字段
        size_t h = 2166136261u;
        auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
        mix(static_cast<uint32_t>(k.ctrlIdx));
        mix(k.nx); mix(k.ny); mix(k.nz);
        mix(k.uvU); mix(k.uvV);
        return h;
    }
};

inline VertexKey MakeVertexKey(int ctrlIdx,
                                float nxf, float nyf, float nzf,
                                float uvU, float uvV)
{
    VertexKey k;
    k.ctrlIdx = ctrlIdx;
    std::memcpy(&k.nx,  &nxf, 4);
    std::memcpy(&k.ny,  &nyf, 4);
    std::memcpy(&k.nz,  &nzf, 4);
    std::memcpy(&k.uvU, &uvU, 4);
    std::memcpy(&k.uvV, &uvV, 4);
    return k;
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

    // 去重 map：key = (ctrlIdx + 变换后法线 + UV)，value = 输出顶点索引
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexMap;

    for (int p = 0; p < polyCount; ++p)
    {
        int polySize = polySizes[p];
        int vertBase = polyVertexOffsets[p];
        if (polySize < 3) continue;  // 退化面，跳过

        // 读取多边形顶点控制点索引
        std::vector<int> verts(polySize);
        for (int vi = 0; vi < polySize; ++vi)
            verts[vi] = fbxMesh->GetPolygonVertex(p, vi);

        // 顶点发射 lambda：按实际数据查 map / 新建输出顶点，推入 indices
        // key 基于实际变换后数据，相同位置+法线+UV 的顶点会被正确合并（smooth shading），
        // 不同法线（硬边）的顶点保持独立（hard edge）。
        auto emitVertex = [&](int localIdx)
        {
            int ctrlIdx  = verts[localIdx];
            int vertexId = vertBase + localIdx;

            // 计算变换后数据（法线需要先归一化）
            FbxVector4 pt = geoTransform.MultT(ctrlPts[ctrlIdx]);

            FbxVector4 n = geoTransform.MultR(ReadNormal(normalElem, ctrlIdx, vertexId));
            double nLen = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (nLen > 1e-12) { n[0]/=nLen; n[1]/=nLen; n[2]/=nLen; }
            float nxf = (float)n[0], nyf = (float)n[1], nzf = (float)n[2];

            float uvU = 0.0f, uvV = 0.0f;
            if (uvElem)
            {
                FbxVector2 uv = ReadUV(uvElem, ctrlIdx, vertexId);
                uvU = (float)uv[0];
                uvV = (float)(1.0 - uv[1]);
            }

            VertexKey key = MakeVertexKey(ctrlIdx, nxf, nyf, nzf, uvU, uvV);
            auto mapIt    = vertexMap.find(key);

            uint32_t outIdx;
            if (mapIt != vertexMap.end())
            {
                outIdx = mapIt->second;
            }
            else
            {
                outIdx = static_cast<uint32_t>(outMesh.positions.size());

                outMesh.positions.push_back(
                    {static_cast<float>(pt[0]),
                     static_cast<float>(pt[1]),
                     static_cast<float>(pt[2])});
                outMesh.normals.push_back({nxf, nyf, nzf});

                if (uvElem)
                    outMesh.uvs[0].push_back({uvU, uvV});

                vertexMap[key] = outIdx;

                if (ctrlToOutputMap)
                    (*ctrlToOutputMap)[ctrlIdx].push_back(outIdx);
            }

            outMesh.indices.push_back(outIdx);
        };

        if (polySize == 3)
        {
            // 已是三角形（FBX SDK Triangulate 后的正常情况），直接输出
            emitVertex(0); emitVertex(1); emitVertex(2);
        }
        else
        {
            // 扇形三角化兜底（N > 3，SDK Triangulate 后理论上不应出现）
            for (int ti = 0; ti < polySize - 2; ++ti)
            {
                emitVertex(0);
                emitVertex(ti + 1);
                emitVertex(ti + 2);
            }
        }
    }

    return {true, ""};
}

} // namespace mc
