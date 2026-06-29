#pragma once

#include "mc/common/Types.h"
#include <string>
#include <vector>

namespace mc {

// ============================================================
// Texture
// ============================================================
enum class TextureWrap
{
    ClampToEdge,
    Repeat,
    MirroredRepeat
};

enum class TextureFilter
{
    Nearest,
    Linear,
    NearestMipmapNearest,
    NearestMipmapLinear,
    LinearMipmapNearest,
    LinearMipmapLinear
};

struct TextureSampler
{
    TextureFilter  minFilter = TextureFilter::LinearMipmapLinear;
    TextureFilter  magFilter = TextureFilter::Linear;
    TextureWrap    wrapS     = TextureWrap::Repeat;
    TextureWrap    wrapT     = TextureWrap::Repeat;
};

enum class ColorSpace
{
    Auto,
    SRGB,
    Linear,
    Raw
};

struct Texture
{
    ObjectID                   id;
    std::string                name;

    // 二选一：uri（外置） 或 embeddedData（内嵌）
    std::string                uri;  // 相对路径或绝对路径（UTF-8）
    bool                       embedded = false;
    std::vector<uint8_t>       embeddedData;  // PNG/JPG/WebP 原始字节
    std::string                mimeType;  // "image/png", "image/jpeg" ...

    TextureSampler             sampler;
    ColorSpace                 colorSpace = ColorSpace::Auto;
};

} // namespace mc