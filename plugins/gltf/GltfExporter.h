#pragma once

#include "mc/exporter/IExporter.h"

namespace mc {

// ============================================================
// GltfExporter
// ============================================================
// 将 mc::Scene 导出为 GLTF 2.0（.gltf ASCII 或 .glb 二进制）。
// Phase11 范围：静态模型 Node / Mesh / Material / Texture。

class GltfExporter : public IExporter
{
public:
    bool CanExport(const std::string& ext) const override;
    VoidResult Export(const Scene& scene, ExportContext& ctx) override;
};

} // namespace mc
