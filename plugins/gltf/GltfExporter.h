#pragma once

#include "mc/exporter/IExporter.h"

namespace mc {

// ============================================================
// GltfExporter
// ============================================================
// 将 mc::Scene 导出为 GLTF 2.0（.gltf ASCII 或 .glb 二进制）。
// Phase11+14 范围：Node / Mesh / Material / Texture / Animation（TRS）。

class GltfExporter : public IExporter
{
public:
    bool CanExport(const std::string& ext) const override;
    VoidResult Export(const Scene& scene, ExportContext& ctx) override;
};

} // namespace mc
