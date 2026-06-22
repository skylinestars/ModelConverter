#pragma once

#include "mc/pipeline/IPass.h"
#include "mc/common/Types.h"
#include <memory>
#include <vector>
#include <string>

namespace mc {

// ============================================================
// Pipeline —— Pass 执行器
// ============================================================
// 使用方式：
//   Pipeline pipeline;
//   pipeline.AddPass(std::make_unique<ValidatePass>());
//   pipeline.AddPass(std::make_unique<AxisConvertPass>());
//   VoidResult result = pipeline.Execute(scene);
//
// 执行策略：
//   - 按 AddPass 顺序依次执行
//   - 任意 Pass 返回 ok=false，立即停止，透传该错误
//   - 所有 Pass 的 warnings 累积合并到最终 VoidResult

class Pipeline
{
public:
    Pipeline()  = default;
    ~Pipeline() = default;

    // 禁止拷贝（Pass 持有 unique_ptr）
    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // 移动
    Pipeline(Pipeline&&)            noexcept = default;
    Pipeline& operator=(Pipeline&&) noexcept = default;

    // 追加一个 Pass 到队列末尾
    void AddPass(std::unique_ptr<IPass> pass);

    // 清空所有 Pass
    void Clear();

    // 已注册的 Pass 数量
    size_t PassCount() const;

    // 按顺序执行所有 Pass
    // 任意 Pass 失败则立即停止并返回其错误；
    // 全部成功时返回 ok=true，warnings 包含所有 Pass 的累积警告。
    VoidResult Execute(Scene& scene);

private:
    std::vector<std::unique_ptr<IPass>> m_passes;
};

} // namespace mc
