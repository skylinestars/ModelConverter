#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"
#include <string>

namespace mc {

// ============================================================
// ExportOptions —— 导出行为参数
// ============================================================
struct ExportOptions
{
    bool embedTextures  = false;   // 纹理内嵌到文件（GLB 等格式适用）
    bool prettyPrint    = false;   // JSON 格式化（GLTF ASCII 适用）
    bool exportNormals  = true;
    bool exportUVs      = true;
};

// ============================================================
// ExportContext —— 导出上下文（运行时状态）
// ============================================================
struct ExportContext
{
    std::string outputPath;        // 导出目标路径
    ExportOptions options;

    // 统计信息（Export 完成后填充）
    size_t meshesExported     = 0;
    size_t materialsExported  = 0;
    size_t texturesExported   = 0;
    size_t nodesExported      = 0;
};

// ============================================================
// IExporter —— 导出器接口
// ============================================================
class IExporter
{
public:
    virtual ~IExporter() = default;

    // 判断是否支持该文件扩展名（小写，含点，如 ".gltf"）
    virtual bool CanExport(const std::string& extension) const = 0;

    // 将 scene 导出到 ctx.outputPath，返回 VoidResult
    virtual VoidResult Export(const Scene& scene, ExportContext& ctx) = 0;
};

} // namespace mc
