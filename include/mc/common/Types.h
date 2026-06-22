#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <limits>

namespace mc {

// ============================================================
// ObjectID
// ============================================================
using ObjectID = uint64_t;
constexpr ObjectID INVALID_ID = std::numeric_limits<ObjectID>::max();

// ============================================================
// Result<T> / VoidResult
// 简化版：ok/value/error 三段式。
// 将来可以无损替换为 std::expected（C++23）或 tl::expected。
// ============================================================
template <class T>
struct Result
{
    bool                     ok       = false;
    T                        value    = {};
    std::string              error;             // 致命错误（ok=false 时必填）
    std::vector<std::string> warnings;          // 非致命问题（ok=true 时也可能存在）

    explicit operator bool() const { return ok; }
};

template <>
struct Result<void>
{
    bool                     ok = false;
    std::string              error;
    std::vector<std::string> warnings;

    explicit operator bool() const { return ok; }
};

using VoidResult = Result<void>;

} // namespace mc