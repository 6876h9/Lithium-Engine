#pragma once

#include <cmath>
#include <array>

struct Vector2 {
    float x = 0.0f;
    float y = 0.0f;

    Vector2() = default;
    Vector2(float x, float y) : x(x), y(y) {}

    Vector2 operator+(const Vector2& other) const { return {x + other.x, y + other.y}; }
    Vector2 operator-(const Vector2& other) const { return {x - other.x, y - other.y}; }
    Vector2 operator*(float scalar) const { return {x * scalar, y * scalar}; }
};

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3 operator+(const Vector3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vector3 operator-(const Vector3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vector3 operator*(float scalar) const { return {x * scalar, y * scalar, z * scalar}; }
    Vector3 operator*(const Vector3& other) const { return {x * other.x, y * other.y, z * other.z}; }
    Vector3 operator/(float scalar) const { return {x / scalar, y / scalar, z / scalar}; }

    Vector3& operator+=(const Vector3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    Vector3& operator-=(const Vector3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }
    Vector3& operator*=(float scalar) { x *= scalar; y *= scalar; z *= scalar; return *this; }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vector3 normalized() const {
        float len = length();
        if (len > 0.0f) {
            return {x / len, y / len, z / len};
        }
        return {0.0f, 0.0f, 0.0f};
    }

    static float dot(const Vector3& a, const Vector3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vector3 cross(const Vector3& a, const Vector3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
};

// Four-component vector. Used as a quaternion (x, y, z, w) by the animation
// system, which is why normalized() divides all four components rather than
// treating w as a homogeneous divisor.
struct Vector4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    Vector4 operator+(const Vector4& other) const { return {x + other.x, y + other.y, z + other.z, w + other.w}; }
    Vector4 operator-(const Vector4& other) const { return {x - other.x, y - other.y, z - other.z, w - other.w}; }
    Vector4 operator*(float scalar) const { return {x * scalar, y * scalar, z * scalar, w * scalar}; }

    float length() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    Vector4 normalized() const {
        float len = length();
        if (len > 0.0f) {
            return {x / len, y / len, z / len, w / len};
        }
        // Identity quaternion, so a degenerate key can't collapse a bone to zero.
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    // --- Quaternion algebra -------------------------------------------------
    // Blending poses is done in local TRS space rather than on matrices: a matrix
    // lerp shears and shrinks the bone halfway through the blend, which is what
    // makes a naively cross-faded character visibly collapse mid-transition.

    static Vector4 quat_identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    static float quat_dot(const Vector4& a, const Vector4& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    // Hamilton product: the rotation of b followed by the rotation of a.
    static Vector4 quat_mul(const Vector4& a, const Vector4& b) {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        };
    }

    // Inverse of a unit quaternion. Animation keys are always unit length, so the
    // conjugate is the inverse and the divide by the squared norm is skipped.
    static Vector4 quat_conjugate(const Vector4& q) {
        return {-q.x, -q.y, -q.z, q.w};
    }

    // Shortest-arc spherical interpolation. The sign flip matters: two quaternions
    // describing nearly the same orientation can differ in sign, and interpolating
    // between them without it sends the bone the long way around.
    static Vector4 quat_slerp(const Vector4& a_in, const Vector4& b_in, float t) {
        Vector4 a = a_in;
        Vector4 b = b_in;
        float dot = quat_dot(a, b);
        if (dot < 0.0f) {
            b = { -b.x, -b.y, -b.z, -b.w };
            dot = -dot;
        }
        if (dot > 0.9995f) {
            // The arc is too short for the sin() form to be well conditioned, and a
            // straight lerp is indistinguishable from the arc at this separation.
            Vector4 r = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                          a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
            return r.normalized();
        }
        dot = (dot > 1.0f) ? 1.0f : dot;
        float theta = std::acos(dot);
        float sin_theta = std::sin(theta);
        float wa = std::sin((1.0f - t) * theta) / sin_theta;
        float wb = std::sin(t * theta) / sin_theta;
        return { wa * a.x + wb * b.x, wa * a.y + wb * b.y,
                 wa * a.z + wb * b.z, wa * a.w + wb * b.w };
    }
};

struct DVector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    DVector3 operator+(const DVector3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    DVector3 operator-(const DVector3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    DVector3 operator*(double scalar) const { return {x * scalar, y * scalar, z * scalar}; }
    DVector3 operator*(const DVector3& other) const { return {x * other.x, y * other.y, z * other.z}; }
    DVector3 operator/(double scalar) const { return {x / scalar, y / scalar, z / scalar}; }

    DVector3& operator+=(const DVector3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    DVector3& operator-=(const DVector3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }
    DVector3& operator*=(double scalar) { x *= scalar; y *= scalar; z *= scalar; return *this; }

    double length() const { return std::sqrt(x * x + y * y + z * z); }
    DVector3 normalized() const {
        double len = length();
        if (len > 0.0) {
            return {x / len, y / len, z / len};
        }
        return {0.0, 0.0, 0.0};
    }

    // Conversion to float Vector3
    Vector3 to_vec3() const {
        return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    }
};

struct Matrix4x4 {
    std::array<float, 16> m = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    static Matrix4x4 identity() {
        return Matrix4x4();
    }

    static Matrix4x4 translation(const Vector3& v) {
        Matrix4x4 mat;
        mat.m[12] = v.x;
        mat.m[13] = v.y;
        mat.m[14] = v.z;
        return mat;
    }

    static Matrix4x4 rotationY(float angleRadians) {
        Matrix4x4 mat;
        float c = std::cos(angleRadians);
        float s = std::sin(angleRadians);
        mat.m[0] = c;   mat.m[2] = s;
        mat.m[8] = -s;  mat.m[10] = c;
        return mat;
    }

    static Matrix4x4 rotationX(float angleRadians) {
        Matrix4x4 mat;
        float c = std::cos(angleRadians);
        float s = std::sin(angleRadians);
        mat.m[5] = c;   mat.m[6] = -s;
        mat.m[9] = s;   mat.m[10] = c;
        return mat;
    }

    static Matrix4x4 rotationZ(float angleRadians) {
        Matrix4x4 mat;
        float c = std::cos(angleRadians);
        float s = std::sin(angleRadians);
        mat.m[0] = c;   mat.m[1] = -s;
        mat.m[4] = s;   mat.m[5] = c;
        return mat;
    }

    static Matrix4x4 scale(const Vector3& v) {
        Matrix4x4 mat;
        mat.m[0] = v.x;
        mat.m[5] = v.y;
        mat.m[10] = v.z;
        return mat;
    }

    // Rotation matrix for a quaternion (x, y, z, w). Animation keys are stored as
    // quaternions rather than Euler angles because slerp between two Eulers goes
    // through gimbal lock and takes the wrong arc; nothing else in the engine needs
    // this, but every rotation channel of every clip is sampled through it.
    static Matrix4x4 from_quaternion(const Vector4& q_in) {
        Vector4 q = q_in.normalized();
        Matrix4x4 mat;
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

        // Column-major: element (row, col) lives at m[col * 4 + row].
        mat.m[0]  = 1.0f - 2.0f * (yy + zz);
        mat.m[1]  =        2.0f * (xy + wz);
        mat.m[2]  =        2.0f * (xz - wy);

        mat.m[4]  =        2.0f * (xy - wz);
        mat.m[5]  = 1.0f - 2.0f * (xx + zz);
        mat.m[6]  =        2.0f * (yz + wx);

        mat.m[8]  =        2.0f * (xz + wy);
        mat.m[9]  =        2.0f * (yz - wx);
        mat.m[10] = 1.0f - 2.0f * (xx + yy);
        return mat;
    }

    // Composes a bone's local transform as T * R * S, matching Transform::get_matrix().
    static Matrix4x4 from_trs(const Vector3& t, const Vector4& r, const Vector3& s) {
        Matrix4x4 mat = from_quaternion(r);
        // Scale first: post-multiplying by a diagonal S scales each basis column.
        mat.m[0] *= s.x; mat.m[1] *= s.x; mat.m[2]  *= s.x;
        mat.m[4] *= s.y; mat.m[5] *= s.y; mat.m[6]  *= s.y;
        mat.m[8] *= s.z; mat.m[9] *= s.z; mat.m[10] *= s.z;
        // Translation is the last column, unaffected by R and S.
        mat.m[12] = t.x;
        mat.m[13] = t.y;
        mat.m[14] = t.z;
        return mat;
    }

    // Extracts the rotation of a transform matrix as a quaternion, using Shepperd's
    // method: pick the branch whose divisor is largest so the square root is never
    // taken of a near-zero number. Needed to decompose a bone's rest transform,
    // which the importer stores as a matrix but which blending has to interpolate
    // as TRS.
    Vector4 to_quaternion() const {
        // Divide the scale out of each basis column first; a quaternion has no way
        // to represent a scaled frame, and leaving it in denormalises the result.
        float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        float ix = (sx > 1e-8f) ? 1.0f / sx : 0.0f;
        float iy = (sy > 1e-8f) ? 1.0f / sy : 0.0f;
        float iz = (sz > 1e-8f) ? 1.0f / sz : 0.0f;

        // r[row][col], from column-major storage where (row, col) is m[col * 4 + row].
        float r00 = m[0] * ix, r10 = m[1] * ix, r20 = m[2] * ix;
        float r01 = m[4] * iy, r11 = m[5] * iy, r21 = m[6] * iy;
        float r02 = m[8] * iz, r12 = m[9] * iz, r22 = m[10] * iz;

        float trace = r00 + r11 + r22;
        Vector4 q;
        if (trace > 0.0f) {
            float s = std::sqrt(trace + 1.0f) * 2.0f;
            q.w = 0.25f * s;
            q.x = (r21 - r12) / s;
            q.y = (r02 - r20) / s;
            q.z = (r10 - r01) / s;
        } else if (r00 > r11 && r00 > r22) {
            float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
            q.w = (r21 - r12) / s;
            q.x = 0.25f * s;
            q.y = (r01 + r10) / s;
            q.z = (r02 + r20) / s;
        } else if (r11 > r22) {
            float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
            q.w = (r02 - r20) / s;
            q.x = (r01 + r10) / s;
            q.y = 0.25f * s;
            q.z = (r12 + r21) / s;
        } else {
            float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
            q.w = (r10 - r01) / s;
            q.x = (r02 + r20) / s;
            q.y = (r12 + r21) / s;
            q.z = 0.25f * s;
        }
        return q.normalized();
    }

    // Splits a transform matrix back into the translation, rotation and scale that
    // from_trs() would recompose into it. A mirrored matrix (negative determinant)
    // cannot be expressed as a positive scale and a rotation, so the sign is folded
    // into the x axis - the same convention every DCC tool uses.
    void decompose_trs(Vector3& out_translation, Vector4& out_rotation, Vector3& out_scale) const {
        out_translation = { m[12], m[13], m[14] };
        out_scale = {
            std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]),
            std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]),
            std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10])
        };

        // Determinant of the upper 3x3, in column-major indices.
        float det =
            m[0] * (m[5] * m[10] - m[6] * m[9]) -
            m[4] * (m[1] * m[10] - m[2] * m[9]) +
            m[8] * (m[1] * m[6]  - m[2] * m[5]);
        if (det < 0.0f) out_scale.x = -out_scale.x;

        Matrix4x4 rot = *this;
        float ix = (std::abs(out_scale.x) > 1e-8f) ? 1.0f / out_scale.x : 0.0f;
        float iy = (out_scale.y > 1e-8f) ? 1.0f / out_scale.y : 0.0f;
        float iz = (out_scale.z > 1e-8f) ? 1.0f / out_scale.z : 0.0f;
        rot.m[0] *= ix; rot.m[1] *= ix; rot.m[2]  *= ix;
        rot.m[4] *= iy; rot.m[5] *= iy; rot.m[6]  *= iy;
        rot.m[8] *= iz; rot.m[9] *= iz; rot.m[10] *= iz;
        rot.m[12] = rot.m[13] = rot.m[14] = 0.0f;
        out_rotation = rot.to_quaternion();
    }

    static Matrix4x4 perspective(float fovDegrees, float aspect, float nearPlane, float farPlane) {
        Matrix4x4 mat;
        float fovRad = fovDegrees * (3.14159265f / 180.0f);
        float tanHalfFov = std::tan(fovRad / 2.0f);
        mat.m[0] = 1.0f / (aspect * tanHalfFov);
        mat.m[5] = 1.0f / tanHalfFov;
        mat.m[10] = -(farPlane + nearPlane) / (farPlane - nearPlane);
        mat.m[11] = -1.0f;
        mat.m[14] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
        mat.m[15] = 0.0f;
        return mat;
    }

    static Matrix4x4 orthographic(float left, float right, float bottom, float top, float zNear, float zFar) {
        Matrix4x4 mat;
        mat.m[0] = 2.0f / (right - left);
        mat.m[5] = 2.0f / (top - bottom);
        mat.m[10] = -2.0f / (zFar - zNear);
        mat.m[12] = -(right + left) / (right - left);
        mat.m[13] = -(top + bottom) / (top - bottom);
        mat.m[14] = -(zFar + zNear) / (zFar - zNear);
        return mat;
    }

    static Matrix4x4 look_at(const Vector3& eye, const Vector3& center, const Vector3& up) {
        Vector3 f = (center - eye).normalized();
        Vector3 s = Vector3::cross(f, up).normalized();
        Vector3 u = Vector3::cross(s, f);

        Matrix4x4 mat;
        mat.m[0] = s.x;
        mat.m[4] = s.y;
        mat.m[8] = s.z;
        
        mat.m[1] = u.x;
        mat.m[5] = u.y;
        mat.m[9] = u.z;
        
        mat.m[2] = -f.x;
        mat.m[6] = -f.y;
        mat.m[10] = -f.z;
        
        mat.m[12] = -Vector3::dot(s, eye);
        mat.m[13] = -Vector3::dot(u, eye);
        mat.m[14] = Vector3::dot(f, eye);
        return mat;
    }

    Matrix4x4 operator*(const Matrix4x4& other) const {
        Matrix4x4 result;
        for (int i = 0; i < 4; ++i) {       // col of other (and result)
            for (int j = 0; j < 4; ++j) {   // row of this (and result)
                result.m[i * 4 + j] = 
                    m[0 * 4 + j] * other.m[i * 4 + 0] +
                    m[1 * 4 + j] * other.m[i * 4 + 1] +
                    m[2 * 4 + j] * other.m[i * 4 + 2] +
                    m[3 * 4 + j] * other.m[i * 4 + 3];
            }
        }
        return result;
    }

    Matrix4x4 inverse() const {
        Matrix4x4 inv;
        inv.m[0] = m[5]  * m[10] * m[15] - m[5]  * m[11] * m[14] - m[9]  * m[6]  * m[15] + m[9]  * m[7]  * m[14] + m[13] * m[6]  * m[11] - m[13] * m[7]  * m[10];
        inv.m[4] = -m[4]  * m[10] * m[15] + m[4]  * m[11] * m[14] + m[8]  * m[6]  * m[15] - m[8]  * m[7]  * m[14] - m[12] * m[6]  * m[11] + m[12] * m[7]  * m[10];
        inv.m[8] = m[4]  * m[9]  * m[15] - m[4]  * m[11] * m[13] - m[8]  * m[5]  * m[15] + m[8]  * m[7]  * m[13] + m[12] * m[5]  * m[11] - m[12] * m[7]  * m[9];
        inv.m[12] = -m[4]  * m[9]  * m[14] + m[4]  * m[10] * m[13] + m[8]  * m[5]  * m[14] - m[8]  * m[6]  * m[13] - m[12] * m[5]  * m[10] + m[12] * m[6]  * m[9];
        inv.m[1] = -m[1]  * m[10] * m[15] + m[1]  * m[11] * m[14] + m[9]  * m[2]  * m[15] - m[9]  * m[3]  * m[14] - m[13] * m[2]  * m[11] + m[13] * m[3]  * m[10];
        inv.m[5] = m[0]  * m[10] * m[15] - m[0]  * m[11] * m[14] - m[8]  * m[2]  * m[15] + m[8]  * m[3]  * m[14] + m[12] * m[2]  * m[11] - m[12] * m[3]  * m[10];
        inv.m[9] = -m[0]  * m[9]  * m[15] + m[0]  * m[11] * m[13] + m[8]  * m[1]  * m[15] - m[8]  * m[3]  * m[13] - m[12] * m[1]  * m[11] + m[12] * m[3]  * m[9];
        inv.m[13] = m[0]  * m[9]  * m[14] - m[0]  * m[10] * m[13] - m[8]  * m[1]  * m[14] + m[8]  * m[2]  * m[13] + m[12] * m[1]  * m[10] - m[12] * m[2]  * m[9];
        inv.m[2] = m[1]  * m[6]  * m[15] - m[1]  * m[7]  * m[14] - m[5]  * m[2]  * m[15] + m[5]  * m[3]  * m[14] + m[13] * m[2]  * m[7]  - m[13] * m[3]  * m[6];
        inv.m[6] = -m[0]  * m[6]  * m[15] + m[0]  * m[7]  * m[14] + m[4]  * m[2]  * m[15] - m[4]  * m[3]  * m[14] - m[12] * m[2]  * m[7]  + m[12] * m[3]  * m[6];
        inv.m[10] = m[0]  * m[5]  * m[15] - m[0]  * m[7]  * m[13] - m[4]  * m[1]  * m[15] + m[4]  * m[3]  * m[13] + m[12] * m[1]  * m[7]  - m[12] * m[3]  * m[5];
        inv.m[14] = -m[0]  * m[5]  * m[14] + m[0]  * m[6]  * m[13] + m[4]  * m[1]  * m[14] - m[4]  * m[2]  * m[13] - m[12] * m[1]  * m[6]  + m[12] * m[2]  * m[5];
        inv.m[3] = -m[1]  * m[6]  * m[11] + m[1]  * m[7]  * m[10] + m[5]  * m[2]  * m[11] - m[5]  * m[3]  * m[10] - m[9]  * m[2]  * m[7]  + m[9]  * m[3]  * m[6];
        inv.m[7] = m[0]  * m[6]  * m[11] - m[0]  * m[7]  * m[10] - m[4]  * m[2]  * m[11] + m[4]  * m[3]  * m[10] + m[8]  * m[2]  * m[7]  - m[8]  * m[3]  * m[6];
        inv.m[11] = -m[0]  * m[5]  * m[11] + m[0]  * m[7]  * m[9]  + m[4]  * m[1]  * m[11] - m[4]  * m[3]  * m[9]  - m[8]  * m[1]  * m[7]  + m[8]  * m[3]  * m[5];
        inv.m[15] = m[0]  * m[5]  * m[10] - m[0]  * m[6]  * m[9]  - m[4]  * m[1]  * m[10] + m[4]  * m[2]  * m[9]  + m[8]  * m[1]  * m[6]  - m[8]  * m[2]  * m[5];

        float det = m[0] * inv.m[0] + m[1] * inv.m[4] + m[2] * inv.m[8] + m[3] * inv.m[12];
        if (det != 0.0f) {
            det = 1.0f / det;
            for (int i = 0; i < 16; i++) {
                inv.m[i] = inv.m[i] * det;
            }
        }
        return inv;
    }

    Matrix4x4 transpose() const {
        Matrix4x4 res;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                res.m[i * 4 + j] = m[j * 4 + i];
            }
        }
        return res;
    }

    Vector3 operator*(const Vector3& v) const {
        float x = m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * 1.0f;
        float y = m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * 1.0f;
        float z = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * 1.0f;
        float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * 1.0f;
        if (w != 0.0f && w != 1.0f) {
            return {x / w, y / w, z / w};
        }
        return {x, y, z};
    }
};

struct Transform {
    DVector3 position = {0.0, 0.0, 0.0};
    Vector3 rotation = {0.0f, 0.0f, 0.0f}; // Euler angles (pitch, yaw, roll) in radians
    Vector3 scale = {1.0f, 1.0f, 1.0f};

    Matrix4x4 get_matrix() const {
        Matrix4x4 translationMat = Matrix4x4::translation(position.to_vec3());
        Matrix4x4 rotX = Matrix4x4::rotationX(rotation.x);
        Matrix4x4 rotY = Matrix4x4::rotationY(rotation.y);
        Matrix4x4 rotZ = Matrix4x4::rotationZ(rotation.z);
        Matrix4x4 rotationMat = rotZ * rotX * rotY;
        Matrix4x4 scaleMat = Matrix4x4::scale(scale);

        return translationMat * rotationMat * scaleMat;
    }

    Matrix4x4 get_relative_matrix(const DVector3& origin) const {
        DVector3 rel_pos = position - origin;
        Matrix4x4 translationMat = Matrix4x4::translation(rel_pos.to_vec3());
        Matrix4x4 rotX = Matrix4x4::rotationX(rotation.x);
        Matrix4x4 rotY = Matrix4x4::rotationY(rotation.y);
        Matrix4x4 rotZ = Matrix4x4::rotationZ(rotation.z);
        Matrix4x4 rotationMat = rotZ * rotX * rotY;
        Matrix4x4 scaleMat = Matrix4x4::scale(scale);

        return translationMat * rotationMat * scaleMat;
    }

    // Exact inverse of get_relative_matrix(): recovers the components that recompose
    // to the same matrix under this engine's own convention.
    //
    // This exists because ImGuizmo's DecomposeMatrixToComponents cannot be used for
    // the write-back. It returns Euler angles in ImGuizmo's order, whereas the two
    // functions above recompose as rotZ * rotX * rotY - so round-tripping a gizmo
    // drag through it corrupts the rotation of any actor that was already rotated.
    static Transform from_relative_matrix(const Matrix4x4& mat, const DVector3& origin) {
        const std::array<float, 16>& m = mat.m;
        Transform t;

        // The upper 3x3 is R * S. A diagonal S scales each basis column, so the
        // column lengths are the scale and dividing them out leaves pure rotation.
        // Indices are column-major: element (row, col) lives at m[col * 4 + row].
        float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        t.scale = { sx, sy, sz };

        float ix = (sx > 1e-8f) ? 1.0f / sx : 0.0f;
        float iy = (sy > 1e-8f) ? 1.0f / sy : 0.0f;
        float iz = (sz > 1e-8f) ? 1.0f / sz : 0.0f;

        float r00 = m[0] * ix, r20 = m[2]  * ix;
        float r01 = m[4] * iy, r11 = m[5]  * iy, r21 = m[6] * iy;
        float r02 = m[8] * iz, r22 = m[10] * iz;

        // Expanding R = Ez(z) * Ex(x) * Ey(y) for this engine's rotation matrices
        // gives R(2,1) = -sin x, R(2,0) = cos x sin y, R(2,2) = cos x cos y,
        // R(0,1) = sin z cos x and R(1,1) = cos z cos x - which inverts directly.
        float sin_x = -r21;
        // cos x comes from the matrix rather than sqrt(1 - sin^2 x): the two entries
        // below are cos x sin y and cos x cos y, so their hypotenuse *is* cos x, and
        // it stays accurate near +/-90 degrees where 1 - sin^2 x cancels badly.
        float cos_x = std::sqrt(r20 * r20 + r22 * r22);
        // atan2 rather than asin(sin_x): asin's derivative is unbounded as its
        // argument approaches +/-1, so it turns the last bit of float error in a
        // near-90-degree matrix into a visibly wrong angle. atan2 is well conditioned
        // over the whole range, and cos_x is non-negative, so this still yields the
        // [-90, 90] degree principal value this decomposition assumes.
        t.rotation.x = std::atan2(sin_x, cos_x);

        if (cos_x > 1e-5f) {
            t.rotation.y = std::atan2(r20, r22);
            t.rotation.z = std::atan2(r01, r11);
        } else {
            // Gimbal lock: x is at +/-90 degrees, where y and z spin about the same
            // axis and only their sum is recoverable. Pin z and fold it all into y.
            t.rotation.y = std::atan2(-r02, r00);
            t.rotation.z = 0.0f;
        }

        // Translation is the last column, still relative to the camera origin.
        t.position = { static_cast<double>(m[12]) + origin.x,
                       static_cast<double>(m[13]) + origin.y,
                       static_cast<double>(m[14]) + origin.z };
        return t;
    }
};
