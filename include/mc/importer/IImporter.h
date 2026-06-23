#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"
#include <string>

namespace mc {

// ============================================================
// IImporter —— 导入器接口
// ============================================================
// Plugin_Assimp 及后续所有 Importer 实现此接口。
// Import() 将文件内容填入 scene，返回 VoidResult。
// scene 由调用方提供，Import 仅追加数据（不清空 scene）。

class IImporter
{
public:
    virtual ~IImporter() = default;

    // 判断是否支持该文件扩展名（小写，含点，如 ".obj"）
    virtual bool CanImport(const std::string& extension) const = 0;

    // 将 path 指向的文件内容导入到 scene 中
    virtual VoidResult Import(const std::string& path, Scene& scene) = 0;
};

} // namespace mc
