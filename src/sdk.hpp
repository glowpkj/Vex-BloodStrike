// -----------------------------------------------------------------------------
//  sdk.hpp  --  math, UE4 type primitives, world-to-screen
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <array>

namespace vex {

constexpr float kPi        = 3.14159265358979323846f;
constexpr float kDeg2Rad   = kPi / 180.0f;

// --------------------------------------------------------------------- math
struct Vec2 { float x = 0, y = 0; };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    constexpr Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    constexpr Vec3 operator*(float s)         const { return { x * s, y * s, z * s }; }
    float length()  const { return std::sqrt(x * x + y * y + z * z); }
    float length2d() const { return std::sqrt(x * x + y * y); }
};

struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

struct Rotator {
    float pitch = 0, yaw = 0, roll = 0;
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}

// UE basis vectors from an FRotator (roll = X, pitch = Y, yaw = Z).
inline Vec3 forward(const Rotator& r) {
    float sp = std::sin(r.pitch * kDeg2Rad), cp = std::cos(r.pitch * kDeg2Rad);
    float sy = std::sin(r.yaw   * kDeg2Rad), cy = std::cos(r.yaw   * kDeg2Rad);
    return { cp * cy, cp * sy, sp };
}
inline Vec3 right(const Rotator& r) {
    float sp = std::sin(r.pitch * kDeg2Rad), cp = std::cos(r.pitch * kDeg2Rad);
    float sy = std::sin(r.yaw   * kDeg2Rad), cy = std::cos(r.yaw   * kDeg2Rad);
    float sr = std::sin(r.roll   * kDeg2Rad), cr = std::cos(r.roll   * kDeg2Rad);
    return { cr * sy + sr * sp * cy,
            -cr * cy + sr * sp * sy,
            -sr * cp };
}
inline Vec3 up(const Rotator& r) {
    float sp = std::sin(r.pitch * kDeg2Rad), cp = std::cos(r.pitch * kDeg2Rad);
    float sy = std::sin(r.yaw   * kDeg2Rad), cy = std::cos(r.yaw   * kDeg2Rad);
    float sr = std::sin(r.roll   * kDeg2Rad), cr = std::cos(r.roll   * kDeg2Rad);
    return { -sr * sy + cr * sp * cy,
             sr * cy + cr * sp * sy,
             cr * cp };
}

struct ScreenInfo {
    int    width  = 0;
    int    height = 0;
    Vec3   location;
    Rotator rotation;
    float  fov    = 90.f;
    bool   valid  = false;

    /// Combined view-projection, taken straight from the camera object.
    /// When has_vp is set the projection path skips the camera-basis maths.
    float  vp[16] = {};
    bool   has_vp = false;
};

inline float fov_scale(float fov_deg) {
    // UE stores horizontal FOV; convert to the tangent used by the projection
    float h = std::tan(fov_deg * kDeg2Rad * 0.5f);
    return h > 0.0001f ? 1.0f / h : 1.0f;
}

/// Project through a combined view-projection matrix (row-major float[4][4]).
/// A negative w means the point is behind the eye plane.
inline bool project_matrix(const ScreenInfo& s, const Vec3& world, Vec2& out) {
    if (!s.has_vp || s.width <= 0 || s.height <= 0) return false;

    const float* m = s.vp;
    float cx = m[0]  * world.x + m[1]  * world.y + m[2]  * world.z + m[3];
    float cy = m[4]  * world.x + m[5]  * world.y + m[6]  * world.z + m[7];
    float cw = m[12] * world.x + m[13] * world.y + m[14] * world.z + m[15];

    if (cw < 0.05f) return false;

    out.x = (cx / cw * 0.5f + 0.5f) * (float)s.width;
    out.y = (0.5f - cy / cw * 0.5f) * (float)s.height;
    return true;
}

/// Project a world point into screen space.  Prefers the combined matrix when
/// one was read; otherwise reconstructs from the camera basis.  Returns false
/// when the point is behind the camera plane.
inline bool world_to_screen(const Vec3& world, const ScreenInfo& s, Vec2& out) {
    if (!s.valid || s.width <= 0 || s.height <= 0) return false;

    if (s.has_vp) {
        if (project_matrix(s, world, out)) return true;
        // A matrix that fails here is not a view-projection; fall through to
        // the basis path rather than dropping the point silently.
    }

    Vec3  rel = world - s.location;
    Vec3  fwd = forward(s.rotation);
    float z   = dot(rel, fwd);
    if (z < 0.05f) return false;                        // behind the camera

    float h = fov_scale(s.fov);
    float x = dot(rel, right(s.rotation));
    float y = dot(rel, up(s.rotation));

    float ndc_x = (x * h) / z;
    float ndc_y = (y * h) / z;

    // FOV is horizontal -> aspect-correct Y so the box is not stretched.
    float aspect = (float)s.width / (float)s.height;
    ndc_y /= aspect;

    out.x = ndc_x * (s.width  * 0.5f) + s.width  * 0.5f;
    out.y = -ndc_y * (s.height * 0.5f) + s.height * 0.5f;
    return true;
}

/// Rotate a world-space delta into a 2D screen plane anchored at the camera.
/// Used for box corners so boxes stay axis-aligned to the view, not the world.
inline void project_delta(const Vec3& world_delta, const ScreenInfo& s, Vec2& out) {
    Vec3  fwd = forward(s.rotation);
    float z   = dot(world_delta, fwd);
    if (z < 0.05f) z = 0.05f;
    float h = fov_scale(s.fov);
    float aspect = (float)s.width / (float)s.height;
    float ndc_x = (dot(world_delta, right(s.rotation)) * h) / z;
    float ndc_y = (dot(world_delta, up(s.rotation))    * h) / z / aspect;
    out.x = ndc_x * (s.width  * 0.5f);
    out.y = -ndc_y * (s.height * 0.5f);
}

// ----------------------------------------------------------------- UE types
struct FName {
    uint32_t comparison_index = 0;
    uint32_t number           = 0;
    bool operator==(const FName& o) const { return comparison_index == o.comparison_index; }
};

struct FVector3d { double x = 0, y = 0, z = 0; };   // LWC Location components

struct FTransform {
    Vec4 rotation;
    Vec4 translation_scaled;   // (x,y,z) * 1/10000 (world origin rebasing) + w = 1
};

struct FMinimalViewInfo {
    Vec3    location;
    Rotator rotation;
    float   fov;
};

// ------------------------------------------------------------------ helpers
template <typename T>
inline T round_up(T v, T a) { return (v + a - 1) / a * a; }

} // namespace vex
