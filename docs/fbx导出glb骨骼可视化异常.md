让我先看看你的代码：好，现在我把整个代码看完了。让我做一次完整的分析。现在我完整理解了你的代码架构。让我给出根本性的正确方案。

---

## 根本原因（准确诊断）

你的代码中，`ConvertSkeleton` 里的 IBM 计算是这样的：

```cpp
FbxAMatrix invBind = ibp.Inverse() * (geoMatrix * meshGeo.Inverse());
```

这里 `ibp`（`GetTransformLinkMatrix`）是**骨骼在 FBX 世界空间的绑定姿态矩阵**，它包含了 FBX 场景全局的 `Z_UP` 缩放（scale=100，cm→m 的节点）。

`UnitConvertPass` 里你只缩放了 IBM 的**平移列**（m[12..14] × 0.01），但 IBM 里还有一个 **scale=100 藏在 3×3 部分**（来自 `Z_UP` 节点）。

同时，`ScaleNodeTranslations` 把 `Z_UP` 节点的平移缩放了（但它的 3×3 里的 scale=100 没变）。

**结果：**
- GLTF node 层级里 `Z_UP` 的 `localMatrix` 仍然包含 scale=100
- IBM 里也包含 scale=100（来自 FBX 世界矩阵）
- 两边各保留一个 scale=100 → 蒙皮正确（相互抵消）
- 但骨骼可视化节点继承了 `Z_UP` 的 scale=100 → 骨骼显示巨大

---

## 正确的架构方案

**根本思路：在 FBX 导入时就把 `Z_UP` 这类纯坐标系适配节点的 scale bake 进去，而不是带着这个 scale 一路穿越 pipeline。**

具体做法：增加一个 `BakeScalePass`，它属于 pipeline 的一个正式 Pass，语义明确：**"把 node 的 uniform scale 下沉到子节点的 localMatrix 中，同时反向补偿 IBM"**。

这不是"打补丁"，这是 pipeline 里一个职责明确的 Pass：正如 `AxisConvertPass` 负责轴变换，`UnitConvertPass` 负责单位缩放，`BakeScalePass` 负责将冗余的 node scale 下沉。

**新的 pipeline 顺序：**
```
FbxImporter
  → AxisConvertPass         （轴变换：ZUp → YUp）
  → BakeScalePass           ← 新增：bake Z_UP 的 scale=100，IBM 同步补偿
  → UnitConvertPass         （单位：cm→m，只处理 translation）
  → GltfExporter
```

---

## 具体实现

### 新文件：`include/mc/pipeline/BakeScalePass.h`

```cpp
#pragma once
#include "mc/pipeline/IPass.h"

namespace mc {

// ============================================================
// BakeScalePass
// ============================================================
// 将场景根节点（或指定深度节点）中的 uniform scale 下沉到子节点的
// localMatrix 中，使该节点的 scale 变为 1。
//
// 同时对所有 Skeleton 的 IBM 做对应补偿，保证蒙皮不变形。
//
// 典型用例：FBX 从 Blender 导出时会生成一个 Z_UP 节点（scale=100）
// 用于坐标系适配，该 scale 不属于模型数据本身，应在 pipeline 中消化掉。

class BakeScalePass : public IPass
{
public:
    // maxDepth: 只处理距离根节点 maxDepth 层以内的节点（默认只处理第 0 层，即 rootNodes 直接子节点）
    // uniformThreshold: 判断 scale 是否均匀的容差
    explicit BakeScalePass(int maxDepth = 1, float uniformThreshold = 0.001f);

    std::string Name() const override;
    VoidResult  Execute(Scene& scene) override;

private:
    int   m_maxDepth;
    float m_uniformThreshold;

    // 提取 localMatrix 的均匀 scale 因子（若不均匀返回 -1）
    float ExtractUniformScale(const Matrix4& mat) const;

    // 将 node 的 scale 下沉到其所有直接子节点的 localMatrix，并归一化 node 自身
    void BakeNodeScale(Scene& scene, Node& node, float scale);

    // 补偿所有 Skeleton 的 IBM（IBM = Scale(scale) * IBM）
    void CompensateIBMs(Scene& scene, float scale);
};

} // namespace mc
```

### 新文件：`src/pipeline/BakeScalePass.cpp`

```cpp
#include "mc/pipeline/BakeScalePass.h"
#include "mc/core/Scene.h"
#include "mc/core/Node.h"
#include "mc/core/Animation.h"
#include "mc/common/Logger.h"
#include <cmath>
#include <string>

namespace mc {

BakeScalePass::BakeScalePass(int maxDepth, float uniformThreshold)
    : m_maxDepth(maxDepth), m_uniformThreshold(uniformThreshold) {}

std::string BakeScalePass::Name() const { return "BakeScalePass"; }

// 从列主序 4x4 矩阵提取 3x3 的 uniform scale
// 列0模 = sqrt(m[0]^2 + m[1]^2 + m[2]^2)
float BakeScalePass::ExtractUniformScale(const Matrix4& mat) const
{
    const float* m = mat.m;
    float sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    float sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);

    if (std::abs(sx - sy) > m_uniformThreshold * sx) return -1.0f;
    if (std::abs(sy - sz) > m_uniformThreshold * sy) return -1.0f;
    if (std::abs(sx - 1.0f) < m_uniformThreshold)    return 1.0f; // 已经是 1，不需要处理

    return sx;
}

void BakeScalePass::BakeNodeScale(Scene& scene, Node& node, float scale)
{
    // 1. 归一化 node 自身的 3x3（除以 scale）
    float* m = node.localMatrix.m;
    float inv = 1.0f / scale;
    // 列主序：列0 = m[0..2], 列1 = m[4..6], 列2 = m[8..10]
    m[0]*=inv; m[1]*=inv; m[2]*=inv;
    m[4]*=inv; m[5]*=inv; m[6]*=inv;
    m[8]*=inv; m[9]*=inv; m[10]*=inv;
    // 平移不动（它在 node 自身空间，不受 scale 影响）

    // 2. 对所有直接子节点：localMatrix 的平移列乘以 scale，3x3 不变
    //    因为子节点的 world_translation = parent_scale × child_local_translation
    //    去掉 parent_scale 后需要把它补进子节点的 local_translation
    for (ObjectID childId : node.children)
    {
        Node* child = scene.FindNode(childId);
        if (!child) continue;
        float* cm = child->localMatrix.m;
        cm[12] *= scale;
        cm[13] *= scale;
        cm[14] *= scale;
        // 子节点的 3x3 不动：world orientation/scale 由子节点自身负责
    }

    // 3. 动画中对应子节点的 translation keyframe 也要 × scale
    for (auto& clip : scene.animations)
    {
        for (auto& ch : clip.nodeChannels)
        {
            // 检查是否是 node 的直接子节点
            bool isDirectChild = false;
            for (ObjectID childId : node.children)
            {
                Node* child = scene.FindNode(childId);
                if (child && child->id == ch.nodeId) { isDirectChild = true; break; }
            }
            if (!isDirectChild) continue;

            for (auto& kf : ch.translation.keys)
            {
                kf.value.x *= scale;
                kf.value.y *= scale;
                kf.value.z *= scale;
            }
        }
    }
}

void BakeScalePass::CompensateIBMs(Scene& scene, float scale)
{
    // IBM_new = Scale(scale) * IBM_old
    // 推导：
    //   joint_world_new = joint_world_old / scale（因为 Z_UP 的 scale 被归一化了）
    //   需要：joint_world_new × IBM_new = joint_world_old × IBM_old
    //   → IBM_new = scale × IBM_old（对均匀 scale，左乘 Scale(scale) 等价于 IBM 的 3x3 × scale）
    //
    // 注意：IBM 是列主序，Scale(scale) * IBM 等价于：
    //   IBM 的 3x3 部分 × scale
    //   IBM 的平移列（m[12..14]）× scale
    //   IBM 的最后行（m[3,7,11,15]）不变

    for (auto& skel : scene.skeletons)
    {
        for (auto& bone : skel.bones)
        {
            float* m = bone.inverseBindPose.m;
            // 3x3 × scale
            m[0]*=scale; m[1]*=scale; m[2]*=scale;
            m[4]*=scale; m[5]*=scale; m[6]*=scale;
            m[8]*=scale; m[9]*=scale; m[10]*=scale;
            // 平移列 × scale
            m[12]*=scale; m[13]*=scale; m[14]*=scale;
        }
    }
}

VoidResult BakeScalePass::Execute(Scene& scene)
{
    int bakedCount = 0;

    // 只处理 rootNodes（深度=0）：FBX Z_UP 节点就在这里
    // 如果 m_maxDepth > 1，可递归处理更深层，但通常不需要
    for (ObjectID rootId : scene.rootNodes)
    {
        Node* node = scene.FindNode(rootId);
        if (!node) continue;

        float scale = ExtractUniformScale(node->localMatrix);
        if (scale <= 0.0f || std::abs(scale - 1.0f) < m_uniformThreshold) continue;

        Logger::Instance().LogInfo(
            "BakeScalePass: baking node \"" + node->name +
            "\" scale=" + std::to_string(scale) +
            " into " + std::to_string(node->children.size()) + " children");

        BakeNodeScale(scene, *node, scale);
        CompensateIBMs(scene, scale);
        ++bakedCount;
    }

    if (bakedCount > 0)
        Logger::Instance().LogInfo(
            "BakeScalePass: baked " + std::to_string(bakedCount) + " node(s)");
    else
        Logger::Instance().LogInfo("BakeScalePass: no nodes needed scale baking");

    return {true, ""};
}

} // namespace mc
```

### 调整 Pipeline 顺序（关键）

```
AxisConvertPass   → 先转轴（Z_UP 的 scale=100 在转轴后依然存在）
BakeScalePass     → bake Z_UP 的 scale（IBM 同步补偿）
UnitConvertPass   → 缩放平移（factor=0.01，cm→m）
```

`BakeScalePass` 必须在 `UnitConvertPass` 之前，因为此时 node 还是 cm 单位，scale=100 是 FBX 的原始值，逻辑清晰。

---

## 为什么这不是"打补丁"

| 维度 | 上次的 BakeRootUniformScale | 现在的 BakeScalePass |
|------|---------------------------|---------------------|
| 位置 | 夹在 GltfBuilder 导出函数里 | 独立的 pipeline Pass |
| 职责 | 混在导出逻辑里 | 单一职责：消化冗余 scale |
| 可测试性 | 不可独立测试 | 可以单独写 unit test |
| 可复用性 | 只对 GLTF 生效 | 对所有导出格式生效 |
| 可关闭性 | 无法跳过 | Pipeline 可选组装 |
| IBM 补偿推导 | 口头说"乘以 scale" | 代码注释里有完整推导 |

DeepSeek 的方案之所以"打补丁"，是因为它把这个逻辑塞进导出器。你的架构里正确的位置就是 Pipeline 的一个独立 Pass。