#pragma once

#include "mc/common/Types.h"
#include "mc/core/Scene.h"
#include <string>

namespace mc {

// ============================================================
// IPass —— Pipeline Pass 接口
// ============================================================
// 每个 Pass 接收一个 Scene 引用，对其做原地修改（或只读检查），
// 返回 VoidResult：
//   ok=true  → 执行成功（warnings 可非空）
//   ok=false → 执行失败，Pipeline 立即终止并向上透传错误

class IPass
{
public:
    virtual ~IPass() = default;

    // 执行 Pass，Scene 是可读写的（ValidatePass 只读，AxisConvertPass 会修改）
    virtual VoidResult Execute(Scene& scene) = 0;

    // Pass 名称，用于日志和错误定位
    virtual std::string Name() const = 0;
};

} // namespace mc
