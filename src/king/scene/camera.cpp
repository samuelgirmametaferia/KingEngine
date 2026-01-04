#include "camera.h"

#include "../math/dxmath.h"

#include <DirectXMath.h>

namespace king
{

static DirectX::XMVECTOR LoadQuat(const Float4& q)
{
    return DirectX::XMVectorSet(q.x, q.y, q.z, q.w);
}

static Float4 StoreQuat(DirectX::FXMVECTOR q)
{
    Float4 out{};
    DirectX::XMStoreFloat4((DirectX::XMFLOAT4*)&out, q);
    return out;
}

Camera::Camera() = default;

Float3 Camera::Forward() const
{
    // Left-handed: +Z forward
    const auto q = LoadQuat(mOrientation);
    const auto f = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 0, 1, 0), q);
    return dx::StoreFloat3(f);
}

Float3 Camera::Right() const
{
    const auto q = LoadQuat(mOrientation);
    const auto r = DirectX::XMVector3Rotate(DirectX::XMVectorSet(1, 0, 0, 0), q);
    return dx::StoreFloat3(r);
}

Float3 Camera::Up() const
{
    const auto q = LoadQuat(mOrientation);
    const auto u = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 1, 0, 0), q);
    return dx::StoreFloat3(u);
}

void Camera::TranslateLocal(const Float3& delta)
{
    const auto q = LoadQuat(mOrientation);
    const auto d = DirectX::XMVectorSet(delta.x, delta.y, delta.z, 0.0f);
    const auto worldDelta = DirectX::XMVector3Rotate(d, q);

    Float3 wd = dx::StoreFloat3(worldDelta);
    mPosition.x += wd.x;
    mPosition.y += wd.y;
    mPosition.z += wd.z;
}

void Camera::RotateYawPitchRoll(float yawRadians, float pitchRadians, float rollRadians)
{
    // Local-space yaw/pitch/roll using quaternion multiplication.
    // Yaw around local up, pitch around local right, roll around local forward.
    const auto q = LoadQuat(mOrientation);

    const auto up = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 1, 0, 0), q);
    const auto right = DirectX::XMVector3Rotate(DirectX::XMVectorSet(1, 0, 0, 0), q);
    const auto forward = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 0, 1, 0), q);

    const auto qYaw = DirectX::XMQuaternionRotationAxis(up, yawRadians);
    const auto qPitch = DirectX::XMQuaternionRotationAxis(right, pitchRadians);
    const auto qRoll = DirectX::XMQuaternionRotationAxis(forward, rollRadians);

    auto out = DirectX::XMQuaternionMultiply(q, qYaw);
    out = DirectX::XMQuaternionMultiply(out, qPitch);
    out = DirectX::XMQuaternionMultiply(out, qRoll);
    out = DirectX::XMQuaternionNormalize(out);

    mOrientation = StoreQuat(out);
}

void Camera::SetPerspective(const PerspectiveParams& p)
{
    mProjectionType = ProjectionType::Perspective;
    mPerspective = p;
}

void Camera::SetOrthographic(const OrthographicParams& o)
{
    mProjectionType = ProjectionType::Orthographic;
    mOrtho = o;
}

Mat4x4 Camera::ViewMatrix() const
{
    // Robust LH view matrix from eye + forward + up.
    const auto eye = DirectX::XMVectorSet(mPosition.x, mPosition.y, mPosition.z, 1.0f);
    const Float3 f = Forward();
    const Float3 u = Up();
    const auto forward = DirectX::XMVectorSet(f.x, f.y, f.z, 0.0f);
    const auto up = DirectX::XMVectorSet(u.x, u.y, u.z, 0.0f);
    const auto view = DirectX::XMMatrixLookToLH(eye, forward, up);
    return dx::StoreMat4x4(view);
}

Mat4x4 Camera::ProjectionMatrix() const
{
    if (mProjectionType == ProjectionType::Orthographic)
    {
        const auto proj = DirectX::XMMatrixOrthographicLH(mOrtho.width, mOrtho.height, mOrtho.nearZ, mOrtho.farZ);
        return dx::StoreMat4x4(proj);
    }

    const auto proj = DirectX::XMMatrixPerspectiveFovLH(
        mPerspective.verticalFovRadians,
        mPerspective.aspect,
        mPerspective.nearZ,
        mPerspective.farZ
    );
    return dx::StoreMat4x4(proj);
}

Mat4x4 Camera::ViewProjectionMatrix() const
{
    const auto v = dx::LoadMat4x4(ViewMatrix());
    const auto p = dx::LoadMat4x4(ProjectionMatrix());
    return dx::StoreMat4x4(v * p);
}

} // namespace king
