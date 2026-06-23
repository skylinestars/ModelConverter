#pragma once

#include "mc/pipeline/IPass.h"

namespace mc {

// ============================================================
// ValidatePass —— Scene 合法性验证
// ============================================================
// 必须作为 Pipeline 的第一个 Pass 执行，拦截 Importer 产生的脏数据。
// 只发现问题，不自动修复。修复由后续专门 Pass 负责。
//
// 检查项（共16条，见 ValidatePass.cpp）：
//   1.  所有 ObjectID 唯一，无重复
//   2.  rootNodes 中的 ID 均能在 nodes 中找到
//   3.  父子图无循环引用（DFS 检测环）
//   4.  Node::meshIds 中的 ID 均能在 meshes 中找到
//   5.  Node::cameraId 若有效，需在 cameras 中找到
//   6.  Node::lightId  若有效，需在 lights 中找到
//   7.  MeshSection::materialId 若有效，需在 materials 中找到
//   8.  indices 最大值 < positions.size()
//   9.  UV/Color 通道长度 == positions.size()（若非空）
//  10.  MorphTarget::positionDeltas.size() == positions.size()（若非空）
//  11.  TextureRef::textureId 若有效，需在 textures 中找到
//  12.  Skin::skeletonId 若有效，需在 skeletons 中找到
//  13.  Skin::meshId    若有效，需在 meshes 中找到
//  14.  PointInstancer::prototypeNodeId 若有效，需在 nodes 中找到
//  15.  NodeAnimation::nodeId 需在 nodes 中找到
//  16.  MorphAnimation::meshId 需在 meshes 中找到，且 morphIndex < mesh.morphTargets.size()

class ValidatePass : public IPass
{
public:
    VoidResult Execute(Scene& scene) override;
    std::string Name() const override { return "ValidatePass"; }
};

} // namespace mc
