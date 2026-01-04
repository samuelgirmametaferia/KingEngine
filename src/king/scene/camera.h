#pragma once

#include "../math/types.h"

#include <cstdint>

namespace king
{

enum class ProjectionType : uint8_t
{
    Perspective = 0,
    Orthographic = 1,
};

struct PerspectiveParams
{
    float verticalFovRadians = 60.0f * 3.1415926535f / 180.0f;
    float aspect = 16.0f / 9.0f;
    float nearZ = 0.1f;
    float farZ = 2000.0f;
};

struct OrthographicParams
{
    float width = 20.0f;
    float height = 20.0f;
    float nearZ = 0.1f;
    float farZ = 2000.0f;
};

// 6DOF camera: position + orientation quaternion.
// Provides precise view/projection matrices.
class Camera
{
public:
    Camera();

    void SetPosition(const Float3& p) { mPosition = p; }
    Float3 Position() const { return mPosition; }

    // Absolute orientation as quaternion (x,y,z,w).
    void SetOrientation(const Float4& q) { mOrientation = q; }
    Float4 Orientation() const { return mOrientation; }

    // Local axes derived from orientation.
    Float3 Forward() const;
    Float3 Right() const;
    Float3 Up() const;

    // 6DOF movement: move along local axes.
    void TranslateLocal(const Float3& delta);

    // 6DOF rotation: yaw/pitch/roll in radians (applied in local space).
    void RotateYawPitchRoll(float yawRadians, float pitchRadians, float rollRadians);

    void SetPerspective(const PerspectiveParams& p);
    void SetOrthographic(const OrthographicParams& o);

    ProjectionType Projection() const { return mProjectionType; }

    Mat4x4 ViewMatrix() const;
    Mat4x4 ProjectionMatrix() const;
    Mat4x4 ViewProjectionMatrix() const;

private:
    Float3 mPosition{};
    Float4 mOrientation{ 0, 0, 0, 1 }; // quaternion

    ProjectionType mProjectionType = ProjectionType::Perspective;
    PerspectiveParams mPerspective{};
    OrthographicParams mOrtho{};
};

} // namespace king
