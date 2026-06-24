#pragma once

#include "mc/exporter/IExporter.h"

namespace mc {

// ============================================================
// FbxExporter
// ============================================================
// 将 mc::Scene 导出为 FBX（ASCII 或 Binary）。
// Phase12+14 范围：Node / Mesh / Material / Animation（TRS）。

class FbxExporter : public IExporter
{
public:
    bool CanExport(const std::string& ext) const override;
    VoidResult Export(const Scene& scene, ExportContext& ctx) override;
};

} // namespace mc
