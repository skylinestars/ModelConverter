#pragma once

#include <cmath>
#include <cstring>
#include <cassert>

namespace mc {

// ============================================================
// Vec2
// ============================================================
struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float v) : x(v), y(v) {}
    Vec2(float x, float y) : x(x), y(y) {}

    float& operator[](int i)       { return (&x)[i]; }
    float  operator[](int i) const { return (&x)[i]; }
};

inline Vec2 operator+(const Vec2& a, const Vec2& b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(const Vec2& a, const Vec2& b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(const Vec2& a, float s)       { return {a.x * s, a.y * s}; }
inline Vec2 operator*(float s, const Vec2& a)       { return {a.x * s, a.y * s}; }

// ============================================================
// Vec3
// ============================================================
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float v) : x(v), y(v), z(v) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    float& operator[](int i)       { return (&x)[i]; }
    float  operator[](int i) const { return (&x)[i]; }
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s)       { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, const Vec3& a)       { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator-(const Vec3& a)                { return {-a.x, -a.y, -a.z}; }

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3& v)
{
    float len = Length(v);
    return (len > 0.0f) ? v * (1.0f / len) : Vec3{};
}

// ============================================================
// Vec4
// ============================================================
struct Vec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    Vec4() = default;
    Vec4(float v) : x(v), y(v), z(v), w(v) {}
    Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    Vec4(const Vec3& xyz, float w) : x(xyz.x), y(xyz.y), z(xyz.z), w(w) {}

    float& operator[](int i)       { return (&x)[i]; }
    float  operator[](int i) const { return (&x)[i]; }
};

// ============================================================
// Quaternion
// ============================================================
struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    Quaternion() = default;
    Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    static Quaternion Identity() { return {0, 0, 0, 1}; }
};

// ============================================================
// Color4
// ============================================================
struct Color4
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    Color4() = default;
    Color4(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {}
    Color4(float v) : r(v), g(v), b(v), a(1.0f) {}
};

// ============================================================
// Matrix4 (column-major, 4x4)
// ============================================================
struct Matrix4
{
    float m[16]{};

    Matrix4()
    {
        // Identity
        m[0] = 1; m[5] = 1; m[10] = 1; m[15] = 1;
    }

    Matrix4(
        float m00, float m01, float m02, float m03,
        float m10, float m11, float m12, float m13,
        float m20, float m21, float m22, float m23,
        float m30, float m31, float m32, float m33)
    {
        m[0] = m00; m[1] = m01; m[2] = m02; m[3] = m03;
        m[4] = m10; m[5] = m11; m[6] = m12; m[7] = m13;
        m[8] = m20; m[9] = m21; m[10]= m22; m[11]= m23;
        m[12]= m30; m[13]= m31; m[14]= m32; m[15]= m33;
    }

    float& operator()(int row, int col)       { return m[col * 4 + row]; }
    float  operator()(int row, int col) const { return m[col * 4 + row]; }

    static Matrix4 Identity() { return {}; }
};

// ============================================================
// BoundingBox
// ============================================================
struct BoundingBox
{
    Vec3 min{};
    Vec3 max{};

    BoundingBox() = default;
    BoundingBox(const Vec3& min, const Vec3& max) : min(min), max(max) {}

    bool IsValid() const
    {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    void Expand(const Vec3& point)
    {
        if (!IsValid()) { min = max = point; return; }
        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    }
};

} // namespace mc