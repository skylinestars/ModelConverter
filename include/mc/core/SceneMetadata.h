#pragma once

#include "mc/common/Types.h"
#include <string>
#include <unordered_map>

namespace mc {

// ============================================================
// SceneMetadata
// ============================================================
enum class Axis       { X, Y, Z };
enum class Handedness { Left, Right };

// 文件级别元信息（GLTF Asset / FBX DocumentInfo / USD metadata 对其映射）
struct AssetInfo
{
    std::string generator;    // "MC Converter v1.0"
    std::string sourceFormat; // "obj", "gltf", "fbx", "usd"
    std::string sourceFile;   // 原始文件名（调试/追溯用）
    std::string author;
    std::string copyright;
};

struct SceneMetadata
{
    AssetInfo    asset;                          // 文件级元信息

    std::string  unit       = "m";               // "m", "cm", "mm", "inch"
    float        unitScale  = 1.0f;              // 到米的换算系数（1.0 表示米）
    Axis         upAxis     = Axis::Y;           // Y 或 Z
    Axis         frontAxis  = Axis::Z;           // 用于判断方向
    Handedness   handedness = Handedness::Right; // Left / Right

    // 扩展：自定义 key-value 元数据
    std::unordered_map<std::string, std::string> custom;
};

} // namespace mc