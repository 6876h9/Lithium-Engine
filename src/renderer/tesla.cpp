#include "renderer/tesla.hpp"
#include "renderer/gl_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// GL enums that SDL_opengl.h may predate.
#ifndef GL_TEXTURE_BUFFER
#define GL_TEXTURE_BUFFER 0x8C2A
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_R32I
#define GL_R32I 0x8235
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_RG32F
#define GL_RG32F 0x8230
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif

namespace {

constexpr float kPi     = 3.14159265358979323846f;
constexpr float kInvPi  = 0.31830988618379067154f;
constexpr float kEps    = 1e-4f;
constexpr float kInf    = 3.402823466e+38f;
// Below this, GGX is numerically a mirror. Clamping keeps every pdf finite, which
// is what lets the specular lobe take part in MIS without a special delta case.
constexpr float kMinAlpha = 1e-3f;

// All textures are resampled to this square size so they can share one array
// texture. 512 keeps a typical albedo map readable without turning a scene's
// texture set into hundreds of megabytes.
constexpr int kTextureSize  = 512;
constexpr int kMaxTextures  = 64;

inline Vector3 neg(const Vector3& v)             { return { -v.x, -v.y, -v.z }; }
inline Vector3 vmin(const Vector3& a, const Vector3& b) { return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) }; }
inline Vector3 vmax(const Vector3& a, const Vector3& b) { return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) }; }
inline float   vmax_comp(const Vector3& v)       { return std::max(v.x, std::max(v.y, v.z)); }
inline float   luminance(const Vector3& c)       { return 0.2126f*c.x + 0.7152f*c.y + 0.0722f*c.z; }
inline Vector3 lerp3(const Vector3& a, const Vector3& b, float t) { return a * (1.0f - t) + b * t; }

inline bool is_black(const Vector3& c) { return c.x <= 0.0f && c.y <= 0.0f && c.z <= 0.0f; }

// --------------------------------------------------------------------------
// PCG32. Identical arithmetic to the GLSL version, so both backends walk the
// same sample sequence and converge to the same image.
// --------------------------------------------------------------------------
inline uint32_t pcg_next(uint64_t& state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t xorshifted = static_cast<uint32_t>(((state >> 18u) ^ state) >> 27u);
    uint32_t rot        = static_cast<uint32_t>(state >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
}

inline float rnd(uint64_t& state) {
    // 24 mantissa bits -> [0,1). Never returns exactly 1, so no pdf blows up.
    return static_cast<float>(pcg_next(state) >> 8) * (1.0f / 16777216.0f);
}

inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

inline uint64_t seed_for(uint32_t pixel, uint32_t sample) {
    return splitmix64((static_cast<uint64_t>(sample) << 32) ^ static_cast<uint64_t>(pixel) ^ 0xDA3E39CB94B95BDBULL);
}

// --------------------------------------------------------------------------
// Shading frame
// --------------------------------------------------------------------------
struct Frame {
    Vector3 t, b, n;

    void from_normal(const Vector3& normal) {
        n = normal;
        // Duff et al., branchless orthonormal basis.
        float sign = std::copysign(1.0f, n.z);
        float a  = -1.0f / (sign + n.z);
        float bb = n.x * n.y * a;
        t = Vector3{ 1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x };
        b = Vector3{ bb, sign + n.y * n.y * a, -n.y };
    }

    Vector3 to_local(const Vector3& v) const {
        return { Vector3::dot(v, t), Vector3::dot(v, b), Vector3::dot(v, n) };
    }
    Vector3 to_world(const Vector3& v) const {
        return t * v.x + b * v.y + n * v.z;
    }
};

// --------------------------------------------------------------------------
// Sampling primitives. Every one returns the pdf it sampled from - nothing is
// left implicit, because an unmatched pdf is exactly how bias creeps in.
// --------------------------------------------------------------------------

// Cosine-weighted hemisphere about +Z. pdf = cos / pi.
inline Vector3 sample_cosine_hemisphere(float u1, float u2) {
    float r   = std::sqrt(u1);
    float phi = 2.0f * kPi * u2;
    float z   = std::sqrt(std::max(0.0f, 1.0f - u1));
    return { r * std::cos(phi), r * std::sin(phi), z };
}
inline float cosine_hemisphere_pdf(float cos_theta) {
    return cos_theta > 0.0f ? cos_theta * kInvPi : 0.0f;
}

// Uniform direction inside a cone of half-angle acos(cos_theta_max), about +Z.
inline Vector3 sample_uniform_cone(float u1, float u2, float cos_theta_max) {
    float cos_theta = 1.0f - u1 * (1.0f - cos_theta_max);
    float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    float phi       = 2.0f * kPi * u2;
    return { sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta };
}
inline float uniform_cone_pdf(float cos_theta_max) {
    float solid_angle = 2.0f * kPi * (1.0f - cos_theta_max);
    return solid_angle > 0.0f ? 1.0f / solid_angle : 0.0f;
}

// GGX / Trowbridge-Reitz normal distribution.
inline float ggx_D(float cos_theta_m, float alpha) {
    if (cos_theta_m <= 0.0f) return 0.0f;
    float a2 = alpha * alpha;
    float c2 = cos_theta_m * cos_theta_m;
    float d  = c2 * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

// Smith Lambda for GGX.
inline float smith_lambda(float cos_theta, float alpha) {
    float c = std::abs(cos_theta);
    if (c >= 1.0f) return 0.0f;
    float a2   = alpha * alpha;
    float tan2 = (1.0f - c * c) / (c * c);
    return 0.5f * (-1.0f + std::sqrt(1.0f + a2 * tan2));
}
inline float smith_G1(float cos_theta, float alpha) {
    return 1.0f / (1.0f + smith_lambda(cos_theta, alpha));
}
// Height-correlated Smith masking-shadowing.
inline float smith_G2(float cos_o, float cos_i, float alpha) {
    return 1.0f / (1.0f + smith_lambda(cos_o, alpha) + smith_lambda(cos_i, alpha));
}

inline Vector3 fresnel_schlick(const Vector3& f0, float cos_theta) {
    float m = std::pow(std::max(0.0f, 1.0f - cos_theta), 5.0f);
    return f0 + (Vector3{ 1.0f, 1.0f, 1.0f } - f0) * m;
}

// Heitz 2018, sampling the distribution of visible normals. Draws only microfacets
// the outgoing direction can actually see, so no sample is wasted and the pdf below
// is exact.
inline Vector3 sample_ggx_vndf(const Vector3& wo, float alpha, float u1, float u2) {
    Vector3 Vh = Vector3{ alpha * wo.x, alpha * wo.y, wo.z }.normalized();

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    Vector3 T1 = lensq > 0.0f ? Vector3{ -Vh.y, Vh.x, 0.0f } * (1.0f / std::sqrt(lensq))
                              : Vector3{ 1.0f, 0.0f, 0.0f };
    Vector3 T2 = Vector3::cross(Vh, T1);

    float r   = std::sqrt(u1);
    float phi = 2.0f * kPi * u2;
    float t1  = r * std::cos(phi);
    float t2  = r * std::sin(phi);
    float s   = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * std::sqrt(std::max(0.0f, 1.0f - t1 * t1)) + s * t2;

    Vector3 Nh = T1 * t1 + T2 * t2 + Vh * std::sqrt(std::max(0.0f, 1.0f - t1 * t1 - t2 * t2));
    return Vector3{ alpha * Nh.x, alpha * Nh.y, std::max(0.0f, Nh.z) }.normalized();
}

// pdf of the reflected direction produced by VNDF sampling.
// pdf(wm) = G1(wo) * max(0, wo.wm) * D(wm) / wo.z, then the reflection Jacobian
// 1/(4 wo.wm) cancels the dot product.
inline float ggx_vndf_pdf(const Vector3& wo, const Vector3& wm, float alpha) {
    if (wo.z <= 0.0f) return 0.0f;
    return smith_G1(wo.z, alpha) * ggx_D(wm.z, alpha) / (4.0f * wo.z);
}

// Power heuristic (beta = 2) with one sample from each strategy.
inline float mis_power(float pdf_a, float pdf_b) {
    float a = pdf_a * pdf_a;
    float b = pdf_b * pdf_b;
    float s = a + b;
    return s > 0.0f ? a / s : 0.0f;
}

// --------------------------------------------------------------------------
// Directional albedo of the GGX lobe, tabulated.
//
// Two things need to know how much energy the specular lobe actually reflects for a
// given view angle and roughness:
//
//   - the diffuse substrate, which must give up exactly that much and no more.
//     A constant Fresnel average is not enough: at grazing incidence the specular
//     albedo climbs past 0.45 while the average only accounts for 0.086, and the
//     surface ends up reflecting ~137% of the light that hits it.
//   - the specular lobe itself, which loses energy at high roughness because a
//     single-scattering microfacet model drops every ray that would have bounced
//     between facets. A rough metal reflects only ~31% of what it should.
//
// Schlick's Fresnel is linear in f0, so the albedo splits exactly as f0*A + B with A
// and B depending only on (cos_theta_o, roughness). Both are integrated here once,
// with the same VNDF sampler the renderer uses, so the table describes the real lobe
// rather than an analytic fit to a different one.
// --------------------------------------------------------------------------
constexpr int kAlbedoLutSize = 32;

struct AlbedoLUT {
    float A[kAlbedoLutSize * kAlbedoLutSize];
    float B[kAlbedoLutSize * kAlbedoLutSize];

    AlbedoLUT() {
        constexpr int kSamples = 8192;
        for (int j = 0; j < kAlbedoLutSize; ++j) {
            float roughness = (j + 0.5f) / kAlbedoLutSize;
            float alpha = std::max(kMinAlpha, roughness * roughness);
            for (int i = 0; i < kAlbedoLutSize; ++i) {
                float cos_o = (i + 0.5f) / kAlbedoLutSize;
                float sin_o = std::sqrt(std::max(0.0f, 1.0f - cos_o * cos_o));
                Vector3 wo{ sin_o, 0.0f, cos_o };

                double a = 0.0, b = 0.0;
                uint64_t rng = seed_for(static_cast<uint32_t>(i), static_cast<uint32_t>(j) + 1u);
                for (int k = 0; k < kSamples; ++k) {
                    Vector3 wm = sample_ggx_vndf(wo, alpha, rnd(rng), rnd(rng));
                    Vector3 wi = wm * (2.0f * Vector3::dot(wo, wm)) - wo;
                    if (wi.z <= 0.0f) continue;

                    // With VNDF sampling the whole estimator collapses to G2/G1:
                    // f*cos/pdf = (D G2 / 4 co ci) * ci / (G1 D / 4 co) = G2/G1.
                    float g1 = smith_G1(wo.z, alpha);
                    if (g1 <= 0.0f) continue;
                    float weight = smith_G2(wo.z, wi.z, alpha) / g1;

                    float c  = std::max(0.0f, Vector3::dot(wi, wm));
                    float fc = std::pow(1.0f - c, 5.0f);
                    a += weight * (1.0f - fc);
                    b += weight * fc;
                }
                A[j * kAlbedoLutSize + i] = static_cast<float>(a / kSamples);
                B[j * kAlbedoLutSize + i] = static_cast<float>(b / kSamples);
            }
        }
    }

    // Bilinear, matching the GL_LINEAR filtering the GPU backend gets for free.
    void lookup(float cos_o, float roughness, float& out_a, float& out_b) const {
        float x = std::min(std::max(cos_o * kAlbedoLutSize - 0.5f, 0.0f), kAlbedoLutSize - 1.0f);
        float y = std::min(std::max(roughness * kAlbedoLutSize - 0.5f, 0.0f), kAlbedoLutSize - 1.0f);
        int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        int x1 = std::min(x0 + 1, kAlbedoLutSize - 1);
        int y1 = std::min(y0 + 1, kAlbedoLutSize - 1);
        float fx = x - x0, fy = y - y0;

        auto at = [&](const float* t, int xi, int yi) { return t[yi * kAlbedoLutSize + xi]; };
        out_a = (at(A,x0,y0)*(1-fx) + at(A,x1,y0)*fx)*(1-fy) + (at(A,x0,y1)*(1-fx) + at(A,x1,y1)*fx)*fy;
        out_b = (at(B,x0,y0)*(1-fx) + at(B,x1,y0)*fx)*(1-fy) + (at(B,x0,y1)*(1-fx) + at(B,x1,y1)*fx)*fy;
    }
};

// Magic statics make this thread-safe, which matters: the CPU backend evaluates
// BSDFs from every worker thread.
const AlbedoLUT& albedo_lut() {
    static const AlbedoLUT lut;
    return lut;
}

// --------------------------------------------------------------------------
// glTF metallic-roughness BSDF: Lambert diffuse + GGX microfacet specular.
// Every routine works in the local frame where the shading normal is +Z.
// --------------------------------------------------------------------------
struct BSDF {
    Vector3 albedo;
    float   metallic;
    float   alpha;         // roughness^2
    Vector3 f0;
    Vector3 diffuse_scale;   // energy left over after the specular lobe takes its share
    Vector3 ms_compensation; // restores the inter-facet scattering single-scatter GGX drops
    float   p_specular;      // lobe selection probability, matched exactly by pdf()

    void setup(const Vector3& base_color, float metallic_in, float roughness, float cos_theta_o) {
        albedo   = base_color;
        metallic = std::min(std::max(metallic_in, 0.0f), 1.0f);
        float r  = std::min(std::max(roughness, 0.0f), 1.0f);
        alpha    = std::max(kMinAlpha, r * r);
        f0       = lerp3(Vector3{ 0.04f, 0.04f, 0.04f }, albedo, metallic);

        const Vector3 one{ 1.0f, 1.0f, 1.0f };

        float lut_a = 1.0f, lut_b = 0.0f;
        albedo_lut().lookup(std::min(std::max(cos_theta_o, 0.0f), 1.0f), r, lut_a, lut_b);

        // Single-scattering GGX loses the energy of every inter-facet bounce, which
        // at high roughness is most of it. Scaling the lobe by this factor restores
        // it; because it is a constant for a given wo it changes the BSDF but not the
        // distribution being sampled, so the estimator stays exact.
        float ess = std::max(1e-4f, lut_a + lut_b);
        ms_compensation = one + f0 * (1.0f / ess - 1.0f);

        // What the specular layer actually reflects at this angle, after that
        // compensation. Schlick is linear in f0, hence the f0*A + B split.
        Vector3 rho_specular = (f0 * lut_a + Vector3{ lut_b, lut_b, lut_b }) * ms_compensation;
        rho_specular = vmin(rho_specular, one);

        // The diffuse substrate receives exactly what the specular layer did not
        // reflect - no more, so a white surface can never return more than it
        // received, and no less, so nothing goes missing at normal incidence.
        diffuse_scale = (one - rho_specular) * (1.0f - metallic);

        float w_diffuse  = luminance(albedo * diffuse_scale);
        float w_specular = luminance(rho_specular);
        float total      = w_diffuse + w_specular;
        // Clamped so neither lobe ever gets a zero probability while still being
        // sampled - pdf() below divides by the same number, so this stays exact.
        p_specular = total > 0.0f ? std::min(0.9f, std::max(0.1f, w_specular / total)) : 0.5f;
    }

    // Full BSDF value, both lobes, for wo/wi in the local frame.
    Vector3 eval(const Vector3& wo, const Vector3& wi) const {
        if (wo.z <= 0.0f || wi.z <= 0.0f) return { 0.0f, 0.0f, 0.0f };

        Vector3 diffuse = albedo * diffuse_scale * kInvPi;

        Vector3 wm = (wo + wi).normalized();
        float D = ggx_D(wm.z, alpha);
        float G = smith_G2(wo.z, wi.z, alpha);
        Vector3 F = fresnel_schlick(f0, std::max(0.0f, Vector3::dot(wi, wm)));
        Vector3 specular = F * ms_compensation * (D * G / (4.0f * wo.z * wi.z));

        return diffuse + specular;
    }

    // Combined pdf of both lobes for a direction. Must be the pdf of the *mixture*,
    // not of whichever lobe happened to generate the sample.
    float pdf(const Vector3& wo, const Vector3& wi) const {
        if (wo.z <= 0.0f || wi.z <= 0.0f) return 0.0f;

        float pdf_diffuse = cosine_hemisphere_pdf(wi.z);

        Vector3 wm = (wo + wi).normalized();
        float pdf_specular = ggx_vndf_pdf(wo, wm, alpha);

        return p_specular * pdf_specular + (1.0f - p_specular) * pdf_diffuse;
    }

    // Draws a direction, returns false if it went below the surface.
    bool sample(const Vector3& wo, float u_lobe, float u1, float u2,
                Vector3& wi, Vector3& value, float& pdf_out) const {
        if (wo.z <= 0.0f) return false;

        if (u_lobe < p_specular) {
            Vector3 wm = sample_ggx_vndf(wo, alpha, u1, u2);
            wi = wm * (2.0f * Vector3::dot(wo, wm)) - wo;
            if (wi.z <= 0.0f) return false;
        } else {
            wi = sample_cosine_hemisphere(u1, u2);
            if (wi.z <= 0.0f) return false;
        }

        pdf_out = pdf(wo, wi);
        if (pdf_out <= 0.0f) return false;
        value = eval(wo, wi);
        return !is_black(value);
    }
};

// --------------------------------------------------------------------------
// Texture sampling
//
// Bilinear, GL_REPEAT, always from the full-resolution image - no mip chain. That
// is not a shortcut: a mip level is a prefiltered average chosen from screen-space
// derivatives, which do not exist for a scattered ray, and prefiltering is a bias.
// Point-sampling level 0 while the camera ray is jittered per sample converges to
// the correctly filtered result on its own.
// --------------------------------------------------------------------------
inline Vector3 sample_texture_rgba8(const unsigned char* tex, float u, float v) {
    // Wrap into [0,1). fmod keeps large UVs (tiled surfaces) working.
    u = u - std::floor(u);
    v = v - std::floor(v);

    float fx = u * kTextureSize - 0.5f;
    float fy = v * kTextureSize - 0.5f;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    float tx = fx - x0;
    float ty = fy - y0;

    Vector3 acc{ 0.0f, 0.0f, 0.0f };
    for (int j = 0; j < 2; ++j) {
        int yy = ((y0 + j) % kTextureSize + kTextureSize) % kTextureSize;
        float wy = j ? ty : (1.0f - ty);
        for (int i = 0; i < 2; ++i) {
            int xx = ((x0 + i) % kTextureSize + kTextureSize) % kTextureSize;
            float wx = i ? tx : (1.0f - tx);
            const unsigned char* t = &tex[(static_cast<size_t>(yy) * kTextureSize + xx) * 4];
            float w = wx * wy * (1.0f / 255.0f);
            acc += Vector3{ t[0] * w, t[1] * w, t[2] * w };
        }
    }
    return acc;
}

// --------------------------------------------------------------------------
// Ray / triangle
// --------------------------------------------------------------------------
struct Hit {
    float t = kInf;
    float u = 0.0f, v = 0.0f;
    int   prim = -1;
};

inline bool intersect_triangle(const Vector3& org, const Vector3& dir,
                               const TeslaTriangle& tri, float tmin, float tmax,
                               float& t_out, float& u_out, float& v_out) {
    Vector3 e1 = tri.v1 - tri.v0;
    Vector3 e2 = tri.v2 - tri.v0;
    Vector3 p  = Vector3::cross(dir, e2);
    float det  = Vector3::dot(e1, p);
    if (std::abs(det) < 1e-12f) return false;

    float inv = 1.0f / det;
    Vector3 s = org - tri.v0;
    float u = Vector3::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;

    Vector3 q = Vector3::cross(s, e1);
    float v = Vector3::dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;

    float t = Vector3::dot(e2, q) * inv;
    if (t < tmin || t > tmax) return false;

    t_out = t; u_out = u; v_out = v;
    return true;
}

inline bool slab_test(const Vector3& org, const Vector3& inv_dir,
                      const Vector3& bmin, const Vector3& bmax,
                      float tmin, float tmax, float& t_enter) {
    float t0 = (bmin.x - org.x) * inv_dir.x;
    float t1 = (bmax.x - org.x) * inv_dir.x;
    float lo = std::min(t0, t1), hi = std::max(t0, t1);

    t0 = (bmin.y - org.y) * inv_dir.y;
    t1 = (bmax.y - org.y) * inv_dir.y;
    lo = std::max(lo, std::min(t0, t1));
    hi = std::min(hi, std::max(t0, t1));

    t0 = (bmin.z - org.z) * inv_dir.z;
    t1 = (bmax.z - org.z) * inv_dir.z;
    lo = std::max(lo, std::min(t0, t1));
    hi = std::min(hi, std::max(t0, t1));

    t_enter = std::max(lo, tmin);
    return hi >= t_enter && lo <= tmax;
}

// --------------------------------------------------------------------------
// GLSL helpers
// --------------------------------------------------------------------------
unsigned int compile_stage(unsigned int type, const char* src, const char* label) {
    unsigned int shader = glCreateShader(type);
    if (shader == 0) return 0;
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "[TESLA] " << label << " compile failed:\n" << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

// ===========================================================================
// BVH - binned SAH
// ===========================================================================

void TeslaBVH::clear() {
    nodes_.clear();
    indices_.clear();
}

void TeslaBVH::build(const std::vector<TeslaTriangle>& tris) {
    clear();
    if (tris.empty()) return;

    const int n = static_cast<int>(tris.size());
    indices_.resize(n);

    std::vector<BuildPrim> prims(n);
    Vector3 root_min{ kInf, kInf, kInf };
    Vector3 root_max{ -kInf, -kInf, -kInf };

    for (int i = 0; i < n; ++i) {
        indices_[i] = i;
        const TeslaTriangle& t = tris[i];
        BuildPrim& p = prims[i];
        p.bmin = vmin(t.v0, vmin(t.v1, t.v2));
        p.bmax = vmax(t.v0, vmax(t.v1, t.v2));
        // Degenerate-thin boxes make the slab test unreliable, so give every prim
        // a sliver of thickness.
        p.bmin = p.bmin - Vector3{ 1e-5f, 1e-5f, 1e-5f };
        p.bmax = p.bmax + Vector3{ 1e-5f, 1e-5f, 1e-5f };
        p.centroid = (p.bmin + p.bmax) * 0.5f;
        root_min = vmin(root_min, p.bmin);
        root_max = vmax(root_max, p.bmax);
    }

    nodes_.reserve(static_cast<size_t>(n) * 2);
    TeslaBVHNode root;
    root.bmin = root_min;
    root.bmax = root_max;
    root.left_first = 0;
    root.count = n;
    nodes_.push_back(root);

    subdivide(0, prims, 0);
}

void TeslaBVH::subdivide(int node_index, const std::vector<BuildPrim>& prims, int depth) {
    constexpr int kMaxLeafSize = 4;
    constexpr int kMaxDepth    = 48;   // keeps the 64-entry traversal stacks safe
    constexpr int kBinCount    = 12;

    TeslaBVHNode node = nodes_[node_index];
    if (node.count <= kMaxLeafSize || depth >= kMaxDepth) return;

    const int first = node.left_first;
    const int count = node.count;

    // Split candidates are evaluated in centroid space so bins are populated evenly.
    Vector3 cmin{ kInf, kInf, kInf };
    Vector3 cmax{ -kInf, -kInf, -kInf };
    for (int i = 0; i < count; ++i) {
        const Vector3& c = prims[indices_[first + i]].centroid;
        cmin = vmin(cmin, c);
        cmax = vmax(cmax, c);
    }

    Vector3 extent = cmax - cmin;
    float parent_area;
    {
        Vector3 d = node.bmax - node.bmin;
        parent_area = 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
    if (parent_area <= 0.0f) return;

    float best_cost = static_cast<float>(count);   // cost of leaving this a leaf
    int   best_axis = -1;
    float best_split = 0.0f;

    for (int axis = 0; axis < 3; ++axis) {
        float ext = (axis == 0) ? extent.x : (axis == 1) ? extent.y : extent.z;
        if (ext < 1e-8f) continue;

        float cmin_a = (axis == 0) ? cmin.x : (axis == 1) ? cmin.y : cmin.z;
        float scale  = kBinCount / ext;

        Vector3 bin_min[kBinCount], bin_max[kBinCount];
        int     bin_count[kBinCount];
        for (int b = 0; b < kBinCount; ++b) {
            bin_min[b] = Vector3{ kInf, kInf, kInf };
            bin_max[b] = Vector3{ -kInf, -kInf, -kInf };
            bin_count[b] = 0;
        }

        for (int i = 0; i < count; ++i) {
            const BuildPrim& p = prims[indices_[first + i]];
            float c = (axis == 0) ? p.centroid.x : (axis == 1) ? p.centroid.y : p.centroid.z;
            int b = std::min(kBinCount - 1, static_cast<int>((c - cmin_a) * scale));
            if (b < 0) b = 0;
            bin_count[b]++;
            bin_min[b] = vmin(bin_min[b], p.bmin);
            bin_max[b] = vmax(bin_max[b], p.bmax);
        }

        // Sweep right-to-left, then left-to-right, accumulating SAH terms.
        float right_area[kBinCount];
        int   right_count[kBinCount];
        Vector3 acc_min{ kInf, kInf, kInf }, acc_max{ -kInf, -kInf, -kInf };
        int acc_n = 0;
        for (int b = kBinCount - 1; b >= 0; --b) {
            if (bin_count[b] > 0) {
                acc_min = vmin(acc_min, bin_min[b]);
                acc_max = vmax(acc_max, bin_max[b]);
                acc_n  += bin_count[b];
            }
            right_count[b] = acc_n;
            if (acc_n > 0) {
                Vector3 d = acc_max - acc_min;
                right_area[b] = 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
            } else {
                right_area[b] = 0.0f;
            }
        }

        acc_min = Vector3{ kInf, kInf, kInf };
        acc_max = Vector3{ -kInf, -kInf, -kInf };
        acc_n = 0;
        for (int b = 0; b < kBinCount - 1; ++b) {
            if (bin_count[b] > 0) {
                acc_min = vmin(acc_min, bin_min[b]);
                acc_max = vmax(acc_max, bin_max[b]);
                acc_n  += bin_count[b];
            }
            if (acc_n == 0 || right_count[b + 1] == 0) continue;

            Vector3 d = acc_max - acc_min;
            float left_area = 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);

            float cost = 0.125f + (left_area * acc_n + right_area[b + 1] * right_count[b + 1]) / parent_area;
            if (cost < best_cost) {
                best_cost  = cost;
                best_axis  = axis;
                best_split = cmin_a + (b + 1) / scale;
            }
        }
    }

    if (best_axis < 0) return;   // splitting costs more than tracing the leaf

    // In-place partition.
    int mid = first;
    for (int i = first; i < first + count; ++i) {
        const Vector3& c = prims[indices_[i]].centroid;
        float cv = (best_axis == 0) ? c.x : (best_axis == 1) ? c.y : c.z;
        if (cv < best_split) {
            std::swap(indices_[i], indices_[mid]);
            ++mid;
        }
    }

    int left_count = mid - first;
    if (left_count == 0 || left_count == count) return;

    auto bounds_of = [&](int begin, int n_prims, Vector3& bmin, Vector3& bmax) {
        bmin = Vector3{ kInf, kInf, kInf };
        bmax = Vector3{ -kInf, -kInf, -kInf };
        for (int i = 0; i < n_prims; ++i) {
            const BuildPrim& p = prims[indices_[begin + i]];
            bmin = vmin(bmin, p.bmin);
            bmax = vmax(bmax, p.bmax);
        }
    };

    TeslaBVHNode left, right;
    bounds_of(first, left_count, left.bmin, left.bmax);
    left.left_first = first;
    left.count = left_count;

    bounds_of(mid, count - left_count, right.bmin, right.bmax);
    right.left_first = mid;
    right.count = count - left_count;

    int left_index = static_cast<int>(nodes_.size());
    nodes_.push_back(left);
    nodes_.push_back(right);

    nodes_[node_index].left_first = left_index;
    nodes_[node_index].count = 0;

    subdivide(left_index,     prims, depth + 1);
    subdivide(left_index + 1, prims, depth + 1);
}

// ===========================================================================
// TeslaRenderer - scene
// ===========================================================================

TeslaRenderer::TeslaRenderer() = default;

TeslaRenderer::~TeslaRenderer() {
    shutdown_gpu();
}

void TeslaRenderer::begin_scene() {
    // Textures deliberately survive: they are keyed by GL id and re-registered by
    // the same materials every rebuild, and reading them back from the driver on
    // every scene change would be pointless work.
    triangles_.clear();
    materials_.clear();
    lights_.clear();
    emitters_.clear();
    emitter_cdf_.clear();
    emitter_area_total_ = 0.0f;
}

int TeslaRenderer::add_material(const TeslaMaterial& m) {
    materials_.push_back(m);
    return static_cast<int>(materials_.size()) - 1;
}

void TeslaRenderer::add_triangle(const TeslaTriangle& t) {
    triangles_.push_back(t);
}

void TeslaRenderer::add_light(const TeslaLight& l) {
    lights_.push_back(l);
}

void TeslaRenderer::set_sky(const TeslaSky& s) {
    sky_ = s;
}

int TeslaRenderer::add_texture(unsigned int gl_texture) {
    if (gl_texture == 0) return -1;

    for (size_t i = 0; i < texture_source_ids_.size(); ++i) {
        if (texture_source_ids_[i] == gl_texture) return static_cast<int>(i);
    }
    if (static_cast<int>(textures_.size()) >= kMaxTextures) {
        std::cerr << "[TESLA] texture limit (" << kMaxTextures
                  << ") reached; further materials will render untextured." << std::endl;
        return -1;
    }

    // TextureResource frees its CPU copy once the upload is done, so the pixels come
    // back from the driver rather than from the resource.
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    int src_w = 0, src_h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &src_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &src_h);
    if (src_w <= 0 || src_h <= 0) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return -1;
    }

    std::vector<unsigned char> src(static_cast<size_t>(src_w) * src_h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Box-average into the shared size. Averaging rather than point-sampling matters
    // when a 2048-wide source is being reduced: dropping texels instead of averaging
    // them turns fine detail into aliasing that no amount of path-tracing samples
    // will remove, because it is baked into the texture itself.
    std::vector<unsigned char> dst(static_cast<size_t>(kTextureSize) * kTextureSize * 4);
    for (int y = 0; y < kTextureSize; ++y) {
        int sy0 = static_cast<int>(static_cast<int64_t>(y)     * src_h / kTextureSize);
        int sy1 = static_cast<int>(static_cast<int64_t>(y + 1) * src_h / kTextureSize);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < kTextureSize; ++x) {
            int sx0 = static_cast<int>(static_cast<int64_t>(x)     * src_w / kTextureSize);
            int sx1 = static_cast<int>(static_cast<int64_t>(x + 1) * src_w / kTextureSize);
            if (sx1 <= sx0) sx1 = sx0 + 1;

            unsigned int acc[4] = { 0, 0, 0, 0 };
            unsigned int n = 0;
            for (int sy = sy0; sy < sy1 && sy < src_h; ++sy) {
                for (int sx = sx0; sx < sx1 && sx < src_w; ++sx) {
                    const unsigned char* t = &src[(static_cast<size_t>(sy) * src_w + sx) * 4];
                    acc[0] += t[0]; acc[1] += t[1]; acc[2] += t[2]; acc[3] += t[3];
                    ++n;
                }
            }
            unsigned char* d = &dst[(static_cast<size_t>(y) * kTextureSize + x) * 4];
            if (n == 0) n = 1;
            d[0] = static_cast<unsigned char>(acc[0] / n);
            d[1] = static_cast<unsigned char>(acc[1] / n);
            d[2] = static_cast<unsigned char>(acc[2] / n);
            d[3] = static_cast<unsigned char>(acc[3] / n);
        }
    }

    textures_.push_back(std::move(dst));
    texture_source_ids_.push_back(gl_texture);
    textures_dirty_ = true;
    return static_cast<int>(textures_.size()) - 1;
}

void TeslaRenderer::set_environment(const float* pixels, int width, int height, unsigned int gl_texture) {
    if (pixels && width > 0 && height > 0) {
        env_pixels_.assign(pixels, pixels + static_cast<size_t>(width) * height * 3);
        sky_.env_width  = width;
        sky_.env_height = height;
    } else {
        env_pixels_.clear();
        sky_.env_width = sky_.env_height = 0;
    }
    sky_.env_texture = gl_texture;
}

void TeslaRenderer::end_scene() {
    bvh_.build(triangles_);

    // Emitter table. Emissive triangles are in the BVH, so a scattered ray can land
    // on one; that is why they need both explicit sampling and MIS, unlike the
    // analytic lights.
    emitters_.clear();
    emitter_cdf_.clear();
    emitter_area_total_ = 0.0f;

    for (int i = 0; i < static_cast<int>(triangles_.size()); ++i) {
        const TeslaTriangle& t = triangles_[i];
        if (t.material < 0 || t.material >= static_cast<int>(materials_.size())) continue;
        if (is_black(materials_[t.material].emission)) continue;

        float area = 0.5f * Vector3::cross(t.v1 - t.v0, t.v2 - t.v0).length();
        if (area <= 0.0f) continue;

        emitters_.push_back(i);
        emitter_area_total_ += area;
        emitter_cdf_.push_back(emitter_area_total_);
    }
    for (auto& c : emitter_cdf_) {
        if (emitter_area_total_ > 0.0f) c /= emitter_area_total_;
    }

    // Cheap content hash so the caller can skip rebuilding an unchanged scene.
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    };
    if (!triangles_.empty()) mix(triangles_.data(), triangles_.size() * sizeof(TeslaTriangle));
    if (!materials_.empty()) mix(materials_.data(), materials_.size() * sizeof(TeslaMaterial));
    if (!lights_.empty())    mix(lights_.data(),    lights_.size()    * sizeof(TeslaLight));
    mix(&sky_, sizeof(sky_));
    scene_hash_ = h;

    gpu_uploaded_ = false;
    if (gpu_ready_) {
        upload_scene_to_gpu();
        upload_textures_to_gpu();
    }

    reset_accumulation();
}

void TeslaRenderer::set_camera(const Matrix4x4& view, const Matrix4x4& projection, const Vector3& position) {
    inv_view_ = view.inverse();
    inv_proj_ = projection.inverse();
    camera_position_ = position;
}

void TeslaRenderer::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_  = std::max(1, width);
    height_ = std::max(1, height);

    accum_.assign(static_cast<size_t>(width_) * height_, Vector3{ 0.0f, 0.0f, 0.0f });
    accum_counts_.assign(static_cast<size_t>(width_) * height_, 0.0f);
    upload_scratch_.assign(static_cast<size_t>(width_) * height_ * 4, 0.0f);

    constexpr int kTile = 16;
    tiles_x_ = (width_  + kTile - 1) / kTile;
    tiles_y_ = (height_ + kTile - 1) / kTile;

    if (gl_initialized_) ensure_accum_target();
    reset_accumulation();
}

void TeslaRenderer::reset_accumulation() {
    samples_done_ = 0;
    next_tile_.store(0);

    std::fill(accum_.begin(), accum_.end(), Vector3{ 0.0f, 0.0f, 0.0f });
    std::fill(accum_counts_.begin(), accum_counts_.end(), 0.0f);

    if (gl_initialized_ && accum_fbo_ != 0) {
        int prev_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, accum_fbo_);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prev_fbo));
    }
}

void TeslaRenderer::step() {
    if (is_complete()) return;
    if (width_ <= 0 || height_ <= 0) return;

    if (gpu_active()) {
        step_gpu();
    } else {
        step_cpu();
    }
}

// ===========================================================================
// CPU integrator
// ===========================================================================

void TeslaRenderer::step_cpu() {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() +
        std::chrono::microseconds(static_cast<long long>(settings_.cpu_time_budget_ms * 1000.0f));

    const int total_tiles = tiles_x_ * tiles_y_;
    if (total_tiles <= 0) return;

    unsigned int hw = std::thread::hardware_concurrency();
    const int worker_count = std::max(1u, hw);

    std::atomic<bool> out_of_time{ false };

    auto worker = [&]() {
        for (;;) {
            int tile = next_tile_.fetch_add(1);
            if (tile >= total_tiles) return;

            render_tile(tile, samples_done_);

            // Only the clock check is shared; each tile is independent, so a
            // partially finished pass leaves per-pixel counts correct.
            if ((tile & 7) == 0 && clock::now() >= deadline) {
                out_of_time.store(true, std::memory_order_relaxed);
                return;
            }
            if (out_of_time.load(std::memory_order_relaxed)) return;
        }
    };

    if (worker_count == 1) {
        worker();
    } else {
        std::vector<std::thread> threads;
        threads.reserve(worker_count);
        for (int i = 0; i < worker_count; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    }

    if (next_tile_.load() >= total_tiles) {
        next_tile_.store(0);
        ++samples_done_;
    }

    // Push sum + per-pixel count to the GPU; the present pass divides.
    const size_t pixels = static_cast<size_t>(width_) * height_;
    for (size_t i = 0; i < pixels; ++i) {
        upload_scratch_[i * 4 + 0] = accum_[i].x;
        upload_scratch_[i * 4 + 1] = accum_[i].y;
        upload_scratch_[i * 4 + 2] = accum_[i].z;
        upload_scratch_[i * 4 + 3] = accum_counts_[i];
    }

    if (accum_texture_ != 0) {
        glBindTexture(GL_TEXTURE_2D, accum_texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_FLOAT, upload_scratch_.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void TeslaRenderer::render_tile(int tile_index, int sample_index) {
    constexpr int kTile = 16;
    const int tx = (tile_index % tiles_x_) * kTile;
    const int ty = (tile_index / tiles_x_) * kTile;

    const int x_end = std::min(tx + kTile, width_);
    const int y_end = std::min(ty + kTile, height_);

    for (int y = ty; y < y_end; ++y) {
        for (int x = tx; x < x_end; ++x) {
            const uint32_t pixel = static_cast<uint32_t>(y) * width_ + x;
            uint64_t rng = seed_for(pixel, static_cast<uint32_t>(sample_index));

            // Jitter over the whole pixel footprint - a box reconstruction filter,
            // which is unbiased with respect to the pixel's average radiance.
            float jx = rnd(rng);
            float jy = rnd(rng);

            float ndc_x = (2.0f * (x + jx)) / width_  - 1.0f;
            float ndc_y = (2.0f * (y + jy)) / height_ - 1.0f;

            const auto& ip = inv_proj_.m;
            float e_x = ip[0] * ndc_x + ip[4] * ndc_y + ip[8]  * -1.0f + ip[12];
            float e_y = ip[1] * ndc_x + ip[5] * ndc_y + ip[9]  * -1.0f + ip[13];

            const auto& iv = inv_view_.m;
            Vector3 dir{
                iv[0] * e_x + iv[4] * e_y + iv[8]  * -1.0f,
                iv[1] * e_x + iv[5] * e_y + iv[9]  * -1.0f,
                iv[2] * e_x + iv[6] * e_y + iv[10] * -1.0f,
            };
            dir = dir.normalized();

            Vector3 radiance = trace(camera_position_, dir, rng);

            // NaN/Inf guard. A non-finite sample would poison the running mean for
            // good, and dropping it is the only way to keep the average defined.
            if (std::isfinite(radiance.x) && std::isfinite(radiance.y) && std::isfinite(radiance.z)) {
                accum_[pixel] += radiance;
            }
            accum_counts_[pixel] += 1.0f;
        }
    }
}

Vector3 TeslaRenderer::trace(Vector3 origin, Vector3 dir, uint64_t& rng) const {
    Vector3 radiance{ 0.0f, 0.0f, 0.0f };
    Vector3 throughput{ 1.0f, 1.0f, 1.0f };

    const bool have_bvh = !bvh_.empty();
    const auto& nodes = bvh_.nodes();
    const auto& order = bvh_.indices();

    // pdf of the direction that produced the current ray, carried so a hit on an
    // emitter can be MIS-weighted against the light-sampling strategy.
    float bsdf_pdf = 0.0f;
    bool  prev_was_camera = true;

    auto sky_radiance = [&](const Vector3& d) -> Vector3 {
        if (!sky_.enabled) return { 0.0f, 0.0f, 0.0f };

        // Equirectangular environment, using the same mapping the raster path's
        // directionToEquirectUV does, so both renderers agree on which way is which.
        if (!env_pixels_.empty() && sky_.env_width > 0 && sky_.env_height > 0) {
            const int W = sky_.env_width, H = sky_.env_height;
            float u = std::atan2(d.z, d.x + 1e-5f) * (0.5f / kPi) + 0.5f;
            float v = std::asin(std::min(std::max(d.y, -1.0f), 1.0f)) * (1.0f / kPi) + 0.5f;

            float fx = u * W - 0.5f;
            float fy = v * H - 0.5f;
            int x0 = static_cast<int>(std::floor(fx));
            int y0 = static_cast<int>(std::floor(fy));
            float tx = fx - x0, ty = fy - y0;

            Vector3 acc{ 0.0f, 0.0f, 0.0f };
            for (int j = 0; j < 2; ++j) {
                int yy = std::min(std::max(y0 + j, 0), H - 1);          // clamp latitude
                float wy = j ? ty : (1.0f - ty);
                for (int i = 0; i < 2; ++i) {
                    int xx = ((x0 + i) % W + W) % W;                    // wrap longitude
                    float wx = i ? tx : (1.0f - tx);
                    const float* t = &env_pixels_[(static_cast<size_t>(yy) * W + xx) * 3];
                    float w = wx * wy;
                    acc += Vector3{ t[0] * w, t[1] * w, t[2] * w };
                }
            }
            return acc * sky_.intensity;
        }

        float up = std::min(std::max(d.y, -1.0f), 1.0f);
        Vector3 c = lerp3(sky_.horizon, sky_.zenith, std::max(0.0f, up));
        float down = std::min(std::max(-up * 5.0f, 0.0f), 1.0f);
        c = lerp3(c, sky_.ground, down);
        return c * sky_.intensity;
    };

    auto occluded = [&](const Vector3& o, const Vector3& d, float dist) -> bool {
        if (!have_bvh) return false;
        Vector3 inv{ 1.0f / (d.x == 0.0f ? 1e-20f : d.x),
                     1.0f / (d.y == 0.0f ? 1e-20f : d.y),
                     1.0f / (d.z == 0.0f ? 1e-20f : d.z) };
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        const float tmax = dist - kEps;
        while (sp > 0) {
            const TeslaBVHNode& node = nodes[stack[--sp]];
            float t_enter;
            if (!slab_test(o, inv, node.bmin, node.bmax, kEps, tmax, t_enter)) continue;
            if (node.count > 0) {
                for (int i = 0; i < node.count; ++i) {
                    const TeslaTriangle& tri = triangles_[order[node.left_first + i]];
                    float t, u, v;
                    if (intersect_triangle(o, d, tri, kEps, tmax, t, u, v)) return true;
                }
            } else {
                stack[sp++] = node.left_first;
                stack[sp++] = node.left_first + 1;
            }
        }
        return false;
    };

    auto closest_hit = [&](const Vector3& o, const Vector3& d) -> Hit {
        Hit hit;
        if (!have_bvh) return hit;
        Vector3 inv{ 1.0f / (d.x == 0.0f ? 1e-20f : d.x),
                     1.0f / (d.y == 0.0f ? 1e-20f : d.y),
                     1.0f / (d.z == 0.0f ? 1e-20f : d.z) };
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const TeslaBVHNode& node = nodes[stack[--sp]];
            float t_enter;
            if (!slab_test(o, inv, node.bmin, node.bmax, kEps, hit.t, t_enter)) continue;
            if (node.count > 0) {
                for (int i = 0; i < node.count; ++i) {
                    int prim = order[node.left_first + i];
                    float t, u, v;
                    if (intersect_triangle(o, d, triangles_[prim], kEps, hit.t, t, u, v)) {
                        hit.t = t; hit.u = u; hit.v = v; hit.prim = prim;
                    }
                }
            } else {
                stack[sp++] = node.left_first;
                stack[sp++] = node.left_first + 1;
            }
        }
        return hit;
    };

    for (int depth = 0;; ++depth) {
        if (settings_.max_depth > 0 && depth >= settings_.max_depth) break;

        Hit hit = closest_hit(origin, dir);

        // ---- escaped: environment ----
        if (hit.prim < 0) {
            // The sky is a smooth function and carries no sun disc, so BSDF
            // sampling alone estimates it correctly - no MIS partner, weight 1.
            radiance += throughput * sky_radiance(dir);
            break;
        }

        const TeslaTriangle& tri = triangles_[hit.prim];
        const TeslaMaterial& mat = materials_[std::min(std::max(tri.material, 0),
                                                       static_cast<int>(materials_.size()) - 1)];

        const float w = 1.0f - hit.u - hit.v;

        // Albedo map, multiplied into the base colour exactly as the rasteriser's
        // geometry pass does. Sampled once and reused for emission so a textured
        // emitter glows in its own colours.
        Vector3 tex_tint{ 1.0f, 1.0f, 1.0f };
        if (mat.albedo_texture >= 0 && mat.albedo_texture < static_cast<int>(textures_.size())) {
            float tu = tri.uv0.x * w + tri.uv1.x * hit.u + tri.uv2.x * hit.v;
            float tv = tri.uv0.y * w + tri.uv1.y * hit.u + tri.uv2.y * hit.v;
            tex_tint = sample_texture_rgba8(textures_[mat.albedo_texture].data(), tu, tv);
        }
        const Vector3 surface_albedo = mat.base_color * tex_tint;
        Vector3 shading_normal = (tri.n0 * w + tri.n1 * hit.u + tri.n2 * hit.v).normalized();
        Vector3 geo_normal = Vector3::cross(tri.v1 - tri.v0, tri.v2 - tri.v0).normalized();

        // Two-sided surfaces: flip both normals to the side the ray arrived from.
        // Without this, a back-face hit builds its hemisphere below the surface and
        // every subsequent sample is wrong.
        const bool backface = Vector3::dot(geo_normal, dir) > 0.0f;
        if (backface) {
            geo_normal = neg(geo_normal);
            shading_normal = neg(shading_normal);
        }
        if (Vector3::dot(shading_normal, geo_normal) < 0.0f) shading_normal = geo_normal;

        const Vector3 hit_point = origin + dir * hit.t;
        const Vector3 wo_world = neg(dir);

        // ---- emitted radiance, MIS-weighted against light sampling ----
        if (!is_black(mat.emission)) {
            float weight = 1.0f;
            if (!prev_was_camera && emitter_area_total_ > 0.0f) {
                float area = 0.5f * Vector3::cross(tri.v1 - tri.v0, tri.v2 - tri.v0).length();
                float cos_light = std::abs(Vector3::dot(geo_normal, dir));
                if (area > 0.0f && cos_light > 1e-6f) {
                    // Same measure as the NEE pdf below: uniform over total emitter
                    // area, converted to solid angle.
                    float pdf_area  = 1.0f / emitter_area_total_;
                    float pdf_light = pdf_area * (hit.t * hit.t) / cos_light;
                    weight = mis_power(bsdf_pdf, pdf_light);
                }
            }
            radiance += throughput * mat.emission * tex_tint * weight;
        }

        Frame frame;
        frame.from_normal(shading_normal);
        Vector3 wo = frame.to_local(wo_world);
        if (wo.z <= 0.0f) break;

        BSDF bsdf;
        bsdf.setup(surface_albedo, mat.metallic, mat.roughness, wo.z);

        const Vector3 shadow_origin = hit_point + geo_normal * kEps;

        // ---------------------------------------------------------------
        // Next event estimation - analytic lights.
        // None of them are in the BVH, so a scattered ray can never hit one and
        // there is nothing to MIS against. Each contribution is
        //   f * cos(theta) * L / pdf
        // with the pdf that actually generated the sample.
        // ---------------------------------------------------------------
        for (const TeslaLight& light : lights_) {
            Vector3 emitted = light.color * light.intensity;
            if (is_black(emitted)) continue;

            Vector3 to_light;          // unit, surface -> light
            float   dist = kInf;
            float   inv_pdf = 0.0f;    // 1/pdf in solid angle, or the delta form
            Vector3 radiance_in{ 0.0f, 0.0f, 0.0f };

            switch (light.type) {
            case TeslaLightType::Directional: {
                Vector3 axis = neg(light.direction).normalized();
                if (light.angular_radius > 0.0f) {
                    float cos_max = std::cos(light.angular_radius);
                    Frame lf; lf.from_normal(axis);
                    Vector3 local = sample_uniform_cone(rnd(rng), rnd(rng), cos_max);
                    to_light = lf.to_world(local).normalized();
                    float pdf = uniform_cone_pdf(cos_max);
                    if (pdf <= 0.0f) continue;
                    inv_pdf = 1.0f / pdf;
                    // Radiance chosen so the irradiance matches `intensity`
                    // regardless of the disc size: L * solid_angle = E.
                    radiance_in = emitted * (1.0f / (2.0f * kPi * (1.0f - cos_max)));
                } else {
                    to_light = axis;
                    inv_pdf = 1.0f;                 // delta light: E is given directly
                    radiance_in = emitted;
                }
                dist = kInf;
                break;
            }
            case TeslaLightType::Point:
            case TeslaLightType::Spot: {
                Vector3 offset = light.position - hit_point;
                float d2 = Vector3::dot(offset, offset);
                if (d2 < 1e-12f) continue;
                float d = std::sqrt(d2);

                if (light.radius > 0.0f && light.radius < d) {
                    // Sphere light: sample the cone it subtends.
                    float sin2 = (light.radius * light.radius) / d2;
                    float cos_max = std::sqrt(std::max(0.0f, 1.0f - sin2));
                    Frame lf; lf.from_normal(offset * (1.0f / d));
                    Vector3 local = sample_uniform_cone(rnd(rng), rnd(rng), cos_max);
                    to_light = lf.to_world(local).normalized();
                    float pdf = uniform_cone_pdf(cos_max);
                    if (pdf <= 0.0f) continue;
                    inv_pdf = 1.0f / pdf;
                    // Uniform sphere of radius r with radiance L has intensity
                    // I = L * pi * r^2, so L = I / (pi r^2). As r -> 0 this
                    // reproduces the inverse-square point light exactly.
                    radiance_in = emitted * (1.0f / (kPi * light.radius * light.radius));
                    dist = d;   // conservative: shadow up to the sphere centre
                } else {
                    to_light = offset * (1.0f / d);
                    inv_pdf = 1.0f / d2;            // delta light: I/d^2
                    radiance_in = emitted;
                    dist = d;
                }

                if (light.type == TeslaLightType::Spot) {
                    float cos_angle = Vector3::dot(neg(to_light), light.direction.normalized());
                    float denom = light.cos_inner - light.cos_outer;
                    float falloff = denom > 1e-6f
                        ? std::min(1.0f, std::max(0.0f, (cos_angle - light.cos_outer) / denom))
                        : (cos_angle >= light.cos_outer ? 1.0f : 0.0f);
                    if (falloff <= 0.0f) continue;
                    radiance_in = radiance_in * falloff;
                }
                break;
            }
            case TeslaLightType::Area: {
                // Uniform by area, then converted to the solid angle measure.
                float su = rnd(rng) * 2.0f - 1.0f;
                float sv = rnd(rng) * 2.0f - 1.0f;
                Vector3 point = light.position + light.u_axis * su + light.v_axis * sv;

                Vector3 offset = point - hit_point;
                float d2 = Vector3::dot(offset, offset);
                if (d2 < 1e-12f) continue;
                float d = std::sqrt(d2);
                to_light = offset * (1.0f / d);

                Vector3 light_normal = Vector3::cross(light.u_axis, light.v_axis).normalized();
                float cos_light = Vector3::dot(light_normal, neg(to_light));
                if (cos_light <= 0.0f) continue;      // one-sided emitter

                float area = 4.0f * Vector3::cross(light.u_axis, light.v_axis).length();
                if (area <= 0.0f) continue;

                float pdf_area  = 1.0f / area;
                float pdf_solid = pdf_area * d2 / cos_light;
                if (pdf_solid <= 0.0f) continue;
                inv_pdf = 1.0f / pdf_solid;
                radiance_in = emitted;              // intensity read as radiance
                dist = d;
                break;
            }
            }

            Vector3 wi = frame.to_local(to_light);
            if (wi.z <= 0.0f) continue;

            Vector3 f = bsdf.eval(wo, wi);
            if (is_black(f)) continue;

            float shadow_dist = (dist == kInf) ? 1e30f : dist;
            if (occluded(shadow_origin, to_light, shadow_dist)) continue;

            radiance += throughput * f * radiance_in * (wi.z * inv_pdf);
        }

        // ---------------------------------------------------------------
        // Next event estimation - emissive geometry, with MIS.
        // ---------------------------------------------------------------
        if (!emitters_.empty() && emitter_area_total_ > 0.0f) {
            float u = rnd(rng);
            size_t lo = 0, hi = emitter_cdf_.size() - 1;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (emitter_cdf_[mid] < u) lo = mid + 1; else hi = mid;
            }
            int prim = emitters_[lo];
            const TeslaTriangle& etri = triangles_[prim];
            const TeslaMaterial& emat = materials_[etri.material];

            float su = rnd(rng);
            float sv = rnd(rng);
            float s = std::sqrt(su);
            float b0 = 1.0f - s;
            float b1 = sv * s;
            float b2 = 1.0f - b0 - b1;
            Vector3 point = etri.v0 * b0 + etri.v1 * b1 + etri.v2 * b2;

            Vector3 offset = point - hit_point;
            float d2 = Vector3::dot(offset, offset);
            if (d2 > 1e-12f) {
                float d = std::sqrt(d2);
                Vector3 to_light = offset * (1.0f / d);

                Vector3 enormal = Vector3::cross(etri.v1 - etri.v0, etri.v2 - etri.v0).normalized();
                float cos_light = std::abs(Vector3::dot(enormal, neg(to_light)));

                if (cos_light > 1e-6f) {
                    // Choosing the emitter proportional to area makes the joint pdf
                    // uniform over the total emitting area.
                    float pdf_area  = 1.0f / emitter_area_total_;
                    float pdf_solid = pdf_area * d2 / cos_light;

                    Vector3 wi = frame.to_local(to_light);
                    if (wi.z > 0.0f && pdf_solid > 0.0f) {
                        Vector3 f = bsdf.eval(wo, wi);
                        if (!is_black(f) && !occluded(shadow_origin, to_light, d)) {
                            float pdf_b = bsdf.pdf(wo, wi);
                            float weight = mis_power(pdf_solid, pdf_b);
                            radiance += throughput * f * emat.emission * (wi.z * weight / pdf_solid);
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------
        // BSDF sampling - continue the path.
        // ---------------------------------------------------------------
        Vector3 wi, f;
        float pdf = 0.0f;
        if (!bsdf.sample(wo, rnd(rng), rnd(rng), rnd(rng), wi, f, pdf)) break;

        Vector3 contribution = f * (wi.z / pdf);
        if (is_black(contribution)) break;

        throughput = throughput * contribution;
        bsdf_pdf = pdf;
        prev_was_camera = false;

        Vector3 next_dir = frame.to_world(wi).normalized();
        // Offset along the geometric normal, which is the only one guaranteed to
        // point out of the surface.
        origin = hit_point + geo_normal * kEps;
        dir = next_dir;

        if (settings_.firefly_clamp > 0.0f) {
            float m = vmax_comp(throughput);
            if (m > settings_.firefly_clamp) throughput = throughput * (settings_.firefly_clamp / m);
        }

        // ---------------------------------------------------------------
        // Russian roulette. Terminating with probability (1-q) and dividing the
        // survivors by q leaves the expected value untouched - this is what makes
        // ending a path early unbiased, unlike a fixed depth cut.
        // ---------------------------------------------------------------
        if (depth + 1 >= settings_.rr_start_depth) {
            float q = std::min(settings_.rr_max_survival, std::max(0.02f, vmax_comp(throughput)));
            if (rnd(rng) >= q) break;
            throughput = throughput * (1.0f / q);
        }
    }

    return radiance;
}

// ===========================================================================
// GPU backend
// ===========================================================================

static const char* kTeslaVertexSource = R"(#version 330 core
layout (location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// The fragment shader below is a line-for-line mirror of TeslaRenderer::trace.
// Same pdfs, same MIS weights, same roulette, same RNG - so the two backends are
// interchangeable and converge to the same image.
static const char* kTeslaFragmentSource = R"(#version 330 core
out vec4 FragColor;
in vec2 vUV;

uniform samplerBuffer  uTriangles;   // 6 texels per triangle
uniform samplerBuffer  uNodes;       // 2 texels per BVH node
uniform isamplerBuffer uIndices;     // primitive order
uniform samplerBuffer  uMaterials;   // 2 texels per material
uniform samplerBuffer  uLights;      // 6 texels per light
uniform samplerBuffer  uEmitters;    // 1 texel per emitter: (primIndex, cdf, area, 0)
uniform sampler2D      uAlbedoLUT;   // r = A, g = B in rho_spec = f0*A + B
uniform sampler2D      uEnvMap;      // equirectangular environment
uniform int            uHasEnvMap;
uniform sampler2DArray uTextures;    // albedo maps, one layer each

uniform mat4  uInvView;
uniform mat4  uInvProj;
uniform vec3  uCameraPos;

uniform int   uLightCount;
uniform int   uEmitterCount;
uniform float uEmitterAreaTotal;
uniform int   uHasBVH;

uniform vec3  uSkyZenith;
uniform vec3  uSkyHorizon;
uniform vec3  uSkyGround;
uniform float uSkyIntensity;
uniform int   uSkyEnabled;

uniform int   uSampleIndex;
uniform vec2  uResolution;
uniform int   uMaxDepth;
uniform int   uRRStartDepth;
uniform float uRRMaxSurvival;
uniform float uFireflyClamp;

const float PI     = 3.14159265358979323846;
const float INV_PI = 0.31830988618379067154;
const float EPS    = 1e-4;
const float INF    = 3.402823466e+38;
const float MIN_ALPHA = 1e-3;

// ---------------------------------------------------------------------------
// PCG32, on a 64-bit state emulated as uvec2(hi, lo) because core GLSL has no
// 64-bit integers. The arithmetic is bit-for-bit the CPU implementation, so both
// backends walk the same sequence.
// ---------------------------------------------------------------------------
uvec2 gRngState;

uvec2 u64_add(uvec2 a, uvec2 b) {
    uint lo = a.y + b.y;
    uint carry = (lo < a.y) ? 1u : 0u;
    return uvec2(a.x + b.x + carry, lo);
}

// 32x32 -> 64 by 16-bit halves. umulExtended would do this in one call but it is
// GLSL 4.00, and targeting 3.30 is what lets the GPU backend run on hardware that
// reports a 3.3 context rather than only under a software rasteriser.
uvec2 mul32(uint a, uint b) {
    uint a0 = a & 0xFFFFu, a1 = a >> 16u;
    uint b0 = b & 0xFFFFu, b1 = b >> 16u;
    uint p00 = a0 * b0;
    uint mid = a0 * b1 + a1 * b0;
    uint carry_mid = (mid < a0 * b1) ? 0x10000u : 0u;   // mid overflowed 32 bits
    uint lo = p00 + (mid << 16u);
    uint carry_lo = (lo < p00) ? 1u : 0u;
    uint hi = a1 * b1 + (mid >> 16u) + carry_mid + carry_lo;
    return uvec2(hi, lo);
}

uvec2 u64_mul(uvec2 a, uvec2 b) {
    uvec2 lolo = mul32(a.y, b.y);
    return uvec2(lolo.x + a.x * b.y + a.y * b.x, lolo.y);
}

uvec2 u64_shr(uvec2 a, uint s) {   // s in [1,31]
    return uvec2(a.x >> s, (a.y >> s) | (a.x << (32u - s)));
}

uint pcg_next() {
    // state = state * 6364136223846793005 + 1442695040888963407
    gRngState = u64_add(u64_mul(gRngState, uvec2(0x5851F42Du, 0x4C957F2Du)),
                        uvec2(0x14057B7Eu, 0xF767814Fu));
    uvec2 t = gRngState ^ u64_shr(gRngState, 18u);   // (state >> 18) ^ state
    uint xorshifted = u64_shr(t, 27u).y;             // low 32 bits of (t >> 27)
    uint rot = gRngState.x >> 27u;                   // state >> 59
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
}

float rnd() {
    return float(pcg_next() >> 8) * (1.0 / 16777216.0);
}

uvec2 splitmix64(uvec2 x) {
    x = u64_add(x, uvec2(0x9E3779B9u, 0x7F4A7C15u));
    x = u64_mul(x ^ u64_shr(x, 30u), uvec2(0xBF58476Du, 0x1CE4E5B9u));
    x = u64_mul(x ^ u64_shr(x, 27u), uvec2(0x94D049BBu, 0x133111EBu));
    return x ^ u64_shr(x, 31u);
}

void seed_rng(uint pixel, uint sample_index) {
    // Matches seed_for(): (sample << 32) ^ pixel ^ 0xDA3E39CB94B95BDB
    gRngState = splitmix64(uvec2(sample_index ^ 0xDA3E39CBu, pixel ^ 0x94B95BDBu));
}

// ---------------------------------------------------------------------------
// Scene access
// ---------------------------------------------------------------------------
struct Triangle { vec3 v0, v1, v2, n0, n1, n2; vec2 uv0, uv1, uv2; int material; };
struct Material { vec3 base_color; float metallic; vec3 emission; float roughness; int albedo_texture; };

// 7 RGBA texels per triangle: the UVs ride in the spare .w lanes so only one extra
// fetch is needed for the last coordinate.
Triangle fetch_triangle(int i) {
    int o = i * 7;
    vec4 t0 = texelFetch(uTriangles, o + 0);
    vec4 t1 = texelFetch(uTriangles, o + 1);
    vec4 t2 = texelFetch(uTriangles, o + 2);
    vec4 t3 = texelFetch(uTriangles, o + 3);
    vec4 t4 = texelFetch(uTriangles, o + 4);
    vec4 t5 = texelFetch(uTriangles, o + 5);
    vec4 t6 = texelFetch(uTriangles, o + 6);
    Triangle t;
    t.v0 = t0.xyz; t.v1 = t1.xyz; t.v2 = t2.xyz;
    t.n0 = t3.xyz; t.n1 = t4.xyz; t.n2 = t5.xyz;
    t.uv0 = vec2(t1.w, t2.w);
    t.uv1 = vec2(t3.w, t4.w);
    t.uv2 = vec2(t5.w, t6.x);
    t.material = int(t0.w);
    return t;
}

Material fetch_material(int i) {
    int o = i * 3;
    vec4 m0 = texelFetch(uMaterials, o + 0);
    vec4 m1 = texelFetch(uMaterials, o + 1);
    vec4 m2 = texelFetch(uMaterials, o + 2);
    Material m;
    m.base_color = m0.rgb; m.metallic = m0.a;
    m.emission = m1.rgb;   m.roughness = m1.a;
    m.albedo_texture = int(m2.x);
    return m;
}

// ---------------------------------------------------------------------------
// Sampling primitives
// ---------------------------------------------------------------------------
void build_frame(vec3 n, out vec3 t, out vec3 b) {
    float sgn = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sgn + n.z);
    float bb = n.x * n.y * a;
    t = vec3(1.0 + sgn * n.x * n.x * a, sgn * bb, -sgn * n.x);
    b = vec3(bb, sgn + n.y * n.y * a, -n.y);
}

vec3 to_local(vec3 v, vec3 t, vec3 b, vec3 n) { return vec3(dot(v,t), dot(v,b), dot(v,n)); }
vec3 to_world(vec3 v, vec3 t, vec3 b, vec3 n) { return t*v.x + b*v.y + n*v.z; }

vec3 sample_cosine_hemisphere(float u1, float u2) {
    float r = sqrt(u1);
    float phi = 2.0 * PI * u2;
    return vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
}
float cosine_hemisphere_pdf(float c) { return c > 0.0 ? c * INV_PI : 0.0; }

vec3 sample_uniform_cone(float u1, float u2, float cos_max) {
    float ct = 1.0 - u1 * (1.0 - cos_max);
    float st = sqrt(max(0.0, 1.0 - ct * ct));
    float phi = 2.0 * PI * u2;
    return vec3(st * cos(phi), st * sin(phi), ct);
}
float uniform_cone_pdf(float cos_max) {
    float sa = 2.0 * PI * (1.0 - cos_max);
    return sa > 0.0 ? 1.0 / sa : 0.0;
}

float ggx_D(float ctm, float alpha) {
    if (ctm <= 0.0) return 0.0;
    float a2 = alpha * alpha;
    float c2 = ctm * ctm;
    float d = c2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float smith_lambda(float ct, float alpha) {
    float c = abs(ct);
    if (c >= 1.0) return 0.0;
    float a2 = alpha * alpha;
    float tan2 = (1.0 - c*c) / (c*c);
    return 0.5 * (-1.0 + sqrt(1.0 + a2 * tan2));
}
float smith_G1(float ct, float alpha) { return 1.0 / (1.0 + smith_lambda(ct, alpha)); }
float smith_G2(float co, float ci, float alpha) {
    return 1.0 / (1.0 + smith_lambda(co, alpha) + smith_lambda(ci, alpha));
}

vec3 fresnel_schlick(vec3 f0, float ct) {
    return f0 + (vec3(1.0) - f0) * pow(max(0.0, 1.0 - ct), 5.0);
}

vec3 sample_ggx_vndf(vec3 wo, float alpha, float u1, float u2) {
    vec3 Vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);
    float r = sqrt(u1);
    float phi = 2.0 * PI * u2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
    vec3 Nh = T1 * t1 + T2 * t2 + Vh * sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2));
    return normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
}

float ggx_vndf_pdf(vec3 wo, vec3 wm, float alpha) {
    if (wo.z <= 0.0) return 0.0;
    return smith_G1(wo.z, alpha) * ggx_D(wm.z, alpha) / (4.0 * wo.z);
}

float mis_power(float a, float b) {
    float aa = a * a, bb = b * b;
    float s = aa + bb;
    return s > 0.0 ? aa / s : 0.0;
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// ---------------------------------------------------------------------------
// BSDF
// ---------------------------------------------------------------------------
struct BSDF {
    vec3 albedo; float metallic; float alpha;
    vec3 f0; vec3 diffuse_scale; vec3 ms_compensation; float p_spec;
};

BSDF make_bsdf(vec3 base, float metallic, float roughness, float cos_theta_o) {
    BSDF b;
    b.albedo = base;
    b.metallic = clamp(metallic, 0.0, 1.0);
    float r = clamp(roughness, 0.0, 1.0);
    b.alpha = max(MIN_ALPHA, r * r);
    b.f0 = mix(vec3(0.04), base, b.metallic);

    // Same table the CPU backend uses, filtered by the hardware instead of by hand.
    vec2 ab = texture(uAlbedoLUT, vec2(clamp(cos_theta_o, 0.0, 1.0), r)).rg;

    // Restores the inter-facet scattering a single-scattering microfacet model drops.
    // A constant for a given wo, so it changes the BSDF without changing the
    // distribution being sampled - the estimator stays exact.
    float ess = max(1e-4, ab.x + ab.y);
    b.ms_compensation = vec3(1.0) + b.f0 * (1.0 / ess - 1.0);

    // What the specular layer actually reflects at this angle. Schlick is linear in
    // f0, hence the f0*A + B split.
    vec3 rho_specular = min((b.f0 * ab.x + vec3(ab.y)) * b.ms_compensation, vec3(1.0));

    // The diffuse substrate gets exactly the remainder - no more, so a white surface
    // never returns more than it received, and no less, so nothing goes missing.
    b.diffuse_scale = (vec3(1.0) - rho_specular) * (1.0 - b.metallic);

    float wd = luminance(base * b.diffuse_scale);
    float ws = luminance(rho_specular);
    float total = wd + ws;
    b.p_spec = total > 0.0 ? clamp(ws / total, 0.1, 0.9) : 0.5;
    return b;
}

vec3 bsdf_eval(BSDF b, vec3 wo, vec3 wi) {
    if (wo.z <= 0.0 || wi.z <= 0.0) return vec3(0.0);
    vec3 diffuse = b.albedo * b.diffuse_scale * INV_PI;
    vec3 wm = normalize(wo + wi);
    float D = ggx_D(wm.z, b.alpha);
    float G = smith_G2(wo.z, wi.z, b.alpha);
    vec3 F = fresnel_schlick(b.f0, max(0.0, dot(wi, wm)));
    return diffuse + F * b.ms_compensation * (D * G / (4.0 * wo.z * wi.z));
}

float bsdf_pdf(BSDF b, vec3 wo, vec3 wi) {
    if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
    vec3 wm = normalize(wo + wi);
    return b.p_spec * ggx_vndf_pdf(wo, wm, b.alpha)
         + (1.0 - b.p_spec) * cosine_hemisphere_pdf(wi.z);
}

bool bsdf_sample(BSDF b, vec3 wo, float ul, float u1, float u2,
                 out vec3 wi, out vec3 value, out float pdf) {
    wi = vec3(0.0); value = vec3(0.0); pdf = 0.0;
    if (wo.z <= 0.0) return false;
    if (ul < b.p_spec) {
        vec3 wm = sample_ggx_vndf(wo, b.alpha, u1, u2);
        wi = 2.0 * dot(wo, wm) * wm - wo;
        if (wi.z <= 0.0) return false;
    } else {
        wi = sample_cosine_hemisphere(u1, u2);
        if (wi.z <= 0.0) return false;
    }
    pdf = bsdf_pdf(b, wo, wi);
    if (pdf <= 0.0) return false;
    value = bsdf_eval(b, wo, wi);
    return any(greaterThan(value, vec3(0.0)));
}

// ---------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------
bool intersect_tri(vec3 org, vec3 dir, Triangle tri, float tmin, float tmax,
                   out float t, out float u, out float v) {
    t = 0.0; u = 0.0; v = 0.0;
    vec3 e1 = tri.v1 - tri.v0;
    vec3 e2 = tri.v2 - tri.v0;
    vec3 p = cross(dir, e2);
    float det = dot(e1, p);
    if (abs(det) < 1e-12) return false;
    float inv = 1.0 / det;
    vec3 s = org - tri.v0;
    u = dot(s, p) * inv;
    if (u < 0.0 || u > 1.0) return false;
    vec3 q = cross(s, e1);
    v = dot(dir, q) * inv;
    if (v < 0.0 || u + v > 1.0) return false;
    t = dot(e2, q) * inv;
    return t >= tmin && t <= tmax;
}

// A zero component would make 1/dir infinite and 0*inf a NaN in the slab test.
vec3 safe_dir(vec3 d) {
    return vec3(d.x == 0.0 ? 1e-20 : d.x,
                d.y == 0.0 ? 1e-20 : d.y,
                d.z == 0.0 ? 1e-20 : d.z);
}

bool slab(vec3 org, vec3 invd, vec3 bmin, vec3 bmax, float tmin, float tmax) {
    vec3 t0 = (bmin - org) * invd;
    vec3 t1 = (bmax - org) * invd;
    vec3 lo = min(t0, t1);
    vec3 hi = max(t0, t1);
    float enter = max(max(lo.x, lo.y), max(lo.z, tmin));
    float exit  = min(min(hi.x, hi.y), min(hi.z, tmax));
    return exit >= enter;
}

struct HitRec { float t; float u; float v; int prim; };

HitRec closest_hit(vec3 org, vec3 dir) {
    HitRec h;
    h.t = INF; h.u = 0.0; h.v = 0.0; h.prim = -1;
    if (uHasBVH == 0) return h;

    vec3 invd = 1.0 / safe_dir(dir);
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        int ni = stack[--sp];
        vec4 n0 = texelFetch(uNodes, ni * 2 + 0);
        vec4 n1 = texelFetch(uNodes, ni * 2 + 1);
        if (!slab(org, invd, n0.xyz, n1.xyz, EPS, h.t)) continue;
        int left_first = floatBitsToInt(n0.w);
        int count = floatBitsToInt(n1.w);
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                int prim = texelFetch(uIndices, left_first + i).r;
                float t, u, v;
                if (intersect_tri(org, dir, fetch_triangle(prim), EPS, h.t, t, u, v)) {
                    h.t = t; h.u = u; h.v = v; h.prim = prim;
                }
            }
        } else if (sp < 62) {
            stack[sp++] = left_first;
            stack[sp++] = left_first + 1;
        }
    }
    return h;
}

bool occluded(vec3 org, vec3 dir, float dist) {
    if (uHasBVH == 0) return false;
    float tmax = dist - EPS;
    vec3 invd = 1.0 / safe_dir(dir);
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        int ni = stack[--sp];
        vec4 n0 = texelFetch(uNodes, ni * 2 + 0);
        vec4 n1 = texelFetch(uNodes, ni * 2 + 1);
        if (!slab(org, invd, n0.xyz, n1.xyz, EPS, tmax)) continue;
        int left_first = floatBitsToInt(n0.w);
        int count = floatBitsToInt(n1.w);
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                int prim = texelFetch(uIndices, left_first + i).r;
                float t, u, v;
                if (intersect_tri(org, dir, fetch_triangle(prim), EPS, tmax, t, u, v)) return true;
            }
        } else if (sp < 62) {
            stack[sp++] = left_first;
            stack[sp++] = left_first + 1;
        }
    }
    return false;
}

vec3 sky_radiance(vec3 d) {
    if (uSkyEnabled == 0) return vec3(0.0);
    if (uHasEnvMap != 0) {
        // Same mapping as directionToEquirectUV in the raster path.
        vec2 uv = vec2(atan(d.z, d.x + 1e-5), asin(clamp(d.y, -1.0, 1.0)));
        uv *= vec2(0.1591549, 0.3183099);
        uv += 0.5;
        return textureLod(uEnvMap, uv, 0.0).rgb * uSkyIntensity;
    }
    float up = clamp(d.y, -1.0, 1.0);
    vec3 c = mix(uSkyHorizon, uSkyZenith, max(0.0, up));
    c = mix(c, uSkyGround, clamp(-up * 5.0, 0.0, 1.0));
    return c * uSkyIntensity;
}

// ---------------------------------------------------------------------------
// Integrator
// ---------------------------------------------------------------------------
vec3 trace(vec3 origin, vec3 dir) {
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    float last_bsdf_pdf = 0.0;
    bool from_camera = true;

    for (int depth = 0; ; ++depth) {
        if (uMaxDepth > 0 && depth >= uMaxDepth) break;
        if (depth > 512) break;   // hard guard against a runaway loop on the GPU

        HitRec hit = closest_hit(origin, dir);
        if (hit.prim < 0) {
            radiance += throughput * sky_radiance(dir);
            break;
        }

        Triangle tri = fetch_triangle(hit.prim);
        Material mat = fetch_material(tri.material);

        float w = 1.0 - hit.u - hit.v;

        // Albedo map. textureLod at level 0 rather than texture(): screen-space
        // derivatives are meaningless for a scattered ray and undefined inside this
        // loop's non-uniform control flow, and a mip level is a prefiltered average
        // - i.e. a bias. Per-sample jitter resolves minification instead.
        vec3 tex_tint = vec3(1.0);
        if (mat.albedo_texture >= 0) {
            vec2 uv = tri.uv0 * w + tri.uv1 * hit.u + tri.uv2 * hit.v;
            tex_tint = textureLod(uTextures, vec3(fract(uv), float(mat.albedo_texture)), 0.0).rgb;
        }
        vec3 surface_albedo = mat.base_color * tex_tint;
        vec3 shading_normal = normalize(tri.n0 * w + tri.n1 * hit.u + tri.n2 * hit.v);
        vec3 geo_normal = normalize(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));

        if (dot(geo_normal, dir) > 0.0) {
            geo_normal = -geo_normal;
            shading_normal = -shading_normal;
        }
        if (dot(shading_normal, geo_normal) < 0.0) shading_normal = geo_normal;

        vec3 hit_point = origin + dir * hit.t;
        vec3 wo_world = -dir;

        if (any(greaterThan(mat.emission, vec3(0.0)))) {
            float weight = 1.0;
            if (!from_camera && uEmitterAreaTotal > 0.0) {
                float area = 0.5 * length(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
                float cos_light = abs(dot(geo_normal, dir));
                if (area > 0.0 && cos_light > 1e-6) {
                    float pdf_light = (1.0 / uEmitterAreaTotal) * (hit.t * hit.t) / cos_light;
                    weight = mis_power(last_bsdf_pdf, pdf_light);
                }
            }
            radiance += throughput * mat.emission * tex_tint * weight;
        }

        vec3 ft, fb;
        build_frame(shading_normal, ft, fb);
        vec3 wo = to_local(wo_world, ft, fb, shading_normal);
        if (wo.z <= 0.0) break;

        BSDF bsdf = make_bsdf(surface_albedo, mat.metallic, mat.roughness, wo.z);
        vec3 shadow_origin = hit_point + geo_normal * EPS;

        // ---- analytic lights: NEE only, MIS weight 1 ----
        for (int li = 0; li < uLightCount; ++li) {
            int o = li * 6;
            vec4 l0 = texelFetch(uLights, o + 0);   // color.rgb, intensity
            vec4 l1 = texelFetch(uLights, o + 1);   // position.xyz, radius
            vec4 l2 = texelFetch(uLights, o + 2);   // direction.xyz, angular_radius
            vec4 l3 = texelFetch(uLights, o + 3);   // cos_inner, cos_outer, type, -
            vec4 l4 = texelFetch(uLights, o + 4);   // u_axis.xyz
            vec4 l5 = texelFetch(uLights, o + 5);   // v_axis.xyz

            vec3 emitted = l0.rgb * l0.a;
            if (!any(greaterThan(emitted, vec3(0.0)))) continue;

            int type = int(l3.z);
            vec3 to_light = vec3(0.0);
            float dist = INF;
            float inv_pdf = 0.0;
            vec3 radiance_in = vec3(0.0);
            bool valid = true;

            if (type == 0) {                       // directional
                vec3 axis = normalize(-l2.xyz);
                if (l2.w > 0.0) {
                    float cos_max = cos(l2.w);
                    vec3 at, ab;
                    build_frame(axis, at, ab);
                    vec3 local = sample_uniform_cone(rnd(), rnd(), cos_max);
                    to_light = normalize(to_world(local, at, ab, axis));
                    float pdf = uniform_cone_pdf(cos_max);
                    if (pdf <= 0.0) valid = false;
                    else {
                        inv_pdf = 1.0 / pdf;
                        radiance_in = emitted / (2.0 * PI * (1.0 - cos_max));
                    }
                } else {
                    to_light = axis;
                    inv_pdf = 1.0;
                    radiance_in = emitted;
                }
                dist = INF;
            } else if (type == 1 || type == 2) {   // point / spot
                vec3 offset = l1.xyz - hit_point;
                float d2 = dot(offset, offset);
                if (d2 < 1e-12) valid = false;
                else {
                    float d = sqrt(d2);
                    float radius = l1.w;
                    if (radius > 0.0 && radius < d) {
                        float sin2 = (radius * radius) / d2;
                        float cos_max = sqrt(max(0.0, 1.0 - sin2));
                        vec3 at, ab;
                        vec3 axis = offset / d;
                        build_frame(axis, at, ab);
                        vec3 local = sample_uniform_cone(rnd(), rnd(), cos_max);
                        to_light = normalize(to_world(local, at, ab, axis));
                        float pdf = uniform_cone_pdf(cos_max);
                        if (pdf <= 0.0) valid = false;
                        else {
                            inv_pdf = 1.0 / pdf;
                            radiance_in = emitted / (PI * radius * radius);
                        }
                        dist = d;
                    } else {
                        to_light = offset / d;
                        inv_pdf = 1.0 / d2;
                        radiance_in = emitted;
                        dist = d;
                    }

                    if (valid && type == 2) {
                        float cos_angle = dot(-to_light, normalize(l2.xyz));
                        float denom = l3.x - l3.y;
                        float falloff = denom > 1e-6
                            ? clamp((cos_angle - l3.y) / denom, 0.0, 1.0)
                            : (cos_angle >= l3.y ? 1.0 : 0.0);
                        if (falloff <= 0.0) valid = false;
                        else radiance_in *= falloff;
                    }
                }
            } else {                               // area
                float su = rnd() * 2.0 - 1.0;
                float sv = rnd() * 2.0 - 1.0;
                vec3 point = l1.xyz + l4.xyz * su + l5.xyz * sv;
                vec3 offset = point - hit_point;
                float d2 = dot(offset, offset);
                if (d2 < 1e-12) valid = false;
                else {
                    float d = sqrt(d2);
                    to_light = offset / d;
                    vec3 lnormal = normalize(cross(l4.xyz, l5.xyz));
                    float cos_light = dot(lnormal, -to_light);
                    float area = 4.0 * length(cross(l4.xyz, l5.xyz));
                    if (cos_light <= 0.0 || area <= 0.0) valid = false;
                    else {
                        float pdf_solid = (1.0 / area) * d2 / cos_light;
                        if (pdf_solid <= 0.0) valid = false;
                        else {
                            inv_pdf = 1.0 / pdf_solid;
                            radiance_in = emitted;
                            dist = d;
                        }
                    }
                }
            }

            if (!valid) continue;

            vec3 wi = to_local(to_light, ft, fb, shading_normal);
            if (wi.z <= 0.0) continue;

            vec3 f = bsdf_eval(bsdf, wo, wi);
            if (!any(greaterThan(f, vec3(0.0)))) continue;

            float shadow_dist = (dist == INF) ? 1e30 : dist;
            if (occluded(shadow_origin, to_light, shadow_dist)) continue;

            radiance += throughput * f * radiance_in * (wi.z * inv_pdf);
        }

        // ---- emissive geometry: NEE + MIS ----
        if (uEmitterCount > 0 && uEmitterAreaTotal > 0.0) {
            float u = rnd();
            int lo = 0, hi = uEmitterCount - 1;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (texelFetch(uEmitters, mid).g < u) lo = mid + 1; else hi = mid;
            }
            vec4 e = texelFetch(uEmitters, lo);
            int prim = floatBitsToInt(e.r);
            Triangle etri = fetch_triangle(prim);
            Material emat = fetch_material(etri.material);

            float su = rnd(), sv = rnd();
            float s = sqrt(su);
            float b0 = 1.0 - s;
            float b1 = sv * s;
            float b2 = 1.0 - b0 - b1;
            vec3 point = etri.v0 * b0 + etri.v1 * b1 + etri.v2 * b2;

            vec3 offset = point - hit_point;
            float d2 = dot(offset, offset);
            if (d2 > 1e-12) {
                float d = sqrt(d2);
                vec3 to_light = offset / d;
                vec3 enormal = normalize(cross(etri.v1 - etri.v0, etri.v2 - etri.v0));
                float cos_light = abs(dot(enormal, -to_light));
                if (cos_light > 1e-6) {
                    float pdf_solid = (1.0 / uEmitterAreaTotal) * d2 / cos_light;
                    vec3 wi = to_local(to_light, ft, fb, shading_normal);
                    if (wi.z > 0.0 && pdf_solid > 0.0) {
                        vec3 f = bsdf_eval(bsdf, wo, wi);
                        if (any(greaterThan(f, vec3(0.0))) && !occluded(shadow_origin, to_light, d)) {
                            float weight = mis_power(pdf_solid, bsdf_pdf(bsdf, wo, wi));
                            radiance += throughput * f * emat.emission * (wi.z * weight / pdf_solid);
                        }
                    }
                }
            }
        }

        // ---- continue the path ----
        vec3 wi, f;
        float pdf;
        if (!bsdf_sample(bsdf, wo, rnd(), rnd(), rnd(), wi, f, pdf)) break;

        vec3 contribution = f * (wi.z / pdf);
        if (!any(greaterThan(contribution, vec3(0.0)))) break;

        throughput *= contribution;
        last_bsdf_pdf = pdf;
        from_camera = false;

        origin = hit_point + geo_normal * EPS;
        dir = normalize(to_world(wi, ft, fb, shading_normal));

        if (uFireflyClamp > 0.0) {
            float m = max(throughput.r, max(throughput.g, throughput.b));
            if (m > uFireflyClamp) throughput *= uFireflyClamp / m;
        }

        if (depth + 1 >= uRRStartDepth) {
            float q = min(uRRMaxSurvival, max(0.02, max(throughput.r, max(throughput.g, throughput.b))));
            if (rnd() >= q) break;
            throughput /= q;
        }
    }

    return radiance;
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    uint pixel = uint(px.y) * uint(uResolution.x) + uint(px.x);
    seed_rng(pixel, uint(uSampleIndex));

    float jx = rnd();
    float jy = rnd();

    vec2 ndc = vec2(
        (2.0 * (float(px.x) + jx)) / uResolution.x - 1.0,
        (2.0 * (float(px.y) + jy)) / uResolution.y - 1.0
    );

    vec4 eye = uInvProj * vec4(ndc, -1.0, 1.0);
    vec3 dir = normalize((uInvView * vec4(eye.xy, -1.0, 0.0)).xyz);

    vec3 radiance = trace(uCameraPos, dir);

    // A non-finite sample would corrupt the accumulator permanently.
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = vec3(0.0);

    // Additive blending turns this into sum(radiance) in rgb and the sample count
    // in alpha; the present pass divides one by the other.
    FragColor = vec4(radiance, 1.0);
}
)";

void TeslaRenderer::initialize_gpu() {
    if (gl_initialized_) return;

    // The accumulation target is needed whichever backend runs, so it is created
    // before anything that is allowed to fail.
    glGenFramebuffers(1, &accum_fbo_);
    glGenTextures(1, &accum_texture_);
    gl_initialized_ = true;
    ensure_accum_target();

    unsigned int vs = compile_stage(GL_VERTEX_SHADER, kTeslaVertexSource, "tesla vertex");
    if (vs == 0) {
        std::cout << "[TESLA] GPU backend unavailable (vertex stage); using the CPU backend." << std::endl;
        return;
    }
    unsigned int fs = compile_stage(GL_FRAGMENT_SHADER, kTeslaFragmentSource, "tesla fragment");
    if (fs == 0) {
        glDeleteShader(vs);
        std::cout << "[TESLA] GPU backend unavailable (fragment stage); using the CPU backend." << std::endl;
        return;
    }

    trace_program_ = glCreateProgram();
    glAttachShader(trace_program_, vs);
    glAttachShader(trace_program_, fs);
    glLinkProgram(trace_program_);

    int linked = 0;
    glGetProgramiv(trace_program_, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(trace_program_, sizeof(log), nullptr, log);
        std::cerr << "[TESLA] program link failed:\n" << log << std::endl;
        glDeleteProgram(trace_program_);
        trace_program_ = 0;
        return;
    }

    // Fullscreen triangle pair.
    const float quad[] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, 1.0f,
    };
    glGenVertexArrays(1, &quad_vao_);
    glGenBuffers(1, &quad_vbo_);
    glBindVertexArray(quad_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    glGenBuffers(1, &tri_buffer_);  glGenTextures(1, &tri_tex_);
    glGenBuffers(1, &bvh_buffer_);  glGenTextures(1, &bvh_tex_);
    glGenBuffers(1, &idx_buffer_);  glGenTextures(1, &idx_tex_);
    glGenBuffers(1, &mat_buffer_);  glGenTextures(1, &mat_tex_);
    glGenBuffers(1, &lgt_buffer_);  glGenTextures(1, &lgt_tex_);
    glGenBuffers(1, &emt_buffer_);  glGenTextures(1, &emt_tex_);

    // Directional-albedo table, identical to the one the CPU backend builds.
    {
        const AlbedoLUT& lut = albedo_lut();
        std::vector<float> rg(kAlbedoLutSize * kAlbedoLutSize * 2);
        for (int i = 0; i < kAlbedoLutSize * kAlbedoLutSize; ++i) {
            rg[i * 2 + 0] = lut.A[i];
            rg[i * 2 + 1] = lut.B[i];
        }
        glGenTextures(1, &albedo_lut_tex_);
        glBindTexture(GL_TEXTURE_2D, albedo_lut_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, kAlbedoLutSize, kAlbedoLutSize, 0, GL_RG, GL_FLOAT, rg.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    gpu_ready_ = true;
    if (!triangles_.empty()) upload_scene_to_gpu();

    std::cout << "[TESLA] GPU backend ready." << std::endl;
}

void TeslaRenderer::shutdown_gpu() {
    if (!gl_initialized_) return;

    glDeleteProgram(trace_program_);
    trace_program_ = 0;

    unsigned int buffers[] = { tri_buffer_, bvh_buffer_, idx_buffer_, mat_buffer_, lgt_buffer_, emt_buffer_, quad_vbo_ };
    glDeleteBuffers(7, buffers);

    unsigned int textures[] = { tri_tex_, bvh_tex_, idx_tex_, mat_tex_, lgt_tex_, emt_tex_,
                                accum_texture_, albedo_lut_tex_ };
    glDeleteTextures(8, textures);

    glDeleteTextures(1, &texture_array_);
    texture_array_ = 0;
    glDeleteVertexArrays(1, &quad_vao_);
    glDeleteFramebuffers(1, &accum_fbo_);

    tri_buffer_ = bvh_buffer_ = idx_buffer_ = mat_buffer_ = lgt_buffer_ = emt_buffer_ = quad_vbo_ = 0;
    tri_tex_ = bvh_tex_ = idx_tex_ = mat_tex_ = lgt_tex_ = emt_tex_ = accum_texture_ = 0;
    albedo_lut_tex_ = 0;
    quad_vao_ = accum_fbo_ = 0;

    gl_initialized_ = false;
    gpu_ready_ = false;
    gpu_uploaded_ = false;
}

void TeslaRenderer::ensure_accum_target() {
    if (!gl_initialized_ || width_ <= 0 || height_ <= 0) return;

    glBindTexture(GL_TEXTURE_2D, accum_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, accum_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, accum_texture_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void TeslaRenderer::upload_scene_to_gpu() {
    if (!gpu_ready_) return;

    auto upload_float = [](unsigned int buffer, unsigned int tex, const std::vector<float>& data) {
        glBindBuffer(GL_TEXTURE_BUFFER, buffer);
        glBufferData(GL_TEXTURE_BUFFER, data.empty() ? 16 : data.size() * sizeof(float),
                     data.empty() ? nullptr : data.data(), GL_STATIC_DRAW);
        glBindTexture(GL_TEXTURE_BUFFER, tex);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buffer);
    };

    // Triangles: 7 RGBA texels each, UVs riding in the spare .w lanes.
    {
        std::vector<float> data(triangles_.size() * 28);
        for (size_t i = 0; i < triangles_.size(); ++i) {
            const TeslaTriangle& t = triangles_[i];
            float* d = &data[i * 28];
            d[0]  = t.v0.x; d[1]  = t.v0.y; d[2]  = t.v0.z; d[3]  = static_cast<float>(t.material);
            d[4]  = t.v1.x; d[5]  = t.v1.y; d[6]  = t.v1.z; d[7]  = t.uv0.x;
            d[8]  = t.v2.x; d[9]  = t.v2.y; d[10] = t.v2.z; d[11] = t.uv0.y;
            d[12] = t.n0.x; d[13] = t.n0.y; d[14] = t.n0.z; d[15] = t.uv1.x;
            d[16] = t.n1.x; d[17] = t.n1.y; d[18] = t.n1.z; d[19] = t.uv1.y;
            d[20] = t.n2.x; d[21] = t.n2.y; d[22] = t.n2.z; d[23] = t.uv2.x;
            d[24] = t.uv2.y; d[25] = 0.0f;  d[26] = 0.0f;   d[27] = 0.0f;
        }
        upload_float(tri_buffer_, tri_tex_, data);
    }

    // BVH nodes: 2 RGBA texels each, ints reinterpreted as floats.
    {
        const auto& nodes = bvh_.nodes();
        std::vector<float> data(nodes.size() * 8);
        for (size_t i = 0; i < nodes.size(); ++i) {
            const TeslaBVHNode& n = nodes[i];
            float* d = &data[i * 8];
            float lf, ct;
            std::memcpy(&lf, &n.left_first, sizeof(float));
            std::memcpy(&ct, &n.count, sizeof(float));
            d[0] = n.bmin.x; d[1] = n.bmin.y; d[2] = n.bmin.z; d[3] = lf;
            d[4] = n.bmax.x; d[5] = n.bmax.y; d[6] = n.bmax.z; d[7] = ct;
        }
        upload_float(bvh_buffer_, bvh_tex_, data);
    }

    // Primitive order: R32I.
    {
        const auto& idx = bvh_.indices();
        glBindBuffer(GL_TEXTURE_BUFFER, idx_buffer_);
        glBufferData(GL_TEXTURE_BUFFER, idx.empty() ? 4 : idx.size() * sizeof(int32_t),
                     idx.empty() ? nullptr : idx.data(), GL_STATIC_DRAW);
        glBindTexture(GL_TEXTURE_BUFFER, idx_tex_);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32I, idx_buffer_);
    }

    // Materials: 3 RGBA texels each.
    {
        std::vector<float> data(materials_.size() * 12);
        for (size_t i = 0; i < materials_.size(); ++i) {
            const TeslaMaterial& m = materials_[i];
            float* d = &data[i * 12];
            d[0] = m.base_color.x; d[1] = m.base_color.y; d[2] = m.base_color.z; d[3] = m.metallic;
            d[4] = m.emission.x;   d[5] = m.emission.y;   d[6] = m.emission.z;   d[7] = m.roughness;
            d[8] = static_cast<float>(m.albedo_texture);
            d[9] = 0.0f; d[10] = 0.0f; d[11] = 0.0f;
        }
        upload_float(mat_buffer_, mat_tex_, data);
    }

    // Lights: 6 RGBA texels each.
    {
        std::vector<float> data(lights_.size() * 24);
        for (size_t i = 0; i < lights_.size(); ++i) {
            const TeslaLight& l = lights_[i];
            float* d = &data[i * 24];
            d[0]  = l.color.x;    d[1]  = l.color.y;    d[2]  = l.color.z;    d[3]  = l.intensity;
            d[4]  = l.position.x; d[5]  = l.position.y; d[6]  = l.position.z; d[7]  = l.radius;
            d[8]  = l.direction.x;d[9]  = l.direction.y;d[10] = l.direction.z;d[11] = l.angular_radius;
            d[12] = l.cos_inner;  d[13] = l.cos_outer;  d[14] = static_cast<float>(static_cast<int>(l.type)); d[15] = 0.0f;
            d[16] = l.u_axis.x;   d[17] = l.u_axis.y;   d[18] = l.u_axis.z;   d[19] = 0.0f;
            d[20] = l.v_axis.x;   d[21] = l.v_axis.y;   d[22] = l.v_axis.z;   d[23] = 0.0f;
        }
        upload_float(lgt_buffer_, lgt_tex_, data);
    }

    // Emitters: (primIndex bits, cdf, area, 0).
    {
        std::vector<float> data(emitters_.size() * 4);
        for (size_t i = 0; i < emitters_.size(); ++i) {
            float pf;
            int32_t p = emitters_[i];
            std::memcpy(&pf, &p, sizeof(float));
            const TeslaTriangle& t = triangles_[p];
            float area = 0.5f * Vector3::cross(t.v1 - t.v0, t.v2 - t.v0).length();
            data[i * 4 + 0] = pf;
            data[i * 4 + 1] = emitter_cdf_[i];
            data[i * 4 + 2] = area;
            data[i * 4 + 3] = 0.0f;
        }
        upload_float(emt_buffer_, emt_tex_, data);
    }

    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    gpu_uploaded_ = true;
}

void TeslaRenderer::upload_textures_to_gpu() {
    if (!gpu_ready_) return;
    if (!textures_dirty_ && texture_array_ != 0) return;

    if (texture_array_ == 0) glGenTextures(1, &texture_array_);

    glBindTexture(GL_TEXTURE_2D_ARRAY, texture_array_);

    // At least one layer must exist even with no textures: sampling an incomplete
    // array texture is undefined, and the shader still evaluates the sampler.
    const int layers = std::max<int>(1, static_cast<int>(textures_.size()));
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kTextureSize, kTextureSize, layers,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    for (size_t i = 0; i < textures_.size(); ++i) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<int>(i),
                        kTextureSize, kTextureSize, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, textures_[i].data());
    }

    // No mip chain by design - see sample_texture_rgba8. GL_REPEAT is what makes the
    // bilinear footprint wrap correctly across a tile boundary; the fract() in the
    // shader only keeps large UVs in range before that.
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    textures_dirty_ = false;
}

void TeslaRenderer::step_gpu() {
    if (!gpu_ready_ || trace_program_ == 0) return;
    if (!gpu_uploaded_) upload_scene_to_gpu();
    if (textures_dirty_) upload_textures_to_gpu();

    int prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, accum_fbo_);
    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // rgb accumulates radiance, alpha accumulates the sample count.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    glUseProgram(trace_program_);

    auto set_int   = [&](const char* n, int v)   { glUniform1i(glGetUniformLocation(trace_program_, n), v); };
    auto set_float = [&](const char* n, float v) { glUniform1f(glGetUniformLocation(trace_program_, n), v); };
    auto set_vec3  = [&](const char* n, const Vector3& v) { glUniform3f(glGetUniformLocation(trace_program_, n), v.x, v.y, v.z); };

    glUniformMatrix4fv(glGetUniformLocation(trace_program_, "uInvView"), 1, GL_FALSE, inv_view_.m.data());
    glUniformMatrix4fv(glGetUniformLocation(trace_program_, "uInvProj"), 1, GL_FALSE, inv_proj_.m.data());
    set_vec3("uCameraPos", camera_position_);

    set_int("uLightCount", static_cast<int>(lights_.size()));
    set_int("uEmitterCount", static_cast<int>(emitters_.size()));
    set_float("uEmitterAreaTotal", emitter_area_total_);
    set_int("uHasBVH", bvh_.empty() ? 0 : 1);

    set_vec3("uSkyZenith", sky_.zenith);
    set_vec3("uSkyHorizon", sky_.horizon);
    set_vec3("uSkyGround", sky_.ground);
    set_float("uSkyIntensity", sky_.intensity);
    set_int("uSkyEnabled", sky_.enabled ? 1 : 0);

    set_int("uSampleIndex", samples_done_);
    glUniform2f(glGetUniformLocation(trace_program_, "uResolution"),
                static_cast<float>(width_), static_cast<float>(height_));
    set_int("uMaxDepth", settings_.max_depth);
    set_int("uRRStartDepth", settings_.rr_start_depth);
    set_float("uRRMaxSurvival", settings_.rr_max_survival);
    set_float("uFireflyClamp", settings_.firefly_clamp);

    struct { const char* name; unsigned int tex; int unit; } binds[] = {
        { "uTriangles", tri_tex_, 0 },
        { "uNodes",     bvh_tex_, 1 },
        { "uIndices",   idx_tex_, 2 },
        { "uMaterials", mat_tex_, 3 },
        { "uLights",    lgt_tex_, 4 },
        { "uEmitters",  emt_tex_, 5 },
    };
    for (const auto& b : binds) {
        glActiveTexture(GL_TEXTURE0 + b.unit);
        glBindTexture(GL_TEXTURE_BUFFER, b.tex);
        glUniform1i(glGetUniformLocation(trace_program_, b.name), b.unit);
    }

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, albedo_lut_tex_);
    glUniform1i(glGetUniformLocation(trace_program_, "uAlbedoLUT"), 6);

    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, sky_.env_texture);
    glUniform1i(glGetUniformLocation(trace_program_, "uEnvMap"), 7);
    set_int("uHasEnvMap", sky_.env_texture != 0 ? 1 : 0);

    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture_array_);
    glUniform1i(glGetUniformLocation(trace_program_, "uTextures"), 8);

    glBindVertexArray(quad_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prev_fbo));
    glEnable(GL_DEPTH_TEST);

    ++samples_done_;
}
