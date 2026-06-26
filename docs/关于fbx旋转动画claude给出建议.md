看了一下，整体思路是对的，但确实有几处可以简化或改进：

**主要问题**

1. `QuatToEulerDegrees` 里 `std::sqrt(len2)` 算了三次，应该算一次
2. `GenerateCandidates` 生成候选的策略不够系统——你只对单轴加减360，但真正需要的是**逐轴独立 unwrap**，即对每个分量单独找最近的 ±360 倍数，这才是标准做法，也更简单
3. `AltEuler`（你没贴出来）通常是翻转 Y 轴的替代解，但如果 unwrap 做对了，大多数情况下不需要它

**简化后的版本**

```cpp
static FbxDouble3 QuatToEulerDegrees(const Quaternion& q)
{
    float invLen = 1.0f / std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    float x = q.x * invLen, y = q.y * invLen, z = q.z * invLen, w = q.w * invLen;

    float sinRy = 2.0f * (w*y - z*x);
    sinRy = std::clamp(sinRy, -1.0f, 1.0f); // 比 copysign 更简洁

    constexpr float kRadToDeg = 180.0f / 3.14159265358979f;
    return FbxDouble3(
        std::atan2(2.0f*(w*x + y*z), 1.0f - 2.0f*(x*x + y*y)) * kRadToDeg,
        std::asin(sinRy) * kRadToDeg,
        std::atan2(2.0f*(w*z + x*y), 1.0f - 2.0f*(y*y + z*z)) * kRadToDeg
    );
}

// 对每个分量独立 unwrap 到距 prev 最近的值
static FbxDouble3 UnwrapEuler(const FbxDouble3& curr, const FbxDouble3& prev)
{
    FbxDouble3 result = curr;
    for (int i = 0; i < 3; ++i)
    {
        double diff = result[i] - prev[i];
        // 将 diff 折叠到 (-180, 180]
        result[i] -= std::floor((diff + 180.0) / 360.0) * 360.0;
    }
    return result;
}
```

主循环里只需：

```cpp
FbxDouble3 euler = QuatToEulerDegrees(kf.value);
if (hasPrev)
    euler = UnwrapEuler(euler, prevEuler);
prevEuler = euler;
```

`GenerateCandidates` 和候选遍历可以完全删掉。

---

你的 `AltEuler` 公式是对的，但归一化逻辑只处理了一次偏移，而 `x+180` 或 `z+180` 可能超出范围后还需要再折叠——虽然实际上 `(-180+180=0, 180+180=360→0)` 刚好在边界，单次够用，但不够健壮。

更重要的是，结合我上面说的，**可以把 AltEuler + UnwrapEuler 合并成一个函数**，逻辑更清晰：

```cpp
// 归一化单个角度到 (-180, 180]
static double NormalizeAngle(double a)
{
    return a - std::floor((a + 180.0) / 360.0) * 360.0;
}

// EulerXYZ 替代解：(X+180, 180-Y, Z+180)
static FbxDouble3 AltEuler(const FbxDouble3& e)
{
    return FbxDouble3(
        NormalizeAngle(e[0] + 180.0),
        NormalizeAngle(180.0 - e[1]),
        NormalizeAngle(e[2] + 180.0)
    );
}

// 选出与 prev 距离最近的欧拉表示（主解 or 替代解），再逐轴 unwrap
static FbxDouble3 UnwrapEuler(const FbxDouble3& curr, const FbxDouble3& prev)
{
    // 两个候选：主解和替代解
    FbxDouble3 candidates[2] = { curr, AltEuler(curr) };

    auto sqDist = [](const FbxDouble3& a, const FbxDouble3& b) {
        double dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
        return dx*dx + dy*dy + dz*dz;
    };

    // 对每个候选先做逐轴 unwrap，再比距离
    for (auto& c : candidates)
        for (int i = 0; i < 3; ++i)
            c[i] -= std::floor((c[i] - prev[i] + 180.0) / 360.0) * 360.0;

    return sqDist(candidates[0], prev) <= sqDist(candidates[1], prev)
        ? candidates[0] : candidates[1];
}
```

主循环不变：

```cpp
FbxDouble3 euler = QuatToEulerDegrees(kf.value);
if (hasPrev)
    euler = UnwrapEuler(euler, prevEuler);
prevEuler = euler;
```

**关键改进点：**

- 先 unwrap 再比距离，而不是先比距离再 unwrap——原来你的候选集是在原始值上比较的，unwrap 之后距离才有意义
- `NormalizeAngle` 用 `floor` 实现，对任意输入都正确，不依赖单次偏移的边界假设
- 候选只需要主解和替代解两个，加上 unwrap 后的 ±360 已经隐含在 `floor` 里，不需要手动枚举



