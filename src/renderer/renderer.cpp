#include "renderer/renderer.hpp"
#include "world/terrain_component.hpp"
#include "core/mesh_resource.hpp"
#include "core/resource_manager.hpp"
#include "renderer/shader_sources.hpp"
#include "renderer/material_shader.hpp"
#include "renderer/lightmapper.hpp"
#include "core/texture_resource.hpp"
#include "renderer/gl_loader.hpp"
#include "core/mesh_resource.hpp"
#include "core/texture_resource.hpp"
#include "stb_image.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include "world/particle_emitter_component.hpp"
#include <vector>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <random>

// Define global OpenGL function pointers
PFNGLCREATESHADERPROC glCreateShader = nullptr;
PFNGLGENERATEMIPMAPPROC glGenerateMipmap = nullptr;
PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
PFNGLATTACHSHADERPROC glAttachShader = nullptr;
PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
PFNGLDELETESHADERPROC glDeleteShader = nullptr;
PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;

PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;

PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
PFNGLGENQUERIESPROC_LZ glGenQueries = nullptr;
PFNGLDELETEQUERIESPROC_LZ glDeleteQueries = nullptr;
PFNGLBEGINQUERYPROC_LZ glBeginQuery = nullptr;
PFNGLENDQUERYPROC_LZ glEndQuery = nullptr;
PFNGLGETQUERYOBJECTUIVPROC_LZ glGetQueryObjectuiv = nullptr;
PFNGLGETQUERYOBJECTUI64VPROC_LZ glGetQueryObjectui64v = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC glBufferData = nullptr;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;

PFNGLMULTIDRAWELEMENTSPROC glMultiDrawElements = nullptr;

PFNGLUNIFORM1UIPROC glUniform1ui = nullptr;
PFNGLBINDBUFFERBASEPROC glBindBufferBase = nullptr;
PFNGLDISPATCHCOMPUTEPROC glDispatchCompute = nullptr;
PFNGLMEMORYBARRIERPROC glMemoryBarrier = nullptr;
PFNGLMULTIDRAWELEMENTSINDIRECTPROC glMultiDrawElementsIndirect = nullptr;

PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = nullptr;
PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer = nullptr;
PFNGLUNIFORM4FPROC glUniform4f = nullptr;
PFNGLBUFFERSUBDATAPROC glBufferSubData = nullptr;
PFNGLVERTEXATTRIBDIVISORPROC glVertexAttribDivisor = nullptr;
PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced = nullptr;
PFNGLDRAWARRAYSINSTANCEDPROC glDrawArraysInstanced = nullptr;

PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv = nullptr;
PFNGLUNIFORM3FVPROC glUniform3fv = nullptr;
PFNGLUNIFORM1IPROC glUniform1i = nullptr;
PFNGLUNIFORM1FPROC glUniform1f = nullptr;
PFNGLUNIFORM4FVPROC glUniform4fv = nullptr;
PFNGLTEXBUFFERPROC glTexBuffer = nullptr;
PFNGLUNIFORM3FPROC glUniform3f = nullptr;

#ifdef _WIN32
PFNGLACTIVETEXTURELITHPROC lithium_glActiveTexture = nullptr;
PFNGLDRAWBUFFERSLITHPROC   lithium_glDrawBuffers = nullptr;
PFNGLTEXIMAGE3DLITHPROC    lithium_glTexImage3D = nullptr;
PFNGLTEXSUBIMAGE3DLITHPROC lithium_glTexSubImage3D = nullptr;
#endif
PFNGLGETPROGRAMBINARYPROC glGetProgramBinary = nullptr;
PFNGLPROGRAMBINARYPROC   glProgramBinary = nullptr;
PFNGLPROGRAMPARAMETERIPROC glProgramParameteri = nullptr;
PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate = nullptr;
PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = nullptr;
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = nullptr;

PFNGLTEXIMAGE2DMULTISAMPLEPROC glTexImage2DMultisample = nullptr;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC glRenderbufferStorageMultisample = nullptr;
PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = nullptr;
PFNGLUNIFORM2FPROC glUniform2f = nullptr;

namespace {

// Most of the post-process/utility programs below are built inline rather than
// through Renderer::compile_shaders, and historically none of them checked
// compile or link status. A broken shader therefore left an unlinked program
// handle in place; glUseProgram on it is a no-op that raises GL_INVALID_OPERATION
// and silently leaves whatever program was bound previously active, so the pass
// renders with the wrong shader instead of failing loudly. These report it.
bool report_shader_compile(unsigned int shader, const char* name, const char* stage) {
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        std::cerr << "[Shader] " << name << " (" << stage << ") COMPILE FAILED:\n" << log << std::endl;
    }
    return ok != 0;
}

bool report_program_link(unsigned int program, const char* name) {
    int ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        std::cerr << "[Shader] " << name << " LINK FAILED:\n" << log << std::endl;
    }
    return ok != 0;
}

} // namespace

namespace ShaderSources {

const std::string& geometry_vertex() {
    static const std::string source = R"(
        #version 450 core
        layout (location = 0) in vec3 aPos;
        layout (location = 1) in vec3 aColor;
        layout (location = 2) in vec3 aNormal;
        layout (location = 3) in vec2 aUV;
        // Skinning stream. Static meshes leave these attributes disabled, which
        // supplies a constant (0,0,0,0) - and a zero weight sum is exactly the
        // "leave this vertex alone" case handled below, so one program serves both.
        layout (location = 4) in ivec4 aBoneIDs;
        layout (location = 5) in vec4 aWeights;
        // Second uv set, unique per actor rather than per mesh asset: two actors
        // sharing a mesh occupy different regions of the lightmap atlas. Supplied by
        // a per-component buffer, and a constant (0,0) when there is none.
        layout (location = 10) in vec2 aLightmapUV;

        out vec3 FragPos;
        out vec3 Normal;
        out vec3 ourColor;
        out vec2 TexCoord;
        out vec2 LightmapUV;
        out vec4 FragPosLightSpace;

        uniform mat4 uMVP;
        uniform mat4 uModel;
        uniform mat4 uLightSpaceMatrix;

        const int MAX_BONES = 128;
        uniform bool uSkinned;
        uniform mat4 uBones[MAX_BONES];

        void main() {
            vec3 position = aPos;
            vec3 normal = aNormal;

            if (uSkinned) {
                float total = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
                // The static props inside a rigged model carry no influences.
                // Skinning them by a zero matrix would collapse them onto the origin.
                if (total > 0.0) {
                    mat4 skin = mat4(0.0);
                    for (int i = 0; i < 4; ++i) {
                        int index = clamp(aBoneIDs[i], 0, MAX_BONES - 1);
                        skin += uBones[index] * aWeights[i];
                    }
                    // Weights are normalised at import, but dividing here keeps a
                    // mesh from an exporter that did not normalise from inflating.
                    skin /= total;
                    position = (skin * vec4(aPos, 1.0)).xyz;
                    // mat3(skin) is not the correct normal matrix under non-uniform
                    // bone scale, but rigs overwhelmingly use rigid bones, and the
                    // per-vertex inverse the exact form needs is far too expensive.
                    normal = mat3(skin) * aNormal;
                }
            }

            gl_Position = uMVP * vec4(position, 1.0);
            FragPos = vec3(uModel * vec4(position, 1.0));
            Normal = mat3(transpose(inverse(uModel))) * normal;
            ourColor = aColor;
            TexCoord = aUV;
            LightmapUV = aLightmapUV;
            FragPosLightSpace = uLightSpaceMatrix * vec4(FragPos, 1.0);
        }
    )";
    return source;
}

} // namespace ShaderSources

Renderer::Renderer() : view_matrix(Matrix4x4::identity()), projection_matrix(Matrix4x4::identity()) {}

Renderer::~Renderer() {
    if (geometry_shader_program) {
        glDeleteProgram(geometry_shader_program);
    }
    if (lighting_shader_program) {
        glDeleteProgram(lighting_shader_program);
    }
    destroy_fbo();
}

bool Renderer::initialize(int width, int height) {
    if (!load_gl_functions()) {
        std::cerr << "Failed to load modern OpenGL functions!" << std::endl;
        return false;
    }

    // Log what we actually got. Several features here (compute shaders, SSBOs)
    // depend on the real context version and driver, and silently degrade otherwise.
    const GLubyte* gl_renderer = glGetString(GL_RENDERER);
    const GLubyte* gl_version = glGetString(GL_VERSION);
    std::cout << "[Renderer] GL " << (gl_version ? reinterpret_cast<const char*>(gl_version) : "?")
              << " on " << (gl_renderer ? reinterpret_cast<const char*>(gl_renderer) : "?") << std::endl;

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);


    const std::string& geometry_vertex_shader_source = ShaderSources::geometry_vertex();

    const std::string geometry_fragment_shader_source = R"(
        #version 450 core
        layout (location = 0) out vec4 gPosition;
        layout (location = 1) out vec4 gNormal;
        layout (location = 2) out vec4 gAlbedoSpec;
        layout (location = 3) out vec4 gPBR;
        layout (location = 4) out vec4 gBakedGI;

        in vec3 FragPos;
        in vec3 Normal;
        in vec3 ourColor;
        in vec2 TexCoord;
        in vec2 LightmapUV;
        in vec4 FragPosLightSpace;

        uniform vec4 uColorOverride;
        uniform float uMetallic;
        uniform float uRoughness;
        uniform bool uEnableUE4Lighting;
        uniform sampler2D uDiffuseTexture;
        uniform bool uHasDiffuseTexture;
        uniform float uClearcoat;
        uniform float uClearcoatRoughness;
        uniform float uSheen;
        uniform float uSubsurface;
        uniform float uEmissive;
        uniform sampler2D uLightmap;
        uniform bool uHasLightmap;
        // Six directional colours: +x, -x, +y, -y, +z, -z.
        uniform vec3 uAmbientCube[6];

        void main() {
            vec3 objColor = (uColorOverride.a > 0.0) ? uColorOverride.rgb : ourColor;
            if (uHasDiffuseTexture) {
                objColor *= texture(uDiffuseTexture, TexCoord).rgb;
            }
            vec3 albedo = uEnableUE4Lighting ? pow(objColor, vec3(2.2)) : objColor;
            
            // Procedural Grid for Floor
            vec2 coord = FragPos.xz * 1.0;
            vec2 coord10 = FragPos.xz * 0.1;
            vec2 fw_coord = fwidth(coord);
            vec2 fw_coord10 = fwidth(coord10);

            if (abs(normalize(Normal).y) > 0.99 && abs(FragPos.y + 1.0) < 0.05) {
                vec2 safe_fw = max(fw_coord, vec2(0.0001));
                vec2 grid = abs(fract(coord - 0.5) - 0.5) / safe_fw;
                float line = min(grid.x, grid.y);
                float gridWeight = 1.0 - min(line, 1.0);
                
                vec2 safe_fw10 = max(fw_coord10, vec2(0.0001));
                vec2 grid10 = abs(fract(coord10 - 0.5) - 0.5) / safe_fw10;
                float line10 = min(grid10.x, grid10.y);
                float gridWeight10 = 1.0 - min(line10, 1.0);

                albedo = mix(albedo, vec3(0.05), gridWeight * 0.5);
                albedo = mix(albedo, vec3(0.02), gridWeight10 * 0.8);
            }
            
            // Alpha carries emissive: the lighting pass recomputes light-space depth
            // from uLightSpaceMatrix anyway, so the channel was going unread.
            gPosition = vec4(FragPos, uEmissive);
            gNormal = vec4(normalize(Normal), uSubsurface);
            gAlbedoSpec = vec4(albedo, uSheen);
            gPBR = vec4(uMetallic, max(uRoughness, 0.1), uClearcoat, max(uClearcoatRoughness, 0.05));

            // Baked indirect. A static surface reads it from the lightmap atlas; a
            // dynamic one gets the ambient cube the engine sampled from the probe
            // grid at this object's position. Both end up here so the lighting pass
            // does not have to care which kind of object it is shading.
            vec3 baked = vec3(0.0);
            if (uHasLightmap) {
                baked = texture(uLightmap, LightmapUV).rgb;
            } else {
                vec3 n = normalize(Normal);
                vec3 sq = n * n;
                baked  = (n.x >= 0.0 ? uAmbientCube[0] : uAmbientCube[1]) * sq.x;
                baked += (n.y >= 0.0 ? uAmbientCube[2] : uAmbientCube[3]) * sq.y;
                baked += (n.z >= 0.0 ? uAmbientCube[4] : uAmbientCube[5]) * sq.z;
            }
            gBakedGI = vec4(baked, 1.0);
        }
    )";

    geometry_shader_program = compile_shaders(geometry_vertex_shader_source, geometry_fragment_shader_source);
    report_loading_progress("Compiling geometry shaders");
    
    const std::string lighting_vertex_shader_source = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";

    const std::string lighting_fragment_shader_source = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;

        uniform sampler2D gPosition;
        uniform sampler2D gNormal;
        uniform sampler2D gAlbedoSpec;
        uniform sampler2D gPBR;
        uniform sampler2D gBakedGI;
        // Ambient occlusion from render_ssao(). This is the *previous* frame's
        // result: the SSGI half of that pass samples the lit scene colour, so it
        // cannot run before lighting, and AO is low-frequency and temporally stable
        // enough that a frame of lag is not visible. The renderer already makes the
        // same trade for occlusion queries.
        uniform sampler2D uSSAOMap;
        // False until render_ssao() has produced a result, so the first frame after
        // a resize does not multiply ambient by an empty buffer and go black.
        uniform bool uHasSSAO;
        uniform float uSSAOStrength;
        uniform sampler2D shadowMap;
        uniform sampler2D uEnvironmentMap;
        uniform sampler2D uIrradianceMap;
        uniform sampler2D uPrefilteredEnv;
        uniform bool uHasEnvironmentMap;
        uniform float uEnvironmentMaxLod;
        uniform float uPrefilteredMaxLod;

        struct Light {
            int type; // 0=Directional, 1=Point, 2=Spot, 3=Area, 4=Sky
            vec3 position;
            vec3 direction;
            vec3 color;
            float intensity;
            float radius;
            float innerCutOff;
            float outerCutOff;
        };

        uniform Light uLights[8];
        uniform int uNumLights;
        uniform vec3 uCameraPos; // For specular highlights
        uniform bool uEnableUE4Lighting;
        uniform bool uEnableRayTracing;
        uniform mat4 uLightSpaceMatrix;
        uniform float uShadowTexelWorldSize;  // world units covered by one shadow texel
        uniform float uShadowDepthRange;      // world units spanned by the [0,1] depth range
        uniform vec3 uCameraWorldPos;   // absolute camera position (height fog only)
        uniform float uFogDensity;
        uniform float uFogHeight;
        uniform float uFogHeightFalloff;
        // G-buffer / lighting debug views, selected with LITHIUM_DEBUG_LIGHTING.
        // 0 = off. See the lighting pass in unbind_fbo for the list.
        uniform int uLightDebug;

        const float PI = 3.14159265359;

        // Percentage-Closer Soft Shadows. A fixed-radius PCF blur gives every shadow
        // the same softness everywhere, which reads as a flat decal stuck to the
        // ground. PCSS instead estimates how far the occluder is from the receiver and
        // widens the filter with that distance, so contact points stay sharp and the
        // penumbra spreads out with height - the single strongest cue that an object
        // is actually resting on the surface.
        // tan of the sun's apparent angular radius. The real sun is ~0.27 degrees
        // (tan ~0.005), which is almost perfectly sharp; this is deliberately widened
        // for a softer, more filmic penumbra.
        const float kSunSoftness = 0.05;
        const int kBlockerSamples = 16;
        const int kPcfSamples = 32;

        // Interleaved gradient noise: rotates the sample disk per pixel so the finite
        // tap count dissolves into fine dither instead of visible banding rings.
        float shadowNoise(vec2 p) {
            return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
        }

        vec2 spiralTap(int i, int count, float cosR, float sinR) {
            float theta = float(i) * 2.39996323;
            float r = sqrt((float(i) + 0.5) / float(count));
            vec2 d = vec2(cos(theta), sin(theta)) * r;
            return vec2(d.x * cosR - d.y * sinR, d.x * sinR + d.y * cosR);
        }

        float ShadowCalculation(vec4 fragPosLightSpace, vec3 lightDir, vec3 normal) {
            vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
            projCoords = projCoords * 0.5 + 0.5;
            if (projCoords.z > 1.0) return 0.0;
            // Outside the shadow map entirely: treat as lit rather than sampling the
            // clamped border, which used to darken everything past the map's edge.
            if (any(lessThan(projCoords.xy, vec2(0.0))) || any(greaterThan(projCoords.xy, vec2(1.0)))) return 0.0;

            float currentDepth = projCoords.z;
            float NdotL = max(dot(normal, lightDir), 0.0);
            // Slope-scaled depth bias; small, because PCSS keeps contact regions tight
            // and an oversized bias is what makes shadows detach from their caster.
            float bias = max(0.0015 * (1.0 - NdotL), 0.0003);

            vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
            float rot = shadowNoise(gl_FragCoord.xy) * 6.2831853;
            float cosR = cos(rot), sinR = sin(rot);

            // 1. Blocker search: how deep, on average, are the occluders?
            float blockerSum = 0.0;
            int blockerCount = 0;
            float searchRadius = 8.0;
            for (int i = 0; i < kBlockerSamples; ++i) {
                vec2 offset = spiralTap(i, kBlockerSamples, cosR, sinR) * texelSize * searchRadius;
                float d = texture(shadowMap, projCoords.xy + offset).r;
                if (d < currentDepth - bias) {
                    blockerSum += d;
                    ++blockerCount;
                }
            }
            if (blockerCount == 0) return 0.0;   // fully lit, skip the expensive filter
            float avgBlockerDepth = blockerSum / float(blockerCount);

            // 2. Penumbra estimate. A directional light projects orthographically, so
            // penumbra width grows linearly with the receiver/blocker separation -
            // there is no perspective divide by blocker depth here (that form belongs
            // to spot/point lights and, used with an ortho map, made the softness
            // depend on where the object sat in the depth range rather than on how far
            // it floated above its occluder).
            float separationWorld = (currentDepth - avgBlockerDepth) * uShadowDepthRange;
            float penumbraWorld = separationWorld * kSunSoftness;
            float filterRadius = clamp(penumbraWorld / max(uShadowTexelWorldSize, 0.0001), 1.0, 24.0);

            // 3. PCF over the estimated penumbra.
            float shadow = 0.0;
            for (int i = 0; i < kPcfSamples; ++i) {
                vec2 offset = spiralTap(i, kPcfSamples, cosR, sinR) * texelSize * filterRadius;
                float pcfDepth = texture(shadowMap, projCoords.xy + offset).r;
                shadow += (currentDepth - bias > pcfDepth) ? 1.0 : 0.0;
            }
            return shadow / float(kPcfSamples);
        }

        float DistributionGGX(vec3 N, vec3 H, float roughness) {
            float a = roughness*roughness;
            float a2 = a*a;
            float NdotH = max(dot(N, H), 0.0);
            float NdotH2 = NdotH*NdotH;
            float num = a2;
            float denom = (NdotH2 * (a2 - 1.0) + 1.0);
            denom = PI * denom * denom;
            return num / denom;
        }
        
        float GeometrySchlickGGX(float NdotV, float roughness) {
            float r = (roughness + 1.0);
            float k = (r*r) / 8.0;
            float num = NdotV;
            float denom = NdotV * (1.0 - k) + k;
            return num / denom;
        }
        
        float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
            float NdotV = max(dot(N, V), 0.0);
            float NdotL = max(dot(N, L), 0.0);
            float ggx2 = GeometrySchlickGGX(NdotV, roughness);
            float ggx1 = GeometrySchlickGGX(NdotL, roughness);
            return ggx1 * ggx2;
        }
        
        vec3 fresnelSchlick(float cosTheta, vec3 F0) {
            return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
        }

        // Roughness-aware Fresnel (Fresnel-Schlick-Roughness, Karis/Epic "Real Shading
        // in Unreal Engine 4"). Unlike the plain Schlick term above, this clamps the
        // grazing-angle response by (1-roughness) so rough ambient reflections don't
        // over-brighten at silhouettes the way a naive Fresnel does.
        vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
            return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
        }

        // Analytic sky approximation used for ambient/IBL. Shares the same
        // zenith/horizon/haze gradient as the real skybox (render_skybox's sky_frag)
        // so ambient lighting on objects visually matches the sky behind them instead
        // of a disconnected flat color, and adds Rayleigh-style horizon warming plus a
        // night falloff so ambient responds to the sun's actual elevation.
        vec3 sampleAnalyticSky(vec3 dir, vec3 sunDir) {
            vec3 zenith = vec3(0.1, 0.25, 0.8) * 1.2;
            vec3 horizon = vec3(0.45, 0.6, 0.9) * 1.1;
            vec3 sky = mix(horizon, zenith, clamp(abs(dir.y), 0.0, 1.0));

            float haze = pow(1.0 - clamp(abs(dir.y), 0.0, 1.0), 3.0);
            sky = mix(sky, vec3(0.7, 0.75, 0.8) * 1.2, haze * 0.4);

            // Warm dusk/dawn glow that grows as the sun sinks toward the horizon,
            // concentrated in directions near the sun (Rayleigh/Mie-ish approximation).
            float sunElevation = clamp(sunDir.y, -1.0, 1.0);
            float twilight = 1.0 - smoothstep(0.0, 0.35, sunElevation);
            float sunProximity = pow(max(dot(dir, sunDir), 0.0), 4.0);
            vec3 duskColor = vec3(1.0, 0.45, 0.2);
            sky = mix(sky, duskColor, twilight * sunProximity * 0.6);

            // Fade to a dim night sky once the sun is well below the horizon.
            float nightFactor = clamp(-sunElevation * 2.5, 0.0, 1.0);
            sky = mix(sky, sky * 0.05 + vec3(0.02, 0.03, 0.06), nightFactor);

            return sky;
        }

        // Maps a world-space direction to equirectangular UVs (standard lat-long layout).
        // atan(y, x) is degenerate when both arguments are exactly 0 (dir pointing
        // straight up/down, e.g. a flat floor's normal) - GLSL leaves atan(0,0)
        // implementation-defined, which produced garbage/NaN UVs and showed up as
        // a solid, wrong-colored fill on any flat horizontal surface. Biasing x
        // away from exactly 0 keeps the longitude well-defined at the poles.
        vec2 directionToEquirectUV(vec3 dir) {
            vec2 uv = vec2(atan(dir.z, dir.x + 1e-5), asin(clamp(dir.y, -1.0, 1.0)));
            uv *= vec2(0.1591549, 0.3183099); // 1/(2*PI), 1/PI
            uv += 0.5;
            return uv;
        }

        // Diffuse irradiance: sampled from the cosine-convolved irradiance map built
        // on the CPU at load time (see Renderer::build_irradiance_map). Previously
        // this sampled the environment map's coarsest mip, which is a single texel -
        // that gave every normal an identical ambient colour, and it also inherited
        // any Inf produced by mipmapping an HDRI whose sun exceeds the half-float
        // range, which turned the entire lit scene into NaN.
        vec3 sampleEnvironmentDiffuse(vec3 dir) {
            return texture(uIrradianceMap, directionToEquirectUV(dir)).rgb;
        }

        // Specular IBL approximation: LOD scales with roughness so mirror-smooth
        // surfaces show a crisp reflection and rough surfaces show a blurred one.
        // This is a mip-based stand-in for a proper GGX-prefiltered cubemap chain.
        //
        // The result is clamped because an HDRI's sun is a handful of texels carrying
        // thousands of units of radiance. Without a real prefilter those texels stay
        // point-like at low roughness, so a reflection either hits the sun or misses
        // it entirely between neighbouring pixels - sparkling fireflies, which the
        // bloom threshold then smears into visible crosses. Clamping also avoids
        // double-counting the sun, which the scene already has as an explicit
        // directional light with its own analytic specular highlight.
        vec3 sampleEnvironmentSpecular(vec3 dir, float roughness) {
            // Mip index maps linearly to the roughness each level was convolved for
            // (see Renderer::build_prefiltered_specular), so this is a real GGX
            // prefiltered lookup rather than a box-filtered mip standing in for one.
            float lod = clamp(roughness, 0.0, 1.0) * uPrefilteredMaxLod;
            return textureLod(uPrefilteredEnv, directionToEquirectUV(dir), lod).rgb;
        }

        // Split-sum environment BRDF (Karis' analytic fit to the DFG lookup table).
        // Gives the specular IBL correct energy across roughness and viewing angle -
        // the previous roughness-aware Fresnel alone had no geometry/visibility term,
        // so rough metals came out too bright and grazing angles too hot.
        vec3 envBRDFApprox(vec3 F0, float roughness, float NoV) {
            const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
            const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
            vec4 r = roughness * c0 + c1;
            float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
            vec2 AB = vec2(-1.04, 1.04) * a004 + vec2(r.z, r.w);
            return F0 * AB.x + AB.y;
        }

        // Ambient/IBL dispatcher: prefers the loaded HDRI environment map when present,
        // otherwise falls back to the analytic sky approximation.
        vec3 sampleAmbientDiffuse(vec3 dir, vec3 sunDir) {
            return uHasEnvironmentMap ? sampleEnvironmentDiffuse(dir) : sampleAnalyticSky(dir, sunDir);
        }
        // Specular reflection from the HDRI when one is loaded. The concentric
        // moire this used to show came from the environment mip chain being
        // contaminated by +Inf (an HDRI sun brighter than the half-float limit
        // spreading outward through every mip), not from the mip-based roughness
        // approximation itself; with the source clamped on upload the chain is
        // well behaved. Falls back to the analytic sky with no environment map.
        vec3 sampleAmbientSpecular(vec3 dir, vec3 sunDir, float roughness) {
            return uHasEnvironmentMap ? sampleEnvironmentSpecular(dir, roughness)
                                      : sampleAnalyticSky(dir, sunDir);
        }

        void main() {
            vec4 PositionSample = texture(gPosition, TexCoords);
            vec3 FragPos = PositionSample.rgb;
            float emissive = PositionSample.a;
            vec4 NormalSample = texture(gNormal, TexCoords);
            vec3 Normal = NormalSample.rgb;
            float subsurface = NormalSample.a;
            vec4 AlbedoSample = texture(gAlbedoSpec, TexCoords);
            vec3 albedo = AlbedoSample.rgb;
            float sheen = AlbedoSample.a;
            vec4 pbr = texture(gPBR, TexCoords);

            float metallic = pbr.r;
            float roughness = pbr.g;
            float clearcoat = pbr.b;
            float clearcoatRoughness = pbr.a;

            if (length(Normal) < 0.1) {
                discard;
            }
            
            if (!uEnableUE4Lighting) {
                FragColor = vec4(albedo, 1.0);
                return;
            }

            vec3 N = normalize(Normal);
            vec3 V = normalize(uCameraPos - FragPos);
            
            vec3 F0 = vec3(0.04); 
            F0 = mix(F0, albedo, metallic);
            
            vec3 Lo = vec3(0.0);
            float main_shadow = 0.0;
            for (int i = 0; i < uNumLights; ++i) {
                vec3 L;
                float attenuation = 1.0;
                
                if (uLights[i].type == 0 || uLights[i].type == 4) { // Directional or Sky
                    L = normalize(-uLights[i].direction);
                } else { // Point, Spot, Area
                    L = normalize(uLights[i].position - FragPos);
                    float distance = length(uLights[i].position - FragPos);
                    attenuation = 1.0 / (distance * distance);
                    if (uLights[i].type == 2) { // Spot
                        float theta = dot(L, normalize(-uLights[i].direction));
                        float epsilon = uLights[i].innerCutOff - uLights[i].outerCutOff;
                        float intensity = clamp((theta - uLights[i].outerCutOff) / epsilon, 0.0, 1.0);
                        attenuation *= intensity;
                    }
                }
                
                vec3 H = normalize(V + L);
                vec3 lightColor = uLights[i].color * uLights[i].intensity * attenuation;

                float shadow = 0.0;
                if (uLights[i].type == 0) {
                    vec4 FragPosLightSpace = uLightSpaceMatrix * vec4(FragPos, 1.0);
                    shadow = ShadowCalculation(FragPosLightSpace, L, N);
                    main_shadow = max(main_shadow, shadow);
                }

                // Cook-Torrance BRDF (base layer)
                float NDF = DistributionGGX(N, H, roughness);
                float G   = GeometrySmith(N, V, L, roughness);
                vec3 F    = fresnelSchlick(max(dot(H, V), 0.0), F0);

                vec3 numerator    = NDF * G * F;
                float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
                vec3 specular = numerator / denominator;

                vec3 kS = F;
                vec3 kD = vec3(1.0) - kS;
                kD *= 1.0 - metallic;

                float dotNL = dot(N, L);
                float NdotL = max(dotNL, 0.0);
                // Subsurface wrap-diffuse: lets light "wrap around" the terminator for a
                // soft, translucent look (skin/wax/foliage) instead of a hard cutoff.
                // Reduces to plain NdotL when subsurface is 0.
                float wrapNdotL = clamp((dotNL + subsurface) / (1.0 + subsurface), 0.0, 1.0);

                // Clearcoat: a second, colorless, low-roughness GGX lobe on top of the
                // base layer (Disney/Filament clearcoat model). It also attenuates the
                // base layer's energy by its own Fresnel term so the surface doesn't
                // gain energy from the extra layer.
                float clearcoatF = fresnelSchlick(max(dot(H, V), 0.0), vec3(0.04)).r;
                float clearcoatNDF = DistributionGGX(N, H, clearcoatRoughness);
                float clearcoatG = GeometrySmith(N, V, L, clearcoatRoughness);
                float clearcoatSpec = (clearcoatNDF * clearcoatG * clearcoatF) / denominator;
                float baseAttenuation = 1.0 - clearcoat * clearcoatF;

                // Sheen: a cheap grazing-angle rim highlight for cloth/fabric-like materials.
                float sheenFactor = pow(clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0), 4.0) * sheen;

                vec3 baseLayer = (kD * albedo / PI) * wrapNdotL + specular * NdotL;
                vec3 clearcoatLayer = vec3(clearcoatSpec * clearcoat) * NdotL;
                vec3 sheenLayer = albedo * sheenFactor * NdotL;

                Lo += (baseLayer * baseAttenuation + clearcoatLayer + sheenLayer) * lightColor * (1.0 - shadow);
            }

            // Ambient + Sky-Based IBL Approximation
            // Sun direction for the sky model comes from the first directional light
            // (matches the assumption the volumetric shaft code below already makes).
            vec3 sunDir = (uNumLights > 0 && uLights[0].type == 0) ? normalize(-uLights[0].direction) : vec3(0.0, 1.0, 0.0);
            vec3 groundColor = vec3(0.2, 0.15, 0.1);

            // Diffuse Ambient. With an HDRI loaded this is a real cosine-convolved
            // irradiance lookup already divided by PI, so the Lambertian response is
            // simply irradiance * albedo - no hemisphere blend or fudge factor, and
            // the environment's own dark ground already darkens downward-facing
            // normals. The analytic fallback returns sky *radiance* rather than
            // irradiance, so it keeps the hemisphere approximation.
            vec3 skyAboveNormal = sampleAmbientDiffuse(N, sunDir);
            float hemiMix = 0.5 * N.y + 0.5;
            vec3 diffuseAmbient = uHasEnvironmentMap
                ? skyAboveNormal * albedo
                : mix(groundColor, skyAboveNormal, hemiMix) * albedo * 0.5;

            // Specular Ambient: environment/sky sampled along the reflection vector (with
            // roughness-based blur when an HDRI is loaded), so mirror-like surfaces show
            // a reflection consistent with what's actually rendered, with a roughness-aware
            // Fresnel so grazing reflections don't blow out on rough materials.
            vec3 R = reflect(-V, N);
            vec3 envSpecular = sampleAmbientSpecular(R, sunDir, roughness);
            vec3 reflectColor = uHasEnvironmentMap
                ? envSpecular
                : mix(groundColor, envSpecular, clamp(0.5 * R.y + 0.5, 0.0, 1.0));
            float NoV = max(dot(N, V), 0.0);
            vec3 kS_ambient = uHasEnvironmentMap ? envBRDFApprox(F0, roughness, NoV)
                                                 : fresnelSchlickRoughness(NoV, F0, roughness);
            vec3 specularAmbient = reflectColor * kS_ambient;

            // Clearcoat ambient: a sharp, near-mirror reflection layer on top, scaled by
            // how much clearcoat the material has.
            if (clearcoat > 0.0) {
                vec3 ccEnv = sampleAmbientSpecular(R, sunDir, clearcoatRoughness);
                vec3 clearcoatReflect = uHasEnvironmentMap
                    ? ccEnv
                    : mix(groundColor, ccEnv, clamp(0.5 * R.y + 0.5, 0.0, 1.0));
                float clearcoatFresnel = fresnelSchlickRoughness(max(dot(N, V), 0.0), vec3(0.04), clearcoatRoughness).r;
                specularAmbient += clearcoatReflect * clearcoatFresnel * clearcoat;
            }

            // Ground Bounce GI (Fake)
            float bounceMix = uNumLights > 0 ? smoothstep(0.0, 1.0, -uLights[0].direction.y) : 0.0;
            vec3 bounceColor = groundColor * albedo * 1.5 * (uNumLights > 0 ? uLights[0].intensity : 1.0); // Bright ground bounce
            float bounceAmount = max(dot(N, vec3(0.0, -1.0, 0.0)), 0.0); // Surface facing down
            vec3 gi = bounceColor * bounceAmount * bounceMix * (1.0 - metallic);

            // Ambient occlusion, applied to indirect light only.
            //
            // This used to be a whole-image multiply in the post pass, which scaled
            // direct sunlight by AO as well. Direct light is not occluded by the
            // ambient visibility term - the shadow map already answers that - so
            // darkening it too compressed lit surfaces and creases toward each
            // other and read as flat. Here it attenuates exactly what it describes:
            // the hemisphere of incoming indirect light.
            float ssao = uHasSSAO
                ? mix(1.0, texture(uSSAOMap, TexCoords).a, clamp(uSSAOStrength, 0.0, 1.0))
                : 1.0;

            // The shadow term darkens ambient as a coarse stand-in for "this point
            // cannot see much of the sky". It is kept, but no longer has to carry
            // the contact detail SSAO now supplies.
            float occlusion = mix(1.0, 0.2, main_shadow);
            vec3 ambient = (diffuseAmbient + specularAmbient + gi) * occlusion * ssao;

            // Baked indirect light, added rather than attenuated by the shadow term:
            // it already accounts for occlusion - that is what the bake solved - and
            // darkening it again would double-count every shadowed surface.
            //
            // SSAO still applies, because it is not the same quantity: a lightmap is
            // baked at a texel density far too coarse to capture the darkening where
            // two surfaces actually meet, and that contact detail is most of what
            // makes a scene read as grounded rather than pasted together.
            vec3 bakedIndirect = texture(gBakedGI, TexCoords).rgb;
            ambient += bakedIndirect * albedo * (1.0 - metallic) * ssao;

            if (uEnableRayTracing) {
                float fake_ao = uNumLights > 0 ? clamp(dot(N, normalize(-uLights[0].direction)) * 0.5 + 0.5, 0.0, 1.0) : 1.0;
                fake_ao = pow(fake_ao, 2.0);
                ambient *= (0.5 + 0.5 * fake_ao);
            }

            // Volumetric Light Shafts (Raymarching)
            float volumetric = 0.0;
            if (uNumLights > 0 && uLights[0].type == 0 && uEnableUE4Lighting) {
                vec3 startPos = uCameraPos;
                vec3 endPos = FragPos;
                vec3 rayDir = endPos - startPos;
                float rayLength = length(rayDir);
                rayDir /= rayLength;
                
                int numSteps = 32;
                float stepSize = rayLength / float(numSteps);
                
                // Simple dither
                float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 0.5;
                vec3 currentPos = startPos + rayDir * (stepSize * dither);
                
                float scattering = 0.0005 * uLights[0].intensity; // Reduced from 0.02 to prevent blowout
                
                for (int j = 0; j < numSteps; j++) {
                    vec4 lightSpacePos = uLightSpaceMatrix * vec4(currentPos, 1.0);
                    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
                    projCoords = projCoords * 0.5 + 0.5;
                    
                    if(projCoords.z <= 1.0 && projCoords.x >= 0.0 && projCoords.x <= 1.0 && projCoords.y >= 0.0 && projCoords.y <= 1.0) {
                        float pcfDepth = texture(shadowMap, projCoords.xy).r;
                        if (projCoords.z <= pcfDepth + 0.002) {
                            volumetric += scattering;
                        }
                    }
                    currentPos += rayDir * stepSize;
                }
            }

            // Emission is unlit radiance leaving the surface, so it is added after all
            // shading and is unaffected by shadowing or ambient occlusion.
            vec3 color = ambient + Lo + albedo * emissive
                       + (uNumLights > 0 ? volumetric * uLights[0].color : vec3(0.0));

            // Aerial perspective. Distant surfaces are seen through kilometres of air
            // that both absorbs their light and scatters skylight toward the viewer,
            // which is a large part of why a render without it reads as "clean" but
            // synthetic. Fog density falls off exponentially with height, its colour
            // is taken from the sky in the view direction (so it stays consistent with
            // the background rather than being a flat grey wash), and it brightens
            // toward the sun to approximate Mie forward-scattering.
            {
                vec3 toFrag = FragPos - uCameraPos;
                float dist = length(toFrag);
                vec3 viewDir = dist > 0.0001 ? toFrag / dist : vec3(0.0, 0.0, -1.0);

                // Height falloff integrated along the ray between camera and fragment.
                // Heights must be absolute for height-based fog; FragPos is relative.
                float hCam = uCameraWorldPos.y;
                float hFrag = uCameraWorldPos.y + FragPos.y;
                float heightFactor = exp(-max(hCam - uFogHeight, 0.0) * uFogHeightFalloff)
                                   + exp(-max(hFrag - uFogHeight, 0.0) * uFogHeightFalloff);
                float opticalDepth = dist * uFogDensity * 0.5 * heightFactor;
                float fogAmount = 1.0 - exp(-opticalDepth);

                vec3 fogColor = uHasEnvironmentMap ? sampleEnvironmentSpecular(viewDir, 0.55)
                                                   : sampleAnalyticSky(viewDir, sunDir);
                // Mie forward scattering toward the sun.
                float sunAmount = pow(max(dot(viewDir, sunDir), 0.0), 8.0);
                vec3 sunTint = (uNumLights > 0) ? uLights[0].color : vec3(1.0);
                fogColor = mix(fogColor, fogColor * 1.6 + sunTint * 0.35, sunAmount);

                color = mix(color, fogColor, clamp(fogAmount, 0.0, 1.0));
            }

            if (uLightDebug != 0) {
                vec3 d = vec3(0.0);
                if      (uLightDebug == 1) d = albedo;
                else if (uLightDebug == 2) d = N * 0.5 + 0.5;
                else if (uLightDebug == 3) d = fract(FragPos * 0.25);
                else if (uLightDebug == 4) d = vec3(metallic);
                else if (uLightDebug == 5) d = vec3(roughness);
                else if (uLightDebug == 6) d = Lo;                 // direct lighting only
                else if (uLightDebug == 7) d = ambient;            // ambient/IBL only
                else if (uLightDebug == 8) d = diffuseAmbient;
                else if (uLightDebug == 9) d = specularAmbient;
                else if (uLightDebug == 10) d = vec3(main_shadow);
                else if (uLightDebug == 11) d = reflectColor;
                else if (uLightDebug == 12) d = kS_ambient;
                else if (uLightDebug == 13) d = sampleAmbientDiffuse(N, sunDir);
                else if (uLightDebug == 14) d = vec3(volumetric);
                // Non-finite isolation views: each channel flags one term, so a
                // magenta frame can be narrowed to the exact expression producing it.
                else if (uLightDebug == 15) d = vec3(
                    (any(isnan(Lo)) || any(isinf(Lo))) ? 1.0 : 0.0,
                    (any(isnan(ambient)) || any(isinf(ambient))) ? 1.0 : 0.0,
                    (isnan(volumetric) || isinf(volumetric)) ? 1.0 : 0.0);
                else if (uLightDebug == 16) d = vec3(
                    (any(isnan(diffuseAmbient)) || any(isinf(diffuseAmbient))) ? 1.0 : 0.0,
                    (any(isnan(specularAmbient)) || any(isinf(specularAmbient))) ? 1.0 : 0.0,
                    (any(isnan(gi)) || any(isinf(gi))) ? 1.0 : 0.0);
                else if (uLightDebug == 17) d = vec3(
                    (any(isnan(reflectColor)) || any(isinf(reflectColor))) ? 1.0 : 0.0,
                    (any(isnan(kS_ambient)) || any(isinf(kS_ambient))) ? 1.0 : 0.0,
                    (any(isnan(skyAboveNormal)) || any(isinf(skyAboveNormal))) ? 1.0 : 0.0);
                else if (uLightDebug == 18) d = vec3(
                    (isnan(main_shadow) || isinf(main_shadow)) ? 1.0 : 0.0,
                    (any(isnan(N)) || any(isinf(N))) ? 1.0 : 0.0,
                    (any(isnan(V)) || any(isinf(V))) ? 1.0 : 0.0);
                if (uLightDebug < 15 && (any(isnan(color)) || any(isinf(color)))) d = vec3(1.0, 0.0, 1.0);
                FragColor = vec4(d, 1.0);
                return;
            }

            FragColor = vec4(color, clamp(1.0 - roughness, 0.0, 1.0));
        }
    )";

    lighting_shader_program = compile_shaders(lighting_vertex_shader_source, lighting_fragment_shader_source);
    report_loading_progress("Compiling lighting shaders");

    // Look up uniforms for Geometry Pass
    mvp_location = glGetUniformLocation(geometry_shader_program, "uMVP");
    model_location = glGetUniformLocation(geometry_shader_program, "uModel");
    color_override_location = glGetUniformLocation(geometry_shader_program, "uColorOverride");
    metallic_location = glGetUniformLocation(geometry_shader_program, "uMetallic");
    roughness_location = glGetUniformLocation(geometry_shader_program, "uRoughness");
    ue4_lighting_location = glGetUniformLocation(geometry_shader_program, "uEnableUE4Lighting");
    has_diffuse_texture_location = glGetUniformLocation(geometry_shader_program, "uHasDiffuseTexture");
    diffuse_texture_location = glGetUniformLocation(geometry_shader_program, "uDiffuseTexture");
    clearcoat_location = glGetUniformLocation(geometry_shader_program, "uClearcoat");
    clearcoat_roughness_location = glGetUniformLocation(geometry_shader_program, "uClearcoatRoughness");
    sheen_location = glGetUniformLocation(geometry_shader_program, "uSheen");
    subsurface_location = glGetUniformLocation(geometry_shader_program, "uSubsurface");
    emissive_location = glGetUniformLocation(geometry_shader_program, "uEmissive");
    skinned_location = glGetUniformLocation(geometry_shader_program, "uSkinned");
    lightmap_location = glGetUniformLocation(geometry_shader_program, "uLightmap");
    has_lightmap_location = glGetUniformLocation(geometry_shader_program, "uHasLightmap");
    ambient_cube_location = glGetUniformLocation(geometry_shader_program, "uAmbientCube");
    bones_location = glGetUniformLocation(geometry_shader_program, "uBones");

    // The palette is the single largest uniform in the vertex stage; reporting the
    // driver's budget makes a link failure on a thin driver immediately diagnosable
    // rather than presenting as "shaders stopped working".
    {
        GLint max_vertex_uniform_components = 0;
        glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &max_vertex_uniform_components);
        const int required = kMaxBones * 16;
        if (max_vertex_uniform_components < required) {
            std::cout << "[Renderer] Warning: driver reports " << max_vertex_uniform_components
                      << " vertex uniform components but the " << kMaxBones
                      << "-bone skinning palette needs " << required
                      << "; skinned meshes may fail to link." << std::endl;
        }
    }

    // Look up uniforms for Lighting Pass
    camera_pos_location = glGetUniformLocation(lighting_shader_program, "uCameraPos");
    num_lights_location = glGetUniformLocation(lighting_shader_program, "uNumLights");
    ray_tracing_location = glGetUniformLocation(lighting_shader_program, "uEnableRayTracing");
    env_map_location = glGetUniformLocation(lighting_shader_program, "uEnvironmentMap");
    has_env_map_location = glGetUniformLocation(lighting_shader_program, "uHasEnvironmentMap");
    env_map_max_lod_location = glGetUniformLocation(lighting_shader_program, "uEnvironmentMaxLod");
    irradiance_map_location = glGetUniformLocation(lighting_shader_program, "uIrradianceMap");
    prefiltered_env_location = glGetUniformLocation(lighting_shader_program, "uPrefilteredEnv");

    glUseProgram(lighting_shader_program);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "gPosition"), 0);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "gNormal"), 1);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "gAlbedoSpec"), 2);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "gPBR"), 3);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "gBakedGI"), 12);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "shadowMap"), 4);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "uEnvironmentMap"), 5);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "uIrradianceMap"), 6);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "uPrefilteredEnv"), 7);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "uSSAOMap"), 8);
    glUseProgram(0);

    // Depth Shader
    const std::string depth_vert = R"(
        #version 450 core
        layout (location = 0) in vec3 aPos;
        layout (location = 4) in ivec4 aBoneIDs;
        layout (location = 5) in vec4 aWeights;
        uniform mat4 uLightSpaceMatrix;
        uniform mat4 uModel;

        const int MAX_BONES = 128;
        uniform bool uSkinned;
        uniform mat4 uBones[MAX_BONES];

        void main() {
            // Must match the geometry pass exactly: a caster posed differently from
            // the mesh it belongs to casts a shadow that drifts off the character.
            vec3 position = aPos;
            if (uSkinned) {
                float total = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
                if (total > 0.0) {
                    mat4 skin = mat4(0.0);
                    for (int i = 0; i < 4; ++i) {
                        int index = clamp(aBoneIDs[i], 0, MAX_BONES - 1);
                        skin += uBones[index] * aWeights[i];
                    }
                    skin /= total;
                    position = (skin * vec4(aPos, 1.0)).xyz;
                }
            }
            gl_Position = uLightSpaceMatrix * uModel * vec4(position, 1.0);
        }
    )";
    const std::string depth_frag = R"(
        #version 450 core
        void main() {
            // gl_FragDepth = gl_FragCoord.z;
        }
    )";
    
    const char* v_src = depth_vert.c_str();
    unsigned int v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &v_src, NULL); glCompileShader(v);
    const char* f_src = depth_frag.c_str();
    unsigned int f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &f_src, NULL); glCompileShader(f);
    depth_shader_program = glCreateProgram();
    glAttachShader(depth_shader_program, v);
    glAttachShader(depth_shader_program, f);
    glLinkProgram(depth_shader_program);
    report_shader_compile(v, "depth", "vertex");
    report_shader_compile(f, "depth", "fragment");
    report_program_link(depth_shader_program, "depth");
    report_loading_progress("Compiling shadow shaders");

    depth_model_location = glGetUniformLocation(depth_shader_program, "uModel");
    depth_light_space_location = glGetUniformLocation(depth_shader_program, "uLightSpaceMatrix");
    depth_skinned_location = glGetUniformLocation(depth_shader_program, "uSkinned");
    depth_bones_location = glGetUniformLocation(depth_shader_program, "uBones");

    // FXAA Shader
    const std::string fxaa_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";
    
    const std::string fxaa_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D screenTexture;
        uniform sampler2D uDepthMap;
        uniform sampler2D uAdaptedLuminance;
        uniform sampler2D uSSAOMap;   // rgb = screen-space bounce light, a = ambient occlusion
        uniform sampler2D uBloomMap;
        uniform sampler2D uPBRMap;      // gBuffer PBR: r = metallic, g = roughness
        uniform float uBloomIntensity;
        uniform bool uEnableSSR;
        uniform vec2 texelStep;
        uniform mat4 uProj;
        uniform mat4 uInvProj;
        // Post-process debug views, selected with the LITHIUM_DEBUG_VIEW env var.
        // 0 = off (normal output). See resolve_fxaa for the list.
        uniform int uDebugView;

        // Pseudo-random hash
        float hash(vec2 p) {
            return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
        }

        vec3 reconstructPosition(vec2 uv, float z) {
            float x = uv.x * 2.0 - 1.0;
            float y = uv.y * 2.0 - 1.0;
            vec4 pos_s = vec4(x, y, z * 2.0 - 1.0, 1.0);
            vec4 pos_v = uInvProj * pos_s;
            return pos_v.xyz / pos_v.w;
        }

        vec3 reconstructNormal(vec3 pos) {
            return normalize(cross(dFdx(pos), dFdy(pos)));
        }

        void main() {
            float FXAA_SPAN_MAX = 8.0;
            float FXAA_REDUCE_MUL = 1.0/8.0;
            float FXAA_REDUCE_MIN = 1.0/128.0;

            vec3 rgbNW = texture(screenTexture, TexCoords + vec2(-1.0, -1.0) * texelStep).xyz;
            vec3 rgbNE = texture(screenTexture, TexCoords + vec2(1.0, -1.0) * texelStep).xyz;
            vec3 rgbSW = texture(screenTexture, TexCoords + vec2(-1.0, 1.0) * texelStep).xyz;
            vec3 rgbSE = texture(screenTexture, TexCoords + vec2(1.0, 1.0) * texelStep).xyz;
            // Center tap. FXAA needs the un-offset sample both to compute lumaM and
            // as the reference the blur candidates are validated against; without it
            // this shader did not compile at all, which silently disabled the entire
            // final pass (tonemap, gamma, exposure, bloom, SSAO/SSGI, SSR).
            vec3 color  = texture(screenTexture, TexCoords).xyz;

            vec3 luma = vec3(0.299, 0.587, 0.114);
            float lumaNW = dot(rgbNW, luma);
            float lumaNE = dot(rgbNE, luma);
            float lumaSW = dot(rgbSW, luma);
            float lumaSE = dot(rgbSE, luma);
            float lumaM  = dot(color,  luma);

            float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
            float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

            vec2 dir;
            dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
            dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

            float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
            float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

            dir = min(vec2(FXAA_SPAN_MAX, FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX), dir * rcpDirMin)) * texelStep;

            vec3 rgbA = (1.0/2.0) * (
                texture(screenTexture, TexCoords + dir * (1.0/3.0 - 0.5)).xyz +
                texture(screenTexture, TexCoords + dir * (2.0/3.0 - 0.5)).xyz);
            vec3 rgbB = rgbA * (1.0/2.0) + (1.0/4.0) * (
                texture(screenTexture, TexCoords + dir * (0.0/3.0 - 0.5)).xyz +
                texture(screenTexture, TexCoords + dir * (3.0/3.0 - 0.5)).xyz);
            float lumaB = dot(rgbB, luma);
            
            // Lens Post-Processing (Vignette + Chromatic Aberration)
            vec2 center = TexCoords - 0.5;
            float dist = length(center);
            
            // Chromatic Aberration
            float ca_amount = 0.0; // Disabled to prevent blurriness
            vec3 colorCA;
            if((lumaB < lumaMin) || (lumaB > lumaMax)) {
                colorCA = rgbA;
            } else {
                colorCA = rgbB;
            }
            
            // Sample R and B slightly offset (Disabled CA to prevent blurriness)
            vec3 finalColor = colorCA;

            // Bloom from the mip pyramid built in render_bloom(): every scale from
            // half-res down to a few texels contributes, so highlights get a wide,
            // soft falloff instead of the small hard halo a handful of taps produces.
            finalColor += texture(uBloomMap, TexCoords).rgb * uBloomIntensity;
            
            vec3 dbgSceneColor = color;
            vec3 dbgPreExposure = vec3(0.0);
            vec3 dbgNormal = vec3(0.0);
            float dbgAO = 1.0;
            vec3 dbgSSGI = vec3(0.0);
            float dbgSmoothness = 0.0;

            // Screen-Space Global Illumination (SSGI) & SSAO.
            // Both come from the dedicated half-res pass (render_ssao) and have
            // already been depth-aware blurred there; computing them here per-pixel
            // put the raw, unfiltered noise of a 16-sample stochastic estimator
            // directly on screen.
            float depth = texture(uDepthMap, TexCoords).r;
            if (depth < 0.9999) { // Don't process the sky
                vec3 viewPos = reconstructPosition(TexCoords, depth);
                vec3 normal = reconstructNormal(viewPos);
                // Bias normal towards viewer slightly to prevent self-shadowing acne
                normal = normalize(normal - vec3(0,0,1)*0.1);
                dbgNormal = normal;

                vec4 aoSample = texture(uSSAOMap, TexCoords);
                vec3 ssgi = aoSample.rgb;
                float ao = aoSample.a;
                dbgAO = ao;
                dbgSSGI = ssgi;

                // AO is applied in the lighting pass now, against the indirect term
                // alone. Re-applying it here would darken direct light and double the
                // occlusion on ambient - which is what made the image look flat.
                // Only the screen-space bounce is composited here, because it is
                // additive indirect light rather than an occlusion factor.
                finalColor += ssgi * 0.35;
                
                // Screen Space Reflections (SSR)
                float smoothness = texture(screenTexture, TexCoords).a;
                dbgSmoothness = smoothness;
                if (uEnableSSR && smoothness > 0.01) {
                    vec3 viewDir = normalize(viewPos); // Camera is at origin in view space
                    vec3 reflectDir = normalize(reflect(viewDir, normal));
                    
                    vec3 hitColor = vec3(0.0);
                    float stepSize = 0.02;
                    float maxSteps = 100.0;
                    vec3 rayPos = viewPos;
                    float hit = 0.0;
                    
                    for (float i = 1.0; i <= maxSteps; i += 1.0) {
                        rayPos += reflectDir * stepSize;
                        vec4 projPos = uProj * vec4(rayPos, 1.0);
                        projPos.xyz /= projPos.w;
                        projPos.xyz = projPos.xyz * 0.5 + 0.5;
                        
                        if (projPos.x < 0.0 || projPos.x > 1.0 || projPos.y < 0.0 || projPos.y > 1.0) {
                            break; // Off screen
                        }
                        
                        float d = texture(uDepthMap, projPos.xy).r;
                        vec3 sampleViewPos = reconstructPosition(projPos.xy, d);
                        
                        // Check if ray is behind the depth buffer (but not too far behind)
                        if (sampleViewPos.z > rayPos.z && sampleViewPos.z < rayPos.z + 0.1) {
                            // Binary search refinement
                            for(int j=0; j<8; j++){
                                stepSize *= 0.5;
                                if(sampleViewPos.z > rayPos.z) rayPos -= reflectDir * stepSize;
                                else rayPos += reflectDir * stepSize;
                                
                                projPos = uProj * vec4(rayPos, 1.0);
                                projPos.xyz /= projPos.w;
                                projPos.xyz = projPos.xyz * 0.5 + 0.5;
                                d = texture(uDepthMap, projPos.xy).r;
                                sampleViewPos = reconstructPosition(projPos.xy, d);
                            }
                            
                            hitColor = texture(screenTexture, projPos.xy).rgb;
                            hit = 1.0;
                            // Fade out towards screen edges
                            vec2 edgeFade = smoothstep(0.0, 0.05, projPos.xy) * smoothstep(1.0, 0.95, projPos.xy);
                            hit *= edgeFade.x * edgeFade.y;
                            break;
                        }
                    }
                    
                    // Fresnel calculation for reflectivity amount
                    float fresnel = pow(1.0 - max(dot(normal, -viewDir), 0.0), 5.0);
                    // Reflectance from the material's actual F0. Metals reflect
                    // almost everything; the old fixed 0.04 dielectric base meant a
                    // polished metal floor mirrored at ~4% and looked matte.
                    float ssrMetallic = texture(uPBRMap, TexCoords).r;
                    float F0 = mix(0.04, 1.0, ssrMetallic);
                    float reflectivity = (F0 + (1.0 - F0) * fresnel) * smoothness;
                    
                    finalColor = mix(finalColor, hitColor, hit * reflectivity);
                }
            }

            // Vignette
            float vignette = smoothstep(0.8, 0.3, dist);
            finalColor *= vignette;

            // HDR Tonemapping & Gamma Correction
            // Eye adaptation: derive exposure from the temporally-smoothed scene
            // luminance (key-value auto-exposure), so bright and dark scenes don't
            // both render at the same fixed brightness like a static exposure would.
            float adaptedLuminance = exp(texture(uAdaptedLuminance, vec2(0.5)).r);
            float keyValue = 0.18; // "middle grey"
            float autoExposure = clamp(keyValue / max(adaptedLuminance, 0.001), 0.15, 8.0);
            // Bias below middle grey rather than above it. Auto-exposure normalises the
            // frame, so turning lights down does nothing to perceived brightness - it
            // just raises the exposure to compensate. This multiplier is the actual
            // control over how bright the image lands, and at 1.2 every scene was
            // pushed a fifth above middle grey and read as washed out.
            float exposure = autoExposure * 0.82;
            dbgPreExposure = finalColor;
            finalColor *= exposure;
            float A = 2.51; float B = 0.03; float C = 2.43; float D = 0.59; float E = 0.14;
            finalColor = clamp((finalColor * (A * finalColor + B)) / (finalColor * (C * finalColor + D) + E), 0.0, 1.0);
            finalColor = pow(finalColor, vec3(1.0/2.2));

            // Film grain, applied in display space after tonemapping. Adding it before
            // exposure meant the auto-exposure multiplier scaled the grain too, so a
            // dim scene (large exposure) turned a subtle 1.5% dither into heavy
            // visible speckle across the whole frame, sky included.
            float grain = (hash(TexCoords * 1234.0 + lumaM * 100.0) - 0.5) * 0.012;
            finalColor = clamp(finalColor + vec3(grain), 0.0, 1.0);

            if (uDebugView != 0) {
                vec3 dbg = vec3(0.0);
                if      (uDebugView == 1) dbg = pow(clamp(dbgSceneColor, 0.0, 1.0), vec3(1.0/2.2));
                else if (uDebugView == 2) dbg = vec3(dbgAO);
                else if (uDebugView == 3) dbg = pow(clamp(dbgSSGI, 0.0, 1.0), vec3(1.0/2.2));
                else if (uDebugView == 4) dbg = vec3(pow(clamp(depth, 0.0, 1.0), 64.0));
                else if (uDebugView == 5) dbg = dbgNormal * 0.5 + 0.5;
                else if (uDebugView == 6) dbg = pow(clamp(dbgPreExposure, 0.0, 1.0), vec3(1.0/2.2));
                else if (uDebugView == 7) dbg = vec3(clamp(exposure / 8.0, 0.0, 1.0));
                else if (uDebugView == 8) dbg = vec3(dbgSmoothness);
                else if (uDebugView == 9) dbg = vec3(clamp(adaptedLuminance, 0.0, 1.0));
                // Flag non-finite results loudly in magenta - NaN propagates silently
                // through the clamp/tonemap chain and reads as plain black otherwise.
                if (any(isnan(finalColor)) || any(isinf(finalColor))) dbg = vec3(1.0, 0.0, 1.0);
                FragColor = vec4(dbg, 1.0);
                return;
            }

            FragColor = vec4(finalColor, 1.0);
        }
    )";
    
    // Built through compile_shaders so it uses the on-disk program binary
    // cache; this is the expensive one to compile from source.
    fxaa_shader_program = compile_shaders(fxaa_vert, fxaa_frag);
    report_program_link(fxaa_shader_program, "fxaa");
    report_loading_progress("Compiling post-process shaders");


    // God Rays Shader
    const std::string god_rays_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";
    
    const std::string god_rays_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D screenTexture;
        uniform vec2 uSunPos;
        uniform vec3 uSunColor;

        // Radial light shafts (crepuscular rays). This is the occlusion-based
        // approximation: march from the pixel toward the sun's screen position and
        // accumulate whatever bright sky is visible along the way, so geometry between
        // the pixel and the sun carves shadow beams out of the glow.
        //
        // The previous version also stamped a fake lens flare - five "ghost" sprites
        // spaced along the line through screen centre plus a halo - which rendered as a
        // dotted line of coloured blobs across the frame. That is a stylised photographic
        // artefact, not light transport, and it read as garbage on screen, so it is gone.
        void main() {
            const int NUM_SAMPLES = 128;
            const float density = 0.9;
            const float decay = 0.97;
            const float weight = 0.018;
            // Only genuinely bright things (the sky/sun disc) should throw shafts;
            // ordinary lit surfaces must not smear toward the sun.
            const float threshold = 3.0;

            vec2 deltaTexCoord = (TexCoords - uSunPos) * (density / float(NUM_SAMPLES));

            // Jitter each pixel's starting position by a fraction of one step. The
            // march samples at fixed intervals, so without this, neighbouring pixels
            // sample the same small bright feature (the sun disc, or a hot patch of
            // sky) in lockstep - some step onto it, some step over it - and the effect
            // aliases into a dashed line of bright dots running toward the sun.
            // Interleaved gradient noise decorrelates the phase per pixel, turning that
            // structured aliasing into fine noise the accumulation averages away.
            float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            vec2 tc = TexCoords - deltaTexCoord * jitter;
            float illuminationDecay = 1.0;
            vec3 shafts = vec3(0.0);

            for (int i = 0; i < NUM_SAMPLES; ++i) {
                tc -= deltaTexCoord;
                // Sampling outside the frame has no meaningful data; stopping avoids
                // dragging clamped edge pixels inward as streaks.
                if (tc.x < 0.0 || tc.x > 1.0 || tc.y < 0.0 || tc.y > 1.0) break;

                vec3 s = texture(screenTexture, tc).rgb;
                float brightness = dot(s, vec3(0.2126, 0.7152, 0.0722));
                // Smooth knee instead of a hard cutoff, which used to make shafts pop
                // in and out as the sun moved across a pixel boundary.
                float contribution = smoothstep(threshold, threshold * 2.0, brightness);
                shafts += s * contribution * illuminationDecay * weight;
                illuminationDecay *= decay;
            }

            // Fade the whole effect out as the sun leaves the frame, so shafts do not
            // snap off the instant the sun crosses the screen edge.
            vec2 d = max(vec2(0.0), abs(uSunPos - vec2(0.5)) - vec2(0.5));
            float offscreenFade = 1.0 - clamp(length(d) * 4.0, 0.0, 1.0);

            FragColor = vec4(shafts * uSunColor * offscreenFade, 1.0);
        }
    )";
    
    // Built through compile_shaders so it uses the on-disk program binary
    // cache; this is the expensive one to compile from source.
    god_rays_shader_program = compile_shaders(god_rays_vert, god_rays_frag);
    report_program_link(god_rays_shader_program, "god_rays");
    report_loading_progress();

    god_rays_screen_texture_loc = glGetUniformLocation(god_rays_shader_program, "screenTexture");
    god_rays_sun_pos_loc = glGetUniformLocation(god_rays_shader_program, "uSunPos");

    // Composite Shader
    const std::string composite_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";
    
    const std::string composite_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D baseTexture;
        uniform sampler2D blendTexture;
        
        void main() {
            vec3 baseColor = texture(baseTexture, TexCoords).rgb;
            vec3 blendColor = texture(blendTexture, TexCoords).rgb;
            FragColor = vec4(baseColor + blendColor, 1.0);
        }
    )";
    
    // Built through compile_shaders so it uses the on-disk program binary
    // cache; this is the expensive one to compile from source.
    composite_shader_program = compile_shaders(composite_vert, composite_frag);
    report_program_link(composite_shader_program, "composite");
    report_loading_progress();

    composite_base_texture_loc = glGetUniformLocation(composite_shader_program, "baseTexture");
    composite_blend_texture_loc = glGetUniformLocation(composite_shader_program, "blendTexture");

    // TAA Shader
    const std::string taa_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";
    
    const std::string taa_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D currentColor;
        uniform sampler2D historyColor;
        uniform sampler2D uDepthMap;
        uniform mat4 uReproject;
        uniform float uBlendFactor;

        void main() {
            vec4 currentSample = texture(currentColor, TexCoords);
            vec3 current = currentSample.rgb;
            // The lighting pass packs material smoothness (1 - roughness) into alpha,
            // and the SSR stage downstream reads it from here. Writing a constant 1.0
            // threw that away, so every surface - including a roughness-0.9 floor -
            // was treated as a mirror by the reflection pass.
            float smoothness = currentSample.a;
            float centerDepth = texture(uDepthMap, TexCoords).r;
            
            if (centerDepth > 0.9999) {
                FragColor = vec4(current, smoothness);
                return;
            }

            // Neighborhood color bounds for history variance clipping below. `current`
            // itself is intentionally left as the sharp center-texel sample from above -
            // this is real-time rasterized input, not noisy path-traced output, so there's
            // nothing here that needs spatial denoising, and blurring it every pixel every
            // frame was just softening the whole image for no benefit.
            vec2 texSize = vec2(textureSize(currentColor, 0));
            vec2 texelSize = 1.0 / texSize;

            vec3 c_min = vec3(999.0);
            vec3 c_max = vec3(-999.0);

            for(int x = -1; x <= 1; x++) {
                for(int y = -1; y <= 1; y++) {
                    vec2 sampleUV = TexCoords + vec2(x, y) * texelSize;
                    vec3 sampleColor = texture(currentColor, sampleUV).rgb;
                    c_min = min(c_min, sampleColor);
                    c_max = max(c_max, sampleColor);
                }
            }

            // Reproject history
            vec4 prevClip = uReproject * vec4(TexCoords * 2.0 - 1.0, centerDepth * 2.0 - 1.0, 1.0);
            prevClip.xyz /= prevClip.w;
            vec2 prevUV = prevClip.xy * 0.5 + 0.5;

            if (prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0) {
                FragColor = vec4(current, smoothness);
                return;
            }

            vec3 history = texture(historyColor, prevUV).rgb;

            // Neighborhood Clamping (Variance Clipping) to prevent ghosting
            history = clamp(history, c_min, c_max);
            
            FragColor = vec4(mix(history, current, uBlendFactor), smoothness);
        }
    )";
    
    // Built through compile_shaders so it uses the on-disk program binary
    // cache; this is the expensive one to compile from source.
    taa_shader_program = compile_shaders(taa_vert, taa_frag);
    report_program_link(taa_shader_program, "taa");
    report_loading_progress();


    // TESLA present pass.
    //
    // The path tracer accumulates into an RGBA32F target holding (sum of radiance,
    // sample count). This divides one by the other and tonemaps, and does nothing
    // else - deliberately. Routing the path-traced buffer through the raster post
    // chain composited a stale G-buffer onto it: in TESLA mode the geometry pass
    // never runs, so SSAO, bloom and SSR were all sampling textures left over from
    // the last rasterised frame, and FXAA was smearing Monte Carlo noise rather
    // than resolving it.
    const std::string tesla_present_vert = R"(
        #version 330 core
        layout (location = 0) in vec2 aPos;
        out vec2 TexCoords;
        void main() {
            TexCoords = aPos * 0.5 + 0.5;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )";

    const std::string tesla_present_frag = R"(
        #version 330 core
        out vec4 FragColor;
        in vec2 TexCoords;

        uniform sampler2D uAccum;     // rgb = summed radiance, a = sample count
        uniform sampler2D uLuminance; // log-luminance pyramid; top level = frame average
        uniform float uExposure;      // manual multiplier, always applied
        uniform int   uAutoExposure;

        void main() {
            vec4 acc = texture(uAccum, TexCoords);
            // Dividing by each pixel's own sample count is what stops a partially
            // finished pass from showing a brightness step between the tiles
            // visited this round and the ones that have not been.
            float samples = max(acc.a, 1.0);
            vec3 color = acc.rgb / samples;

            float exposure = uExposure;
            if (uAutoExposure != 0) {
                // Level 20 clamps to the 1x1 top of the pyramid.
                float avg_log = textureLod(uLuminance, vec2(0.5), 20.0).r;
                float key = 0.18;   // middle grey, same target the raster path uses
                exposure *= clamp(key / max(exp(avg_log), 1e-4), 0.01, 500.0);
            }
            color *= exposure;

            // ACES filmic approximation, the same curve the raster path uses, so the
            // two render modes land at a comparable brightness.
            float A = 2.51; float B = 0.03; float C = 2.43; float D = 0.59; float E = 0.14;
            color = clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
            color = pow(color, vec3(1.0 / 2.2));

            if (any(isnan(color)) || any(isinf(color))) color = vec3(1.0, 0.0, 1.0);
            FragColor = vec4(color, 1.0);
        }
    )";

    tesla_present_program = compile_shaders(tesla_present_vert, tesla_present_frag);
    report_program_link(tesla_present_program, "tesla_present");

    // Auto-exposure for TESLA.
    //
    // The raster path derives exposure from a luminance texture the geometry pass
    // fills in, and that pass does not run in TESLA mode - so the path tracer had a
    // fixed exposure of 1.0 and any dim scene (a night interior, say) tonemapped to
    // black. This reduces the accumulated radiance to a log-luminance pyramid whose
    // 1x1 top level is the geometric mean of the frame, which is what the key-value
    // formula below wants. Log rather than linear so one bright window does not drag
    // the whole exposure down.
    const std::string tesla_lum_frag = R"(
        #version 330 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D uAccum;
        void main() {
            vec4 acc = texture(uAccum, TexCoords);
            vec3 radiance = acc.rgb / max(acc.a, 1.0);
            float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
            FragColor = vec4(log(max(lum, 1e-4)), 0.0, 0.0, 1.0);
        }
    )";
    tesla_lum_program = compile_shaders(tesla_present_vert, tesla_lum_frag);
    report_program_link(tesla_lum_program, "tesla_luminance");
    
    // Culling Compute Shader
    const std::string culling_compute_src = R"(
        #version 450 core
        layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

        struct DrawCommand {
            uint count;
            uint instanceCount;
            uint firstIndex;
            uint baseVertex;
            uint baseInstance;
        };

        struct MeshCluster {
            vec4 boundsCenter; // xyz = center, w = radius
            uint index_offset;
            uint index_count;
            uint pad1;
            uint pad2;
        };

        layout(std430, binding = 0) buffer ClusterBuffer {
            MeshCluster in_clusters[];
        };

        layout(std430, binding = 1) buffer CommandBuffer {
            DrawCommand out_commands[];
        };

        uniform vec4 uFrustumPlanes[6];
        uniform uint uNumClusters;
        uniform mat4 uModelMatrix;

        void main() {
            uint idx = gl_GlobalInvocationID.x;
            if (idx >= uNumClusters) return;
            
            vec3 local_center = in_clusters[idx].boundsCenter.xyz;
            float local_radius = in_clusters[idx].boundsCenter.w;
            
            vec4 world_center_4 = uModelMatrix * vec4(local_center, 1.0);
            vec3 world_center = world_center_4.xyz / world_center_4.w;
            
            // Extract scale from model matrix
            float scale_x = length(vec3(uModelMatrix[0][0], uModelMatrix[0][1], uModelMatrix[0][2]));
            float scale_y = length(vec3(uModelMatrix[1][0], uModelMatrix[1][1], uModelMatrix[1][2]));
            float scale_z = length(vec3(uModelMatrix[2][0], uModelMatrix[2][1], uModelMatrix[2][2]));
            float max_scale = max(max(scale_x, scale_y), scale_z);
            float world_radius = local_radius * max_scale;
            
            bool visible = true;
            for(int i = 0; i < 6; ++i) {
                if(dot(uFrustumPlanes[i].xyz, world_center) + uFrustumPlanes[i].w < -world_radius) {
                    visible = false;
                    break;
                }
            }
            
            out_commands[idx].count = in_clusters[idx].index_count;
            out_commands[idx].instanceCount = visible ? 1 : 0;
            out_commands[idx].firstIndex = in_clusters[idx].index_offset;
            out_commands[idx].baseVertex = 0;
            out_commands[idx].baseInstance = 0;
        }
    )";

    // Compile Compute Shader. Compute is optional: it needs GL 4.3+ and real
    // hardware support, and glCreateShader returns 0 where the driver lacks it (for
    // example Sandy Bridge era Intel parts, which report a 4.5 context but cannot run
    // compute). Previously that produced a bare "COMPUTE SHADER COMPILATION FAILED"
    // with an empty log followed by a link failure, and left a non-zero but unlinked
    // program handle that the cluster-culling paths would still try to use.
    culling_compute_program = 0;
    unsigned int compute_shader = glCreateShader(GL_COMPUTE_SHADER);
    if (compute_shader == 0) {
        std::cout << "[Renderer] Compute shaders unavailable on this driver; "
                     "GPU cluster culling disabled (rendering is unaffected)." << std::endl;
    } else {
        const char* culling_src = culling_compute_src.c_str();
        glShaderSource(compute_shader, 1, &culling_src, NULL);
        glCompileShader(compute_shader);
        if (report_shader_compile(compute_shader, "culling_compute", "compute")) {
            unsigned int program = glCreateProgram();
            glAttachShader(program, compute_shader);
            glLinkProgram(program);
            if (report_program_link(program, "culling_compute")) {
                culling_compute_program = program;
            } else {
                glDeleteProgram(program);
            }
        }
        glDeleteShader(compute_shader);
    }

    // Create SSBOs
    glGenBuffers(1, &cluster_ssbo);
    glGenBuffers(1, &command_ssbo);

    // Skybox Shader
    const std::string sky_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        out vec3 RayDir;
        uniform vec3 uCamForward;
        uniform vec3 uCamRight;
        uniform vec3 uCamUp;
        uniform float uFovTan;
        uniform float uAspect;
        void main() {
            gl_Position = vec4(aPos, 0.999999, 1.0);
            RayDir = uCamForward + uCamRight * (aPos.x * uAspect * uFovTan) + uCamUp * (aPos.y * uFovTan);
        }
    )";
    const std::string sky_frag = R"(
        #version 450 core
        in vec3 RayDir;
        out vec4 FragColor;
        uniform vec3 uSunDir;
        uniform float uTime;
        uniform bool uEnable3DClouds;
        uniform int uSkyMode;      // 0 = environment HDRI, 1 = procedural, 2 = void colour
        uniform vec3 uVoidColor;
        uniform sampler2D uEnvironmentMap;
        uniform bool uHasEnvironmentMap;

        vec2 skyDirectionToEquirectUV(vec3 dir) {
            // See directionToEquirectUV's comment: bias x away from exactly 0 to
            // avoid the degenerate atan(0,0) case when looking straight up/down.
            vec2 uv = vec2(atan(dir.z, dir.x + 1e-5), asin(clamp(dir.y, -1.0, 1.0)));
            uv *= vec2(0.1591549, 0.3183099);
            uv += 0.5;
            return uv;
        }

        // Hash function for noise
        float hash(vec3 p) {
            p = fract(p * 0.3183099 + 0.1);
            p *= 17.0;
            return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
        }
        
        // 3D Noise
        float noise(vec3 x) {
            vec3 i = floor(x);
            vec3 f = fract(x);
            f = f * f * (3.0 - 2.0 * f);
            return mix(mix(mix(hash(i + vec3(0,0,0)), hash(i + vec3(1,0,0)), f.x),
                           mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
                       mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
                           mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y), f.z);
        }
        
        // Fractional Brownian Motion
        float fbm(vec3 x) {
            float v = 0.0;
            float a = 0.5;
            vec3 shift = vec3(100);
            for (int i = 0; i < 5; ++i) {
                v += a * noise(x);
                x = x * 2.0 + shift;
                a *= 0.5;
            }
            return v;
        }

        void main() {
            vec3 dir = normalize(RayDir);
            vec3 sunDir = normalize(uSunDir);

            // Flat void colour: an authored background, independent of any sky model.
            if (uSkyMode == 2) {
                FragColor = vec4(uVoidColor, 0.0);
                return;
            }

            if (uSkyMode == 0 && uHasEnvironmentMap) {
                // Show the loaded HDRI directly as the sky background so it's
                // visually consistent with the IBL it's providing for ambient light.
                FragColor = vec4(texture(uEnvironmentMap, skyDirectionToEquirectUV(dir)).rgb, 0.0);
                return;
            }

            // Highly Vibrant Sky Gradient (Less Cyan, deeper blue)
            vec3 zenith = vec3(0.1, 0.25, 0.8) * 1.2;
            vec3 horizon = vec3(0.45, 0.6, 0.9) * 1.1;
            
            // Mirror sky gradient below the horizon to eliminate the sharp middle line
            vec3 skyColor = mix(horizon, zenith, clamp(abs(dir.y), 0.0, 1.0));
            
            // Horizon haze (Rayleigh) - applied globally so the void matches the sky perfectly
            float haze = 1.0 - clamp(abs(dir.y), 0.0, 1.0);
            haze = pow(haze, 3.0);
            skyColor = mix(skyColor, vec3(0.7, 0.75, 0.8) * 1.2, haze * 0.4);
            
            if (dir.y > -0.05) {
                // Sun disk
                float sun = max(0.0, dot(dir, sunDir));
                float sunDist = 1.0 - sun;
                
                // Clear and beautiful sun disk (physically accurate size)
                float sunMask = 1.0 - smoothstep(0.00001, 0.00005, sunDist);
                skyColor += vec3(1.0, 0.95, 0.8) * sunMask * 8.0;

                // Sun glow (Mie scattering / Volumetric) using stable exp falloff
                float sunGlow = exp(-sunDist * 15000.0) * 1.0;
                float sunGodRays = exp(-sunDist * 5000.0) * 0.25;
                skyColor += vec3(1.0, 0.85, 0.55) * (sunGlow + sunGodRays); // warm white-gold, not saturated yellow
                
                if (dir.y > 0.0) {
                    if (uEnable3DClouds) {
                        // 3D Volumetric Raymarching
                        float R = 40000.0;
                        float H_bottom = 1000.0;
                        float H_top = 2500.0;
                        
                        float t_bottom = -R * dir.y + sqrt(max(R * R * dir.y * dir.y + 2.0 * R * H_bottom + H_bottom * H_bottom, 0.0));
                        float t_top = -R * dir.y + sqrt(max(R * R * dir.y * dir.y + 2.0 * R * H_top + H_top * H_top, 0.0));
                        
                        vec3 p0 = dir * t_bottom;
                        vec3 p1 = dir * t_top;
                        
                        int steps = 12;
                        vec3 step_dir = (p1 - p0) / float(steps);
                        float step_len = length(step_dir);
                        
                        vec3 p = p0;
                        float total_density = 0.0;
                        vec3 total_color = vec3(0.0);
                        
                        vec3 ambient = vec3(0.55, 0.65, 0.75);
                        vec3 sunColor = vec3(1.0, 0.7, 0.3);
                        
                        for (int i = 0; i < steps; ++i) {
                            if (total_density >= 1.0) break;
                            vec3 sample_p = p;
                            sample_p.x += uTime * 40.0;
                            sample_p.z += uTime * 25.0;
                            
                            float macro = fbm(sample_p * 0.00015);
                            float micro = fbm(sample_p * 0.002);
                            float threshold = mix(0.35, 0.55, macro);
                            
                            // Attenuate density based on height to form cloud tops/bottoms
                            float h_fraction = float(i) / float(steps);
                            float height_atten = 1.0 - abs(h_fraction - 0.5) * 2.0;
                            
                            float density = smoothstep(threshold, threshold + 0.3, micro) * height_atten * 2.0;
                            
                            if (density > 0.0) {
                                float lightMicro = fbm((sample_p + sunDir * 150.0) * 0.002);
                                float shadowDensity = smoothstep(threshold, threshold + 0.3, lightMicro);
                                float shadow = clamp(1.0 - max(shadowDensity - density, 0.0) * 3.5, 0.25, 1.0);
                                
                                vec3 direct = vec3(1.0, 0.95, 0.9) * shadow * 1.6;
                                vec3 cloudColor = ambient + direct + sunColor * pow(sun, 8.0) * density * 0.6;
                                
                                float alpha = density * 0.15;
                                total_color += cloudColor * alpha * (1.0 - total_density);
                                total_density += alpha;
                            }
                            p += step_dir;
                        }
                        
                        float cloudAlpha = clamp(total_density * clamp(dir.y * 4.0, 0.0, 1.0), 0.0, 1.0);
                        skyColor = mix(skyColor, total_color, cloudAlpha);
                    } else {
                        // Current 2D Faux-3D clouds
                        float R = 40000.0; // Planet radius
                        float H = 1000.0; // Cloud layer height
                        
                        float rdY = dir.y;
                        float t = -R * rdY + sqrt(max(R * R * rdY * rdY + 2.0 * R * H + H * H, 0.0));
                        vec3 p = dir * t;
                        
                        p.x += uTime * 40.0;
                        p.z += uTime * 25.0;
                        
                        float macro = fbm(p * 0.00015);
                        float micro = fbm(p * 0.002);
                        
                        float threshold = mix(0.35, 0.55, macro);
                        float density = smoothstep(threshold, threshold + 0.3, micro);
                        
                        if (density > 0.0) {
                            float lightMicro = fbm((p + sunDir * 150.0) * 0.002);
                            float shadowDensity = smoothstep(threshold, threshold + 0.3, lightMicro);
                            float shadow = clamp(1.0 - max(shadowDensity - density, 0.0) * 3.5, 0.25, 1.0);
                            
                            vec3 ambient = vec3(0.55, 0.65, 0.75);
                            vec3 direct = vec3(1.0, 0.95, 0.9) * shadow * 1.6;
                            vec3 cloudColor = ambient + direct;
                            
                            vec3 sunColor = vec3(1.0, 0.7, 0.3);
                            cloudColor += sunColor * pow(sun, 8.0) * density * 0.6;
                            cloudColor = clamp(cloudColor, 0.0, 1.0);
                            
                            float cloudAlpha = density * clamp(dir.y * 4.0, 0.0, 1.0);
                            skyColor = mix(skyColor, cloudColor, cloudAlpha);
                        }
                    }
                }
            }

            // Night falloff, mirroring sampleAnalyticSky in the lighting pass. Only
            // the ambient model knew about the sun's elevation, so with the sun below
            // the horizon a scene would go dark while the sky behind it stayed a
            // bright daylight blue. Applied last so the sun disc and clouds dim too.
            float skySunElevation = clamp(sunDir.y, -1.0, 1.0);
            float skyTwilight = 1.0 - smoothstep(0.0, 0.35, skySunElevation);
            float skySunProximity = pow(max(dot(dir, sunDir), 0.0), 4.0);
            skyColor = mix(skyColor, vec3(1.0, 0.45, 0.2), skyTwilight * skySunProximity * 0.5);
            float skyNight = clamp(-skySunElevation * 2.5, 0.0, 1.0);
            skyColor = mix(skyColor, skyColor * 0.03 + vec3(0.012, 0.016, 0.038), skyNight);

            FragColor = vec4(skyColor, 0.0); // 0.0 alpha for SSR so sky isn't reflective
        }
    )";
    // Built through compile_shaders so it uses the on-disk program binary
    // cache; this is the expensive one to compile from source.
    sky_shader_program = compile_shaders(sky_vert, sky_frag);
    report_program_link(sky_shader_program, "sky");
    report_loading_progress("Compiling sky shaders");

    sky_forward_loc = glGetUniformLocation(sky_shader_program, "uCamForward");
    sky_right_loc = glGetUniformLocation(sky_shader_program, "uCamRight");
    sky_up_loc = glGetUniformLocation(sky_shader_program, "uCamUp");
    sky_fov_tan_loc = glGetUniformLocation(sky_shader_program, "uFovTan");
    sky_aspect_loc = glGetUniformLocation(sky_shader_program, "uAspect");
    sky_sun_dir_loc = glGetUniformLocation(sky_shader_program, "uSunDir");
    sky_time_loc = glGetUniformLocation(sky_shader_program, "uTime");
    sky_enable_3d_clouds_loc = glGetUniformLocation(sky_shader_program, "uEnable3DClouds");
    sky_mode_loc = glGetUniformLocation(sky_shader_program, "uSkyMode");
    sky_void_color_loc = glGetUniformLocation(sky_shader_program, "uVoidColor");
    sky_env_map_location = glGetUniformLocation(sky_shader_program, "uEnvironmentMap");
    sky_has_env_map_location = glGetUniformLocation(sky_shader_program, "uHasEnvironmentMap");

    setup_quad();
    init_occlusion_resources();
    init_particle_shader();
    init_terrain_shader();
    init_foliage_shaders();
    init_shadow_map();
    init_exposure_resources();
    // Timer-query objects for the per-pass profiler. Needs a live GL context, so it
    // cannot be done in the Renderer constructor.
    profiler.initialize();
    report_loading_progress("Initialising render targets");
    init_ssao_shaders();
    report_loading_progress();
    init_bloom_shaders();
    init_slr_shader();
    report_loading_progress();

    create_fbo(width, height);
    report_loading_progress();

    // Image-based lighting is on by default for every scene, not opt-in: load
    // the engine's bundled default environment map so ambient/reflections look
    // right out of the box. If it's missing (e.g. a build directory that
    // hasn't run the CMake copy step), sampleAmbientDiffuse/Specular already
    // fall back to the analytic sky, so this degrades gracefully either way.
    if (!load_environment_map("EngineContent/DefaultSky.hdr")) {
        std::cerr << "[Renderer] No default HDRI at EngineContent/DefaultSky.hdr - falling back to the analytic sky for ambient lighting." << std::endl;
    }

    // Deleted incorrect shadowMap rebinding
    return true;
}


// --- Visibility ------------------------------------------------------------

void Renderer::update_frustum_planes() {
    // Gribb-Hartmann: the rows of the combined matrix are already the clip-space
    // plane equations, so the six view planes are their sums and differences with
    // the w row. Because the model matrices are camera-relative, these come out in
    // camera-relative world space with the camera at the origin.
    const Matrix4x4 vp = projection_matrix * view_matrix;
    const std::array<float, 16>& m = vp.m;

    // Row i of a column-major matrix is (m[0*4+i], m[1*4+i], m[2*4+i], m[3*4+i]).
    const float row0[4] = { m[0], m[4], m[8],  m[12] };
    const float row1[4] = { m[1], m[5], m[9],  m[13] };
    const float row2[4] = { m[2], m[6], m[10], m[14] };
    const float row3[4] = { m[3], m[7], m[11], m[15] };

    auto set_plane = [this](int index, float a, float b, float c, float d) {
        const float length = std::sqrt(a * a + b * b + c * c);
        if (length < 1e-8f) {
            // A degenerate plane must not cull anything, so point it inward with a
            // distance nothing can fall behind.
            frustum_planes[index].normal = { 0.0f, 0.0f, 0.0f };
            frustum_planes[index].distance = 1e30f;
            return;
        }
        const float inv = 1.0f / length;
        frustum_planes[index].normal = { a * inv, b * inv, c * inv };
        frustum_planes[index].distance = d * inv;
    };

    set_plane(0, row3[0] + row0[0], row3[1] + row0[1], row3[2] + row0[2], row3[3] + row0[3]); // left
    set_plane(1, row3[0] - row0[0], row3[1] - row0[1], row3[2] - row0[2], row3[3] - row0[3]); // right
    set_plane(2, row3[0] + row1[0], row3[1] + row1[1], row3[2] + row1[2], row3[3] + row1[3]); // bottom
    set_plane(3, row3[0] - row1[0], row3[1] - row1[1], row3[2] - row1[2], row3[3] - row1[3]); // top
    set_plane(4, row3[0] + row2[0], row3[1] + row2[1], row3[2] + row2[2], row3[3] + row2[3]); // near
    set_plane(5, row3[0] - row2[0], row3[1] - row2[1], row3[2] - row2[2], row3[3] - row2[3]); // far
}

bool Renderer::is_inside_frustum(const Vector3& center_relative, float radius) const {
    if (!enable_frustum_culling) return true;
    // A negative radius means the caller could not work out the bounds - a mesh
    // still streaming in - and guessing would make it pop in late.
    if (radius < 0.0f) return true;

    for (const Plane& plane : frustum_planes) {
        const float signed_distance = Vector3::dot(plane.normal, center_relative) + plane.distance;
        if (signed_distance < -radius) return false;
    }
    return true;
}

void Renderer::init_occlusion_resources() {
    // Positions only, and no lighting: this geometry is never seen. The fragment
    // shader exists solely because a program needs one.
    const std::string vertex_src = R"(#version 450 core
layout (location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
    const std::string fragment_src = R"(#version 450 core
void main() {}
)";

    occlusion_program = compile_shaders(vertex_src, fragment_src);
    if (occlusion_program == 0) {
        std::cerr << "[Renderer] Occlusion query shader failed to compile; "
                     "occlusion culling disabled (rendering is unaffected)." << std::endl;
        enable_occlusion_culling = false;
        return;
    }
    occlusion_mvp_location = glGetUniformLocation(occlusion_program, "uMVP");

    // A cube spanning -0.5..0.5, so a box is drawn by scaling it to the object's
    // extents and translating it to the object's centre.
    const float vertices[] = {
        -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,   0.5f,  0.5f, -0.5f,  -0.5f,  0.5f, -0.5f,
        -0.5f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,   0.5f,  0.5f,  0.5f,  -0.5f,  0.5f,  0.5f
    };
    const unsigned int indices[] = {
        0, 1, 2,  2, 3, 0,   // -Z
        4, 5, 6,  6, 7, 4,   // +Z
        0, 4, 7,  7, 3, 0,   // -X
        1, 5, 6,  6, 2, 1,   // +X
        0, 1, 5,  5, 4, 0,   // -Y
        3, 2, 6,  6, 7, 3    // +Y
    };

    glGenVertexArrays(1, &occlusion_vao);
    glGenBuffers(1, &occlusion_vbo);
    glGenBuffers(1, &occlusion_ebo);

    glBindVertexArray(occlusion_vao);
    glBindBuffer(GL_ARRAY_BUFFER, occlusion_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, occlusion_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

void Renderer::begin_occlusion_pass() {
    culled_by_occlusion = 0;
    if (!enable_occlusion_culling || occlusion_program == 0 || occlusion_vao == 0) return;

    occlusion_pass_active = true;
    ++occlusion_frame;

    glUseProgram(occlusion_program);
    glBindVertexArray(occlusion_vao);
    // Nothing about this pass may alter the frame: no colour, no depth. It only
    // asks the depth test a question.
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
}

void Renderer::submit_occlusion_test(const void* key, const Matrix4x4& relative_model,
                                     const Vector3& local_min, const Vector3& local_max) {
    if (!occlusion_pass_active || key == nullptr) return;

    OcclusionRecord& record = occlusion_records[key];
    record.last_frame = occlusion_frame;

    // Harvest the previous query before reusing its object. Availability is checked
    // rather than the result being demanded: asking for a result that is not ready
    // blocks until the GPU finishes, which is the stall this design exists to avoid.
    if (record.query_pending && record.query != 0) {
        GLuint available = 0;
        glGetQueryObjectuiv(record.query, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) return; // still in flight; leave the last answer standing
        GLuint samples = 0;
        glGetQueryObjectuiv(record.query, GL_QUERY_RESULT, &samples);
        record.visible = (samples != 0);
        record.query_pending = false;
    }

    const Vector3 center = { (local_min.x + local_max.x) * 0.5f,
                             (local_min.y + local_max.y) * 0.5f,
                             (local_min.z + local_max.z) * 0.5f };
    // A minimum size keeps a flat object - a floor plane, a decal - from producing a
    // zero-thickness box that the depth test can miss entirely.
    const Vector3 extent = { std::max(1e-3f, local_max.x - local_min.x),
                             std::max(1e-3f, local_max.y - local_min.y),
                             std::max(1e-3f, local_max.z - local_min.z) };

    const Matrix4x4 box_model = relative_model *
                                Matrix4x4::translation(center) *
                                Matrix4x4::scale(extent);

    // If the camera is inside the box, its front faces are behind the near plane and
    // get clipped away - the query would report nothing drawn and the object would
    // disappear from around the viewer. The eight corners are transformed rather
    // than the centre alone because the box may be rotated and scaled.
    {
        Vector3 world_min = { 1e30f, 1e30f, 1e30f };
        Vector3 world_max = { -1e30f, -1e30f, -1e30f };
        for (int corner = 0; corner < 8; ++corner) {
            const Vector3 local = {
                (corner & 1) ? 0.5f : -0.5f,
                (corner & 2) ? 0.5f : -0.5f,
                (corner & 4) ? 0.5f : -0.5f
            };
            const Vector3 p = box_model * local;
            world_min.x = std::min(world_min.x, p.x); world_max.x = std::max(world_max.x, p.x);
            world_min.y = std::min(world_min.y, p.y); world_max.y = std::max(world_max.y, p.y);
            world_min.z = std::min(world_min.z, p.z); world_max.z = std::max(world_max.z, p.z);
        }
        // The camera sits at the origin in this space. The margin is comfortably
        // more than the 0.1 near plane the projection is built with.
        const float margin = 0.5f;
        if (world_min.x - margin <= 0.0f && world_max.x + margin >= 0.0f &&
            world_min.y - margin <= 0.0f && world_max.y + margin >= 0.0f &&
            world_min.z - margin <= 0.0f && world_max.z + margin >= 0.0f) {
            record.visible = true;
            return;
        }
    }

    if (record.query == 0) glGenQueries(1, &record.query);

    const Matrix4x4 mvp = projection_matrix * view_matrix * box_model;
    if (occlusion_mvp_location != -1) {
        glUniformMatrix4fv(occlusion_mvp_location, 1, GL_FALSE, mvp.m.data());
    }

    glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, record.query);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
    glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
    record.query_pending = true;
}

void Renderer::end_occlusion_pass() {
    if (!occlusion_pass_active) return;
    occlusion_pass_active = false;

    glBindVertexArray(0);
    glUseProgram(0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    // Records not submitted this frame belong to objects that were frustum-culled,
    // deleted, or made invisible. Leaving a stale "occluded" on one would keep it
    // hidden for as long as it stayed out of view and for the first frame it came
    // back, which reads as an object failing to reappear.
    for (auto it = occlusion_records.begin(); it != occlusion_records.end(); ) {
        if (it->second.last_frame == occlusion_frame) {
            ++it;
            continue;
        }
        it->second.visible = true;
        // Long-absent objects are gone for good; free the query object with them.
        if (occlusion_frame - it->second.last_frame > 240) {
            if (it->second.query != 0) glDeleteQueries(1, &it->second.query);
            it = occlusion_records.erase(it);
        } else {
            ++it;
        }
    }
}

bool Renderer::was_occluded(const void* key) const {
    if (!enable_occlusion_culling || key == nullptr) return false;
    auto it = occlusion_records.find(key);
    if (it == occlusion_records.end()) return false;
    return !it->second.visible;
}

void Renderer::reset_occlusion_state() {
    for (auto& entry : occlusion_records) {
        if (entry.second.query != 0) glDeleteQueries(1, &entry.second.query);
    }
    occlusion_records.clear();
}


// --- Terrain ---------------------------------------------------------------

void Renderer::init_terrain_shader() {
    // The ordinary mesh vertex layout, so the shadow depth pass draws terrain with
    // no special case. Only the fragment stage differs.
    const std::string vertex_src = R"(#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aColor;
layout (location = 2) in vec3 aNormal;
layout (location = 3) in vec2 aUV;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

uniform mat4 uMVP;
uniform mat4 uModel;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    FragPos = vec3(uModel * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(uModel))) * aNormal;
    TexCoord = aUV;
}
)";

    const std::string fragment_src = R"(#version 450 core
layout (location = 0) out vec4 gPosition;
layout (location = 1) out vec4 gNormal;
layout (location = 2) out vec4 gAlbedoSpec;
layout (location = 3) out vec4 gPBR;
layout (location = 4) out vec4 gBakedGI;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform vec3 uAmbientCube[6];
uniform sampler2D uSplatMap;
uniform sampler2D uLayer0;
uniform sampler2D uLayer1;
uniform sampler2D uLayer2;
uniform sampler2D uLayer3;
// How many times each layer repeats across the whole terrain. Terrain textures are
// tiled hard, or a 2048px image stretched over 200 metres is a smear.
uniform vec4 uLayerTiling;
// Bitmask of which layers resolved to a real texture. A layer without one falls
// back to a flat colour, so a terrain is legible before any texture is assigned.
uniform int uLayerPresent;
uniform float uMetallic;
uniform float uRoughness;
uniform bool uEnableUE4Lighting;

vec3 layer_colour(int index, sampler2D tex, vec3 fallback) {
    if ((uLayerPresent & (1 << index)) != 0) {
        return texture(tex, TexCoord * uLayerTiling[index]).rgb;
    }
    return fallback;
}

void main() {
    vec4 weights = texture(uSplatMap, TexCoord);
    float total = weights.r + weights.g + weights.b + weights.a;
    // An unpainted texel would otherwise come out black; the first layer is the
    // terrain's base coat.
    if (total < 0.001) { weights = vec4(1.0, 0.0, 0.0, 0.0); total = 1.0; }
    weights /= total;

    vec3 colour = vec3(0.0);
    colour += weights.r * layer_colour(0, uLayer0, vec3(0.29, 0.42, 0.20));  // grass
    colour += weights.g * layer_colour(1, uLayer1, vec3(0.42, 0.32, 0.21));  // dirt
    colour += weights.b * layer_colour(2, uLayer2, vec3(0.44, 0.44, 0.46));  // rock
    colour += weights.a * layer_colour(3, uLayer3, vec3(0.76, 0.70, 0.52));  // sand

    vec3 albedo = uEnableUE4Lighting ? pow(colour, vec3(2.2)) : colour;

    gPosition = vec4(FragPos, 0.0);
    gNormal = vec4(normalize(Normal), 0.0);
    gAlbedoSpec = vec4(albedo, 0.0);
    gPBR = vec4(uMetallic, max(uRoughness, 0.1), 0.0, 0.05);

    // Terrain is never lightmapped - it is generated geometry with no stable second
    // uv set - so it takes its indirect light from the probe grid like anything else
    // that moves.
    vec3 n = normalize(Normal);
    vec3 sq = n * n;
    vec3 baked  = (n.x >= 0.0 ? uAmbientCube[0] : uAmbientCube[1]) * sq.x;
         baked += (n.y >= 0.0 ? uAmbientCube[2] : uAmbientCube[3]) * sq.y;
         baked += (n.z >= 0.0 ? uAmbientCube[4] : uAmbientCube[5]) * sq.z;
    gBakedGI = vec4(baked, 1.0);
}
)";

    terrain_shader_program = compile_shaders(vertex_src, fragment_src);
    if (terrain_shader_program == 0) {
        std::cerr << "[Renderer] Terrain shader failed to compile; terrain will not draw."
                  << std::endl;
        return;
    }

    terrain_mvp_location = glGetUniformLocation(terrain_shader_program, "uMVP");
    terrain_model_location = glGetUniformLocation(terrain_shader_program, "uModel");
    terrain_splat_location = glGetUniformLocation(terrain_shader_program, "uSplatMap");
    terrain_layer_location[0] = glGetUniformLocation(terrain_shader_program, "uLayer0");
    terrain_layer_location[1] = glGetUniformLocation(terrain_shader_program, "uLayer1");
    terrain_layer_location[2] = glGetUniformLocation(terrain_shader_program, "uLayer2");
    terrain_layer_location[3] = glGetUniformLocation(terrain_shader_program, "uLayer3");
    terrain_tiling_location = glGetUniformLocation(terrain_shader_program, "uLayerTiling");
    terrain_layer_present_location = glGetUniformLocation(terrain_shader_program, "uLayerPresent");
    terrain_metallic_location = glGetUniformLocation(terrain_shader_program, "uMetallic");
    terrain_roughness_location = glGetUniformLocation(terrain_shader_program, "uRoughness");
    terrain_ue4_location = glGetUniformLocation(terrain_shader_program, "uEnableUE4Lighting");
    terrain_ambient_cube_location = glGetUniformLocation(terrain_shader_program, "uAmbientCube");
}

void Renderer::set_ambient_cube(const Vector3 cube[6]) {
    // The cube is uploaded with a single glUniform3fv over the whole array, which
    // requires the six Vector3s to be eighteen contiguous floats with no padding.
    static_assert(sizeof(Vector3) == 3 * sizeof(float),
                  "Vector3 must be tightly packed for the ambient cube upload");
    for (int i = 0; i < 6; ++i) current_ambient_cube[i] = cube[i];
}

void Renderer::render_terrain(const TerrainComponent& terrain, const Transform& transform) {
    if (terrain_shader_program == 0 || terrain.get_index_count() == 0) return;

    glUseProgram(terrain_shader_program);

    const Matrix4x4 model = transform.get_relative_matrix(camera_pos);
    const Matrix4x4 mvp = projection_matrix * view_matrix * model;
    if (terrain_mvp_location != -1) glUniformMatrix4fv(terrain_mvp_location, 1, GL_FALSE, mvp.m.data());
    if (terrain_model_location != -1) glUniformMatrix4fv(terrain_model_location, 1, GL_FALSE, model.m.data());

    // Texture units 8 and above, well clear of the ones the mesh path uses, so a
    // terrain draw cannot disturb a diffuse binding left over from a mesh.
    constexpr int kSplatUnit = 8;
    constexpr int kFirstLayerUnit = 9;
    bool layer_present[TerrainComponent::kLayerCount] = { false, false, false, false };
    terrain.bind_material(kSplatUnit, kFirstLayerUnit, layer_present);

    if (terrain_splat_location != -1) glUniform1i(terrain_splat_location, kSplatUnit);
    int present_mask = 0;
    for (int layer = 0; layer < TerrainComponent::kLayerCount; ++layer) {
        if (terrain_layer_location[layer] != -1) {
            glUniform1i(terrain_layer_location[layer], kFirstLayerUnit + layer);
        }
        if (layer_present[layer]) present_mask |= (1 << layer);
    }
    if (terrain_layer_present_location != -1) glUniform1i(terrain_layer_present_location, present_mask);

    if (terrain_tiling_location != -1) {
        glUniform4f(terrain_tiling_location, terrain.layer_tiling[0], terrain.layer_tiling[1],
                    terrain.layer_tiling[2], terrain.layer_tiling[3]);
    }
    if (terrain_metallic_location != -1) glUniform1f(terrain_metallic_location, terrain.metallic);
    if (terrain_roughness_location != -1) glUniform1f(terrain_roughness_location, terrain.roughness);
    if (terrain_ue4_location != -1) glUniform1i(terrain_ue4_location, enable_ue4_lighting ? 1 : 0);
    if (terrain_ambient_cube_location != -1) {
        glUniform3fv(terrain_ambient_cube_location, 6, &current_ambient_cube[0].x);
    }

    glPolygonMode(GL_FRONT_AND_BACK, wireframe_mode ? GL_LINE : GL_FILL);

    profiler.draw_calls++;
    profiler.triangles += static_cast<int>(terrain.get_index_count() / 3);
    terrain.render();

    if (wireframe_mode) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    // The geometry pass leaves its own program bound between draws, so put it back.
    glUseProgram(geometry_shader_program);
}

void Renderer::render_terrain_shadow(const TerrainComponent& terrain, const Transform& transform) {
    if (terrain.get_index_count() == 0) return;
    // The shadow pass has already bound the depth program and its light-space
    // matrix; only the model transform differs per caster.
    const Matrix4x4 model = transform.get_relative_matrix(camera_pos);
    glUniformMatrix4fv(depth_model_location, 1, GL_FALSE, model.m.data());
    apply_skinning_uniforms(depth_skinned_location, depth_bones_location, nullptr);
    profiler.draw_calls++;
    profiler.triangles += static_cast<int>(terrain.get_index_count() / 3);
    terrain.render();
}

// --- Instanced foliage -----------------------------------------------------

void Renderer::init_foliage_shaders() {
    // Locations 6 through 9 carry the four columns of a per-instance model matrix;
    // a mat4 vertex attribute occupies four consecutive slots and there is no way to
    // declare it as one.
    const std::string vertex_src = R"(#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aColor;
layout (location = 2) in vec3 aNormal;
layout (location = 3) in vec2 aUV;
layout (location = 6) in vec4 aInstance0;
layout (location = 7) in vec4 aInstance1;
layout (location = 8) in vec4 aInstance2;
layout (location = 9) in vec4 aInstance3;

out vec3 FragPos;
out vec3 Normal;
out vec3 ourColor;
out vec2 TexCoord;

uniform mat4 uViewProjection;
// Places the terrain the instances belong to. Kept out of the per-instance stream
// so the scatter can be uploaded once in terrain space and reused every frame,
// even though the camera-relative base changes as the camera moves.
uniform mat4 uBaseModel;

void main() {
    mat4 model = uBaseModel * mat4(aInstance0, aInstance1, aInstance2, aInstance3);
    vec4 world = model * vec4(aPos, 1.0);
    gl_Position = uViewProjection * world;
    FragPos = world.xyz;
    // Instances are only rotated, uniformly scaled and translated, so the upper 3x3
    // is already a similarity transform and normalising after it is exact - no
    // inverse-transpose needed.
    Normal = normalize(mat3(model) * aNormal);
    ourColor = aColor;
    TexCoord = aUV;
}
)";

    const std::string fragment_src = R"(#version 450 core
layout (location = 0) out vec4 gPosition;
layout (location = 1) out vec4 gNormal;
layout (location = 2) out vec4 gAlbedoSpec;
layout (location = 3) out vec4 gPBR;
layout (location = 4) out vec4 gBakedGI;

in vec3 FragPos;
in vec3 Normal;
in vec3 ourColor;
in vec2 TexCoord;

uniform vec3 uAmbientCube[6];
uniform float uMetallic;
uniform float uRoughness;
uniform bool uEnableUE4Lighting;
uniform sampler2D uDiffuseTexture;
uniform bool uHasDiffuseTexture;

void main() {
    vec3 objColor = ourColor;
    if (uHasDiffuseTexture) objColor *= texture(uDiffuseTexture, TexCoord).rgb;
    vec3 albedo = uEnableUE4Lighting ? pow(objColor, vec3(2.2)) : objColor;

    gPosition = vec4(FragPos, 0.0);
    gNormal = vec4(normalize(Normal), 0.0);
    gAlbedoSpec = vec4(albedo, 0.0);
    gPBR = vec4(uMetallic, max(uRoughness, 0.1), 0.0, 0.05);

    vec3 n = normalize(Normal);
    vec3 sq = n * n;
    vec3 baked  = (n.x >= 0.0 ? uAmbientCube[0] : uAmbientCube[1]) * sq.x;
         baked += (n.y >= 0.0 ? uAmbientCube[2] : uAmbientCube[3]) * sq.y;
         baked += (n.z >= 0.0 ? uAmbientCube[4] : uAmbientCube[5]) * sq.z;
    gBakedGI = vec4(baked, 1.0);
}
)";

    foliage_shader_program = compile_shaders(vertex_src, fragment_src);
    if (foliage_shader_program != 0) {
        foliage_view_projection_location = glGetUniformLocation(foliage_shader_program, "uViewProjection");
        foliage_base_model_location = glGetUniformLocation(foliage_shader_program, "uBaseModel");
        foliage_metallic_location = glGetUniformLocation(foliage_shader_program, "uMetallic");
        foliage_roughness_location = glGetUniformLocation(foliage_shader_program, "uRoughness");
        foliage_ue4_location = glGetUniformLocation(foliage_shader_program, "uEnableUE4Lighting");
        foliage_ambient_cube_location = glGetUniformLocation(foliage_shader_program, "uAmbientCube");
        foliage_texture_location = glGetUniformLocation(foliage_shader_program, "uDiffuseTexture");
        foliage_has_texture_location = glGetUniformLocation(foliage_shader_program, "uHasDiffuseTexture");
    } else {
        std::cerr << "[Renderer] Foliage shader failed to compile; scattered foliage "
                     "will not draw." << std::endl;
    }

    const std::string depth_vertex_src = R"(#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 6) in vec4 aInstance0;
layout (location = 7) in vec4 aInstance1;
layout (location = 8) in vec4 aInstance2;
layout (location = 9) in vec4 aInstance3;
uniform mat4 uViewProjection;
uniform mat4 uBaseModel;
void main() {
    mat4 model = uBaseModel * mat4(aInstance0, aInstance1, aInstance2, aInstance3);
    gl_Position = uViewProjection * model * vec4(aPos, 1.0);
}
)";
    const std::string depth_fragment_src = R"(#version 450 core
void main() {}
)";
    foliage_depth_program = compile_shaders(depth_vertex_src, depth_fragment_src);
    if (foliage_depth_program != 0) {
        foliage_depth_view_projection_location =
            glGetUniformLocation(foliage_depth_program, "uViewProjection");
        foliage_depth_base_model_location = glGetUniformLocation(foliage_depth_program, "uBaseModel");
    }

    glGenVertexArrays(1, &foliage_vao);
    glGenBuffers(1, &foliage_instance_vbo);
}

bool Renderer::bind_foliage_geometry(const TerrainComponent& terrain, const MeshResource& mesh) {
    foliage_instance_count = 0;
    if (foliage_vao == 0) return false;
    if (mesh.get_state() != ResourceState::LoadedGPU) return false;
    if (mesh.get_vbo() == 0 || mesh.get_ebo() == 0 || mesh.get_indices_count() == 0) return false;

    const std::vector<Matrix4x4>& instances = terrain.get_foliage_instances();
    if (instances.empty()) return false;

    glBindVertexArray(foliage_vao);
    glBindBuffer(GL_ARRAY_BUFFER, foliage_instance_vbo);

    // Re-uploaded only when the scatter actually changed. The instances are in the
    // terrain's own space and the placement is a uniform, so camera movement - which
    // happens every frame - does not invalidate them.
    const bool needs_upload = (foliage_uploaded_owner != &terrain) ||
                              (foliage_uploaded_version != terrain.get_foliage_version());
    if (needs_upload) {
        const size_t required = instances.size() * sizeof(Matrix4x4);
        if (required > foliage_instance_capacity) {
            // Grown, never shrunk: painting foliage repeatedly would otherwise
            // reallocate the buffer on every stroke.
            glBufferData(GL_ARRAY_BUFFER, required, instances.data(), GL_DYNAMIC_DRAW);
            foliage_instance_capacity = required;
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, 0, required, instances.data());
        }
        foliage_uploaded_owner = &terrain;
        foliage_uploaded_version = terrain.get_foliage_version();
    }

    for (int column = 0; column < 4; ++column) {
        const unsigned int location = 6 + column;
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, sizeof(Matrix4x4),
                              (void*)(static_cast<uintptr_t>(column) * 4 * sizeof(float)));
        // Advance once per instance rather than once per vertex, which is the whole
        // point of the divisor.
        glVertexAttribDivisor(location, 1);
    }

    glBindBuffer(GL_ARRAY_BUFFER, mesh.get_vbo());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.get_ebo());

    foliage_instance_count = static_cast<int>(instances.size());
    return true;
}

void Renderer::render_foliage(const TerrainComponent& terrain, const MeshResource& mesh,
                              const Transform& base) {
    if (foliage_shader_program == 0) return;
    if (!bind_foliage_geometry(terrain, mesh)) { glBindVertexArray(0); return; }

    glUseProgram(foliage_shader_program);
    const Matrix4x4 view_projection = projection_matrix * view_matrix;
    if (foliage_view_projection_location != -1) {
        glUniformMatrix4fv(foliage_view_projection_location, 1, GL_FALSE, view_projection.m.data());
    }
    if (foliage_base_model_location != -1) {
        const Matrix4x4 base_model = base.get_relative_matrix(camera_pos);
        glUniformMatrix4fv(foliage_base_model_location, 1, GL_FALSE, base_model.m.data());
    }
    if (foliage_metallic_location != -1) glUniform1f(foliage_metallic_location, 0.0f);
    if (foliage_roughness_location != -1) glUniform1f(foliage_roughness_location, 0.75f);
    if (foliage_ue4_location != -1) glUniform1i(foliage_ue4_location, enable_ue4_lighting ? 1 : 0);
    if (foliage_ambient_cube_location != -1) {
        glUniform3fv(foliage_ambient_cube_location, 6, &current_ambient_cube[0].x);
    }
    if (foliage_has_texture_location != -1) glUniform1i(foliage_has_texture_location, 0);

    profiler.draw_calls++;
    profiler.triangles += static_cast<int>(mesh.get_indices_count() / 3) * foliage_instance_count;
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(mesh.get_indices_count()),
                            GL_UNSIGNED_INT, 0, foliage_instance_count);

    glBindVertexArray(0);
    glUseProgram(geometry_shader_program);
}

void Renderer::render_foliage_shadow(const TerrainComponent& terrain, const MeshResource& mesh,
                                     const Transform& base) {
    if (foliage_depth_program == 0) return;
    if (!bind_foliage_geometry(terrain, mesh)) { glBindVertexArray(0); return; }

    glUseProgram(foliage_depth_program);
    if (foliage_depth_view_projection_location != -1) {
        glUniformMatrix4fv(foliage_depth_view_projection_location, 1, GL_FALSE,
                           light_space_matrix.m.data());
    }
    if (foliage_depth_base_model_location != -1) {
        const Matrix4x4 base_model = base.get_relative_matrix(camera_pos);
        glUniformMatrix4fv(foliage_depth_base_model_location, 1, GL_FALSE, base_model.m.data());
    }
    profiler.draw_calls++;
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(mesh.get_indices_count()),
                            GL_UNSIGNED_INT, 0, foliage_instance_count);

    glBindVertexArray(0);
    // The shadow pass expects its own program still bound for the casters after this.
    glUseProgram(depth_shader_program);
}

void Renderer::setup_quad() {
    float quadVertices[] = {
        // positions   // texCoords
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };
    glGenVertexArrays(1, &quad_vao);
    glGenBuffers(1, &quad_vbo);
    glBindVertexArray(quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
}

void Renderer::init_shadow_map() {
    glGenFramebuffers(1, &shadow_fbo);
    glGenTextures(1, &shadow_depth_map);
    glBindTexture(GL_TEXTURE_2D, shadow_depth_map);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 4096, 4096, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0, 1.0, 1.0, 1.0 };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_depth_map, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::init_particle_shader() {
    const std::string vertex_src = R"(#version 450 core
// The quad corner, in -0.5..0.5, and its uv. The same two triangles for every
// particle; everything that differs comes in through the instance stream.
layout (location = 0) in vec2 aCorner;
layout (location = 1) in vec2 aUV;
// position.xyz + size
layout (location = 6) in vec4 aInstancePosSize;
layout (location = 7) in vec4 aInstanceColor;
// rotation in radians
layout (location = 8) in vec4 aInstanceParams;

out vec2 TexCoord;
out vec4 ParticleColor;
out float ViewDepth;

uniform mat4 uViewProjection;
uniform mat4 uView;

void main() {
    // Billboard: the camera's right and up axes in world space are the first two
    // rows of the view matrix. Building the quad from them is what keeps a particle
    // facing the camera from every angle without a per-particle matrix.
    vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
    vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);

    float rotation = aInstanceParams.x;
    float c = cos(rotation);
    float s = sin(rotation);
    vec2 corner = vec2(aCorner.x * c - aCorner.y * s,
                       aCorner.x * s + aCorner.y * c);

    vec3 world = aInstancePosSize.xyz + (right * corner.x + up * corner.y) * aInstancePosSize.w;

    gl_Position = uViewProjection * vec4(world, 1.0);
    TexCoord = aUV;
    ParticleColor = aInstanceColor;
    // Positive distance in front of the camera, for the soft depth comparison.
    ViewDepth = -(uView * vec4(world, 1.0)).z;
}
)";

    const std::string fragment_src = R"(#version 450 core
out vec4 FragColor;

in vec2 TexCoord;
in vec4 ParticleColor;
in float ViewDepth;

uniform sampler2D uTexture;
uniform bool uHasTexture;
uniform sampler2D uSceneDepth;
uniform mat4 uInvProjection;
uniform vec2 uViewport;
// Distance over which a particle fades out as it approaches the surface behind it.
uniform float uSoftFade;

void main() {
    vec4 colour = ParticleColor;

    if (uHasTexture) {
        colour *= texture(uTexture, TexCoord);
    } else {
        // A soft round dot, so an emitter with no texture assigned looks like a
        // particle rather than like a square.
        float d = length(TexCoord - vec2(0.5)) * 2.0;
        colour.a *= smoothstep(1.0, 0.0, d);
    }

    // Depth comes from the G-buffer rather than from a depth test, because this pass
    // draws into the resolve target whose depth attachment is not the scene's. The
    // same comparison gives both occlusion and the soft intersection fade: a
    // particle behind geometry lands at zero and disappears.
    vec2 screen_uv = gl_FragCoord.xy / uViewport;
    float scene_depth = texture(uSceneDepth, screen_uv).r;
    vec4 clip = vec4(screen_uv * 2.0 - 1.0, scene_depth * 2.0 - 1.0, 1.0);
    vec4 view_position = uInvProjection * clip;
    float scene_view_depth = -(view_position.z / view_position.w);

    float fade = clamp((scene_view_depth - ViewDepth) / max(0.001, uSoftFade), 0.0, 1.0);
    colour.a *= fade;

    if (colour.a <= 0.001) discard;

    // Premultiplied by alpha so one blend equation serves both modes: additive is
    // ONE/ONE and alpha is ONE/ONE_MINUS_SRC_ALPHA, and neither needs the shader to
    // know which is in use.
    FragColor = vec4(colour.rgb * colour.a, colour.a);
}
)";

    particle_shader_program = compile_shaders(vertex_src, fragment_src);
    if (particle_shader_program == 0) {
        std::cerr << "[Renderer] Particle shader failed to compile; particles will not draw."
                  << std::endl;
        return;
    }

    particle_view_projection_location = glGetUniformLocation(particle_shader_program, "uViewProjection");
    particle_view_location = glGetUniformLocation(particle_shader_program, "uView");
    particle_inv_projection_location = glGetUniformLocation(particle_shader_program, "uInvProjection");
    particle_scene_depth_location = glGetUniformLocation(particle_shader_program, "uSceneDepth");
    particle_texture_location = glGetUniformLocation(particle_shader_program, "uTexture");
    particle_has_texture_location = glGetUniformLocation(particle_shader_program, "uHasTexture");
    particle_soft_fade_location = glGetUniformLocation(particle_shader_program, "uSoftFade");
    particle_viewport_location = glGetUniformLocation(particle_shader_program, "uViewport");

    // Two triangles spanning -0.5..0.5, with uvs. Its own VAO rather than the
    // fullscreen quad's, because this one carries per-instance attributes and
    // leaving those enabled on the fullscreen quad would corrupt every post pass.
    const float quad[] = {
        -0.5f, -0.5f, 0.0f, 0.0f,
         0.5f, -0.5f, 1.0f, 0.0f,
         0.5f,  0.5f, 1.0f, 1.0f,
        -0.5f, -0.5f, 0.0f, 0.0f,
         0.5f,  0.5f, 1.0f, 1.0f,
        -0.5f,  0.5f, 0.0f, 1.0f
    };

    glGenVertexArrays(1, &particle_vao);
    glGenBuffers(1, &particle_quad_vbo);
    glGenBuffers(1, &particle_instance_vbo);

    glBindVertexArray(particle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, particle_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, particle_instance_vbo);
    const GLsizei stride = sizeof(ParticleInstance);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ParticleInstance, position));
    glVertexAttribDivisor(6, 1);
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ParticleInstance, color));
    glVertexAttribDivisor(7, 1);
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(ParticleInstance, rotation));
    glVertexAttribDivisor(8, 1);

    glBindVertexArray(0);
}

void Renderer::render_particles(const std::vector<ParticleInstance>& instances, int blend_mode,
                                const std::string& texture_path) {
    if (instances.empty() || particle_shader_program == 0 || particle_vao == 0) return;

    profiler.draw_calls++;
    profiler.triangles += static_cast<int>(instances.size()) * 2;

    glUseProgram(particle_shader_program);
    glBindVertexArray(particle_vao);

    glBindBuffer(GL_ARRAY_BUFFER, particle_instance_vbo);
    const size_t required = instances.size() * sizeof(ParticleInstance);
    if (required > particle_instance_capacity) {
        glBufferData(GL_ARRAY_BUFFER, required, instances.data(), GL_DYNAMIC_DRAW);
        particle_instance_capacity = required;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, required, instances.data());
    }

    const Matrix4x4 view_projection = projection_matrix * view_matrix;
    if (particle_view_projection_location != -1) {
        glUniformMatrix4fv(particle_view_projection_location, 1, GL_FALSE, view_projection.m.data());
    }
    if (particle_view_location != -1) {
        glUniformMatrix4fv(particle_view_location, 1, GL_FALSE, view_matrix.m.data());
    }
    if (particle_inv_projection_location != -1) {
        const Matrix4x4 inverse_projection = projection_matrix.inverse();
        glUniformMatrix4fv(particle_inv_projection_location, 1, GL_FALSE, inverse_projection.m.data());
    }
    if (particle_viewport_location != -1) {
        glUniform2f(particle_viewport_location, static_cast<float>(fbo_width), static_cast<float>(fbo_height));
    }
    if (particle_soft_fade_location != -1) glUniform1f(particle_soft_fade_location, 0.5f);

    // Unit 0 is the scene depth, unit 1 the optional sprite. The G-buffer depth is
    // sampled rather than the resolve target's own attachment: reading a texture
    // that is attached to the bound framebuffer is undefined.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gDepth);
    if (particle_scene_depth_location != -1) glUniform1i(particle_scene_depth_location, 0);

    bool has_texture = false;
    if (!texture_path.empty()) {
        auto texture = ResourceManager::get().load_async<TextureResource>(texture_path);
        if (texture && texture->get_state() == ResourceState::LoadedGPU && texture->get_texture_id() != 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texture->get_texture_id());
            if (particle_texture_location != -1) glUniform1i(particle_texture_location, 1);
            has_texture = true;
        }
    }
    if (particle_has_texture_location != -1) glUniform1i(particle_has_texture_location, has_texture ? 1 : 0);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    // Colour is premultiplied in the shader, so additive and alpha differ only in
    // what they do to the destination.
    if (blend_mode == 1) {
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    } else {
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    }

    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(instances.size()));

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glUseProgram(0);
}


void Renderer::begin_shadow_pass() {
    Vector3 light_direction = {0.5f, -1.0f, 0.5f};
    for (auto* l : lights) {
        if (auto dir_l = dynamic_cast<DirectionalLightComponent*>(l)) {
            light_direction = dir_l->get_direction();
            break;
        }
    }

    // The shadow map used to cover a 500x500 world-unit box. At 4096 texels that is
    // ~0.12 units per texel, so anything at human/prop scale had its shadow resolved
    // by a handful of texels and came out as a soft blob no amount of filtering could
    // rescue. Covering a much smaller region around the camera puts the same 4096
    // texels where they are actually visible.
    const float shadow_map_resolution = 4096.0f;
    float near_plane = -300.0f, far_plane = 300.0f;
    float size = 40.0f;
    shadow_depth_range = far_plane - near_plane;
    shadow_texel_world_size = (2.0f * size) / shadow_map_resolution;
    Matrix4x4 lightProjection = Matrix4x4::orthographic(-size, size, -size, size, near_plane, far_plane);

    // The camera is the origin of camera-relative space, so the light volume is
    // centred on the origin here rather than on the absolute camera position.
    Vector3 target = { 0.0f, 0.0f, 0.0f };
    Vector3 lightPos = { -light_direction.x * 150.0f, -light_direction.y * 150.0f, -light_direction.z * 150.0f };
    Matrix4x4 lightView = Matrix4x4::look_at(lightPos, target, {0,1,0});
    light_space_matrix = lightProjection * lightView;

    // Stabilisation: snap the light-space origin to whole shadow-map texels. Without
    // this the map slides by sub-texel amounts as the camera moves, and every shadow
    // edge crawls and shimmers because each frame quantises the depth test differently.
    Vector3 shadow_origin = light_space_matrix * Vector3{0.0f, 0.0f, 0.0f};
    float origin_x = shadow_origin.x * shadow_map_resolution * 0.5f;
    float origin_y = shadow_origin.y * shadow_map_resolution * 0.5f;
    float offset_x = (std::round(origin_x) - origin_x) * 2.0f / shadow_map_resolution;
    float offset_y = (std::round(origin_y) - origin_y) * 2.0f / shadow_map_resolution;
    Matrix4x4 snap = Matrix4x4::identity();
    snap.m[12] = offset_x;
    snap.m[13] = offset_y;
    light_space_matrix = snap * light_space_matrix;

    glViewport(0, 0, 4096, 4096);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(depth_shader_program);
    glUniformMatrix4fv(depth_light_space_location, 1, GL_FALSE, light_space_matrix.m.data());
    
    // Enable front face culling for shadow map generation to prevent peter-panning and self-shadowing artifacts
    glCullFace(GL_FRONT);
}

void Renderer::end_shadow_pass() {
    glCullFace(GL_BACK); // Restore default back-face culling
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}

bool Renderer::apply_skinning_uniforms(int skinned_uniform, int bones_uniform,
                                        const std::vector<Matrix4x4>* bone_matrices) {
    // Matrix4x4 is a bare std::array<float, 16>, so a vector of them is already the
    // tightly packed, column-major block glUniformMatrix4fv wants. If that ever
    // stops being true the palette would upload as garbage rather than fail to
    // compile, so pin it here.
    static_assert(sizeof(Matrix4x4) == 16 * sizeof(float),
                  "Matrix4x4 must be tightly packed to upload as a bone palette");

    const bool skinned = bone_matrices != nullptr && !bone_matrices->empty();
    if (skinned_uniform != -1) {
        glUniform1i(skinned_uniform, skinned ? 1 : 0);
    }
    if (skinned && bones_uniform != -1) {
        // Clamped rather than trusted: the importer caps skeletons at kMaxBones, but
        // a hand-edited or future-version asset must not write past the array.
        GLsizei count = static_cast<GLsizei>(std::min<size_t>(bone_matrices->size(), kMaxBones));
        glUniformMatrix4fv(bones_uniform, count, GL_FALSE,
                           reinterpret_cast<const float*>(bone_matrices->data()));
    }
    return skinned;
}

void Renderer::render_mesh_shadow(const StaticMeshComponent& mesh_component, const Transform& transform,
                                   const std::vector<Matrix4x4>* bone_matrices,
                                   const MeshResource* lod_mesh) {
    // Camera-relative (LWC), matching the geometry pass. The shadow map used to be
    // rendered in absolute world space while the G-buffer stored camera-relative
    // positions, so the depth comparison was off by exactly the camera position:
    // shadows slid across the ground as the camera moved instead of staying locked
    // to the objects casting them.
    Matrix4x4 model = transform.get_relative_matrix(camera_pos);
    glUniformMatrix4fv(depth_model_location, 1, GL_FALSE, model.m.data());
    apply_skinning_uniforms(depth_skinned_location, depth_bones_location, bone_matrices);
    profiler.draw_calls++;
    profiler.triangles += static_cast<int>(mesh_component.get_indices_count_internal() / 3);
    // The caster has to be the same mesh as the receiver, or a distant object's
    // shadow stays at full detail while the object itself drops to a coarse LOD.
    mesh_component.render(lod_mesh);
}

// Builds the SSAO/SSGI programs. Kept out of initialize() only for readability;
// called from there alongside the other post-process shader setup.
void Renderer::init_ssao_shaders() {
    const std::string ssao_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";

    // Outputs vec4(bounce colour, ambient occlusion).
    const std::string ssao_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D uDepthMap;
        uniform sampler2D uSceneColor;
        uniform sampler2D uNormalMap;
        uniform mat4 uProj;
        uniform mat4 uInvProj;
        uniform mat4 uView;
        uniform float uRadius;
        uniform int uFrameIndex;
        // 0 = GI off. Ambient occlusion is deliberately unaffected: it is a visibility
        // term, not bounce light, and switching it off with GI would flatten contact
        // shadows for no reason.
        uniform int uGIMode;

        vec3 reconstructPosition(vec2 uv, float z) {
            vec4 pos_s = vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, z * 2.0 - 1.0, 1.0);
            vec4 pos_v = uInvProj * pos_s;
            return pos_v.xyz / pos_v.w;
        }

        // Interleaved gradient noise (Jimenez). Its samples are far more evenly
        // distributed across a neighbourhood than a plain sin/fract hash, so what
        // noise remains is high frequency and disappears under the blur pass
        // instead of forming the coarse blotches a white-noise rotation leaves.
        float interleavedGradientNoise(vec2 p) {
            return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
        }

        void main() {
            float depth = texture(uDepthMap, TexCoords).r;
            if (depth >= 0.9999) {          // sky: fully unoccluded, no bounce
                FragColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }

            vec3 viewPos = reconstructPosition(TexCoords, depth);
            // Use the G-buffer normal rather than deriving one from depth
            // derivatives: the derivative version is constant across each 2x2 quad,
            // which quantises the hemisphere orientation and shows up as blocky AO
            // on curved surfaces.
            vec3 worldNormal = texture(uNormalMap, TexCoords).rgb;
            if (dot(worldNormal, worldNormal) < 0.01) {
                FragColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }
            vec3 normal = normalize(mat3(uView) * worldNormal);

            float rotation = interleavedGradientNoise(gl_FragCoord.xy + float(uFrameIndex) * 5.588238) * 6.2831853;
            float cr = cos(rotation), sr = sin(rotation);

            const int samples = 48;
            const float goldenAngle = 2.39996323;
            float ao = 0.0;
            vec3 ssgi = vec3(0.0);

            for (int i = 0; i < samples; ++i) {
                float theta = float(i) * goldenAngle;
                float r = sqrt((float(i) + 0.5) / float(samples));
                vec2 disk = vec2(cos(theta), sin(theta)) * r;
                // Rotate the whole disk per pixel so the fixed spiral does not band.
                disk = vec2(disk.x * cr - disk.y * sr, disk.x * sr + disk.y * cr);

                vec3 sampleDir = normalize(normal + vec3(disk, sqrt(max(1.0 - dot(disk, disk), 0.0))));
                if (dot(sampleDir, normal) < 0.0) sampleDir = -sampleDir;

                vec3 samplePos = viewPos + sampleDir * uRadius * r;
                vec4 offset = uProj * vec4(samplePos, 1.0);
                offset.xyz /= offset.w;
                offset.xyz = offset.xyz * 0.5 + 0.5;
                if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0) continue;

                float sampleDepth = texture(uDepthMap, offset.xy).r;
                if (sampleDepth >= 0.9999) continue;    // sky occludes nothing
                vec3 sampleViewPos = reconstructPosition(offset.xy, sampleDepth);

                float depthDelta = abs(viewPos.z - sampleViewPos.z);
                float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(depthDelta, 0.0001));

                if (sampleViewPos.z > samplePos.z + 0.02) {
                    ao += rangeCheck;
                } else {
                    vec3 toSample = sampleViewPos - viewPos;
                    float d = length(toSample);
                    if (d > 0.0001) {
                        // Clamp the bounce sample. This is a stochastic estimator, so
                        // one very bright source in the scene (an emissive bulb) gets
                        // picked up by a handful of the sample rays and missed by the
                        // rest, which shows up as bright speckle scattered over nearby
                        // surfaces rather than as smooth bounced light.
                        vec3 bounce = min(texture(uSceneColor, offset.xy).rgb, vec3(2.0));
                        ssgi += bounce * max(0.0, dot(normal, toSample / d)) * rangeCheck;
                    }
                }
            }

            ao = 1.0 - (ao / float(samples));
            ssgi /= float(samples);
            if (uGIMode == 0) ssgi = vec3(0.0);
            FragColor = vec4(ssgi, ao);
        }
    )";

    // Depth-aware box blur. A plain blur bleeds occlusion across silhouettes and
    // produces halos, so taps on a very different depth are rejected.
    const std::string ssao_blur_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D uSSAO;
        uniform sampler2D uDepthMap;
        uniform vec2 uTexelStep;

        void main() {
            float centerDepth = texture(uDepthMap, TexCoords).r;
            vec4 sum = vec4(0.0);
            float weight = 0.0;
            for (int x = -2; x <= 2; ++x) {
                for (int y = -2; y <= 2; ++y) {
                    vec2 uv = TexCoords + vec2(x, y) * uTexelStep;
                    float d = texture(uDepthMap, uv).r;
                    float w = (abs(d - centerDepth) < 0.0015) ? 1.0 : 0.0;
                    sum += texture(uSSAO, uv) * w;
                    weight += w;
                }
            }
            FragColor = weight > 0.0 ? sum / weight : texture(uSSAO, TexCoords);
        }
    )";

    ssao_shader_program = compile_shaders(ssao_vert, ssao_frag);
    ssao_blur_shader_program = compile_shaders(ssao_vert, ssao_blur_frag);
    report_program_link(ssao_shader_program, "ssao");
    report_program_link(ssao_blur_shader_program, "ssao_blur");
}

void Renderer::init_slr_shader() {
    const std::string slr_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";

    // Screen-space raymarch of an authored box-shaped participating-media volume.
    // Drawing a fullscreen quad rather than the volume's own geometry keeps the pass
    // correct when the camera is inside the beam, which is exactly when a rasterised
    // volume mesh would be culled away and the effect would vanish.
    const std::string slr_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;

        uniform sampler2D uSceneDepth;
        uniform mat4 uInvProj;
        uniform mat4 uView;        // rotation-only under large-world-coordinates
        uniform mat4 uInvModel;    // camera-relative world -> volume local
        uniform vec3 uColor;
        uniform vec3 uBeamDir;     // direction the light travels (camera-relative world)
        uniform int uShape;        // 0 = box, 1 = cone
        uniform float uAlpha;
        uniform float uSharpness;
        uniform float uIntensity;
        uniform float uFalloff;
        uniform float uCore;
        uniform float uFarDistance;

        vec3 reconstructViewPos(vec2 uv, float z) {
            vec4 clip = vec4(uv * 2.0 - 1.0, z * 2.0 - 1.0, 1.0);
            vec4 v = uInvProj * clip;
            return v.xyz / v.w;
        }

        // Signed distance to a unit cube centred on the origin (half-extent 0.5),
        // negative inside. An exact box SDF is what gives the volume genuinely square
        // corners at high sharpness instead of a rounded blob.
        float boxSDF(vec3 p) {
            vec3 q = abs(p) - vec3(0.5);
            return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
        }

        // Cone with its apex at local +Y, widening to the full radius at the base.
        // This is the shape a spotlight actually throws, so a cone volume reads as a
        // beam from a fixture rather than as a floating slab of light.
        float coneSDF(vec3 p) {
            float h = clamp(0.5 - p.y, 0.0, 1.0);     // 0 at apex, 1 at base
            float radius = h * 0.5;
            float radial = length(p.xz) - radius;
            float axial = max(p.y - 0.5, -0.5 - p.y); // clip to the unit slab
            return max(radial, axial);
        }

        float volumeSDF(vec3 p, int shape) {
            return (shape == 1) ? coneSDF(p) : boxSDF(p);
        }

        // Normalised distance from the beam axis, so the bright core works for a cone
        // (whose radius varies along its length) as well as for a box.
        float axisDistance(vec3 p, int shape) {
            if (shape == 1) {
                float h = clamp(0.5 - p.y, 0.0, 1.0);
                float radius = max(h * 0.5, 0.0001);
                return clamp(length(p.xz) / radius, 0.0, 1.0);
            }
            return clamp(length(p.xz) * 2.0, 0.0, 1.0);
        }

        void main() {
            float depth = texture(uSceneDepth, TexCoords).r;
            vec3 viewPos = reconstructViewPos(TexCoords, depth);
            // Scene geometry occludes the beam; sky lets it march to the far bound.
            float sceneDist = (depth >= 0.9999) ? uFarDistance : length(viewPos);

            vec3 rayDirView = normalize(viewPos);
            // The view matrix is a pure rotation here, so its inverse is its transpose.
            vec3 rayDir = transpose(mat3(uView)) * rayDirView;

            // Intersect the ray with the volume and march only the span that is
            // actually inside it. Deriving the step size from the whole scene depth
            // instead meant a 4-unit beam was sampled with ~10-unit steps: almost
            // every step fell outside the volume, and the one that landed inside
            // contributed a full step length of density, so brightness depended on
            // how far away the background happened to be rather than on the beam.
            //
            // localPos(t) = uInvModel * (origin + rayDir * t), so building the local
            // ray this way keeps t in world units and the slab test returns world
            // distances directly - no correction needed for non-uniform scale.
            vec3 roLocal = (uInvModel * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
            vec3 rdLocal = mat3(uInvModel) * rayDir;

            // Guard against axis-aligned rays: an exactly zero component would make
            // the slab division non-finite.
            vec3 safeRd = vec3(
                abs(rdLocal.x) < 1e-6 ? 1e-6 : rdLocal.x,
                abs(rdLocal.y) < 1e-6 ? 1e-6 : rdLocal.y,
                abs(rdLocal.z) < 1e-6 ? 1e-6 : rdLocal.z);
            vec3 invD = 1.0 / safeRd;
            vec3 tA = (vec3(-0.5) - roLocal) * invD;
            vec3 tB = (vec3( 0.5) - roLocal) * invD;
            vec3 tSmall = min(tA, tB);
            vec3 tBig = max(tA, tB);
            float tEnter = max(max(tSmall.x, tSmall.y), tSmall.z);
            float tExit  = min(min(tBig.x, tBig.y), tBig.z);

            tEnter = max(tEnter, 0.0);
            tExit = min(tExit, min(sceneDist, uFarDistance));
            if (tExit <= tEnter) discard;   // ray misses the volume entirely

            const int STEPS = 48;
            float span = tExit - tEnter;
            float stepLen = span / float(STEPS);
            float maxDist = tExit;

            // Sharpness sets the width of the density transition across the volume
            // boundary. Near 1 it collapses to a hard cut - crisp edges and corners,
            // no bleed. Near 0 it widens symmetrically about the surface so the beam
            // dissolves gradually into the surrounding darkness.
            float edgeWidth = mix(0.45, 0.0015, clamp(uSharpness, 0.0, 1.0));

            // Jitter the first sample so the finite step count breaks up into fine
            // noise rather than visible slabs banding across the beam.
            float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
            float t = tEnter + stepLen * jitter;

            float accum = 0.0;
            for (int i = 0; i < STEPS; ++i) {
                if (t > maxDist) break;
                // Camera sits at the origin of camera-relative space.
                vec3 worldPos = rayDir * t;
                vec3 localPos = (uInvModel * vec4(worldPos, 1.0)).xyz;

                // Shape: the authored box, with sharpness controlling the edge.
                float d = volumeSDF(localPos, uShape);
                float shape = 1.0 - smoothstep(-edgeWidth, edgeWidth, d);

                // A uniform slab of density reads as fog, not as light. Real shafts
                // are brightest at the emitter and along their axis, so energy is
                // concentrated both ways here.
                //
                // Axial: 0 at the emitting end (local +Y) rising to 1 at the far end;
                // brightness decays exponentially with distance travelled.
                float axial = clamp(0.5 - localPos.y, 0.0, 1.0);
                float lengthAtten = exp(-axial * uFalloff);

                // Radial: squeeze energy toward the beam's centre line so it has a
                // hot core and dimmer flanks instead of being flat across its width.
                float radial = axisDistance(localPos, uShape);
                float coreProfile = mix(1.0, exp(-radial * radial * 5.0), clamp(uCore, 0.0, 1.0));

                accum += shape * lengthAtten * coreProfile * stepLen;
                t += stepLen;
            }

            // Henyey-Greenstein phase function: forward-scattering media glow far more
            // brightly when you look along the light's travel direction. Without it the
            // volume looks identical from every angle, which is precisely why it read
            // as fog rather than as a beam of light.
            const float PI = 3.14159265359;
            float g = 0.72;
            float cosTheta = dot(normalize(rayDir), normalize(uBeamDir));
            float denom = 1.0 + g * g - 2.0 * g * cosTheta;
            float phase = (1.0 - g * g) / (4.0 * PI * pow(max(denom, 0.0001), 1.5));
            // Keep a floor so the beam stays visible from the side, just far dimmer.
            accum *= (0.35 + 2.6 * phase);

            // Tint by the authored colour directly so the beam matches the picked
            // value exactly. Alpha is written as 0 and the blend leaves the
            // destination alpha alone: the resolve target's alpha channel carries
            // material smoothness for the SSR stage and must not be trampled.
            FragColor = vec4(uColor * accum * uIntensity * uAlpha, 0.0);
        }
    )";

    slr_shader_program = compile_shaders(slr_vert, slr_frag);
    report_program_link(slr_shader_program, "slr_volume");
}

void Renderer::render_slr_volumes(const std::vector<SLRVolumeInstance>& volumes) {
    if (volumes.empty() || slr_shader_program == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    // Additive in colour, untouched in alpha (alpha holds SSR smoothness).
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);

    glUseProgram(slr_shader_program);

    glActiveTexture(GL_TEXTURE0);
    // Sample the gBuffer depth, not the resolve target's own depth attachment:
    // reading a texture that is simultaneously attached to the bound framebuffer is
    // undefined.
    glBindTexture(GL_TEXTURE_2D, gDepth);
    glUniform1i(glGetUniformLocation(slr_shader_program, "uSceneDepth"), 0);

    Matrix4x4 invProj = projection_matrix.inverse();
    glUniformMatrix4fv(glGetUniformLocation(slr_shader_program, "uInvProj"), 1, GL_FALSE, invProj.m.data());
    glUniformMatrix4fv(glGetUniformLocation(slr_shader_program, "uView"), 1, GL_FALSE, view_matrix.m.data());
    glUniform1f(glGetUniformLocation(slr_shader_program, "uFarDistance"), 500.0f);

    int inv_model_loc = glGetUniformLocation(slr_shader_program, "uInvModel");
    int color_loc = glGetUniformLocation(slr_shader_program, "uColor");
    int beam_dir_loc = glGetUniformLocation(slr_shader_program, "uBeamDir");
    int alpha_loc = glGetUniformLocation(slr_shader_program, "uAlpha");
    int sharpness_loc = glGetUniformLocation(slr_shader_program, "uSharpness");
    int intensity_loc = glGetUniformLocation(slr_shader_program, "uIntensity");
    int falloff_loc = glGetUniformLocation(slr_shader_program, "uFalloff");
    int core_loc = glGetUniformLocation(slr_shader_program, "uCore");
    int shape_loc = glGetUniformLocation(slr_shader_program, "uShape");

    glBindVertexArray(quad_vao);
    for (const SLRVolumeInstance& v : volumes) {
        glUniformMatrix4fv(inv_model_loc, 1, GL_FALSE, v.inv_model.m.data());
        glUniform3f(color_loc, v.color.x, v.color.y, v.color.z);
        glUniform3f(beam_dir_loc, v.beam_dir.x, v.beam_dir.y, v.beam_dir.z);
        glUniform1f(alpha_loc, v.alpha);
        glUniform1f(sharpness_loc, v.sharpness);
        glUniform1f(intensity_loc, v.intensity);
        glUniform1f(falloff_loc, v.falloff);
        glUniform1f(core_loc, v.core);
        glUniform1i(shape_loc, v.shape);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::init_bloom_shaders() {
    const std::string quad_vert = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        out vec2 TexCoords;
        void main() {
            TexCoords = aTexCoords;
            gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0);
        }
    )";

    // Prefilter: isolate the energy above the bloom threshold, with a soft knee so
    // the effect fades in around the threshold rather than switching on at a hard
    // edge (which bands across smooth gradients). Karis average weighting is applied
    // here to stop a single ultra-bright pixel from dominating the whole mip chain.
    const std::string prefilter_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D uSource;
        uniform vec2 uTexelStep;
        uniform float uThreshold;
        uniform float uKnee;

        float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

        vec3 prefilter(vec3 c) {
            float l = luma(c);
            float soft = clamp(l - uThreshold + uKnee, 0.0, 2.0 * uKnee);
            soft = (soft * soft) / (4.0 * uKnee + 0.0001);
            float contribution = max(soft, l - uThreshold) / max(l, 0.0001);
            return c * contribution;
        }

        void main() {
            // 4-tap box with Karis average: weight each tap by 1/(1+luma) so a lone
            // firefly cannot blow out the entire bloom pyramid.
            vec3 a = texture(uSource, TexCoords + vec2(-1.0, -1.0) * uTexelStep).rgb;
            vec3 b = texture(uSource, TexCoords + vec2( 1.0, -1.0) * uTexelStep).rgb;
            vec3 c = texture(uSource, TexCoords + vec2(-1.0,  1.0) * uTexelStep).rgb;
            vec3 d = texture(uSource, TexCoords + vec2( 1.0,  1.0) * uTexelStep).rgb;
            float wa = 1.0 / (1.0 + luma(a));
            float wb = 1.0 / (1.0 + luma(b));
            float wc = 1.0 / (1.0 + luma(c));
            float wd = 1.0 / (1.0 + luma(d));
            vec3 avg = (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 0.0001);
            FragColor = vec4(prefilter(avg), 1.0);
        }
    )";

    // 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of Duty:
    // Advanced Warfare"). Wider than a box filter, so the pyramid stays stable and
    // does not shimmer as detail drops out between levels.
    const std::string down_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D uSource;
        uniform vec2 uTexelStep;

        void main() {
            vec2 t = uTexelStep;
            vec3 a = texture(uSource, TexCoords + vec2(-2.0 * t.x,  2.0 * t.y)).rgb;
            vec3 b = texture(uSource, TexCoords + vec2( 0.0,        2.0 * t.y)).rgb;
            vec3 c = texture(uSource, TexCoords + vec2( 2.0 * t.x,  2.0 * t.y)).rgb;
            vec3 d = texture(uSource, TexCoords + vec2(-2.0 * t.x,  0.0)).rgb;
            vec3 e = texture(uSource, TexCoords).rgb;
            vec3 f = texture(uSource, TexCoords + vec2( 2.0 * t.x,  0.0)).rgb;
            vec3 g = texture(uSource, TexCoords + vec2(-2.0 * t.x, -2.0 * t.y)).rgb;
            vec3 h = texture(uSource, TexCoords + vec2( 0.0,       -2.0 * t.y)).rgb;
            vec3 i = texture(uSource, TexCoords + vec2( 2.0 * t.x, -2.0 * t.y)).rgb;
            vec3 j = texture(uSource, TexCoords + vec2(-t.x,  t.y)).rgb;
            vec3 k = texture(uSource, TexCoords + vec2( t.x,  t.y)).rgb;
            vec3 l = texture(uSource, TexCoords + vec2(-t.x, -t.y)).rgb;
            vec3 m = texture(uSource, TexCoords + vec2( t.x, -t.y)).rgb;

            vec3 result = e * 0.125;
            result += (a + c + g + i) * 0.03125;
            result += (b + d + f + h) * 0.0625;
            result += (j + k + l + m) * 0.125;
            FragColor = vec4(result, 1.0);
        }
    )";

    // 3x3 tent filter upsample, blended additively into the next larger level.
    const std::string up_frag = R"(
        #version 450 core
        out vec4 FragColor;
        in vec2 TexCoords;
        uniform sampler2D uSource;
        uniform vec2 uTexelStep;
        uniform float uRadius;

        void main() {
            vec2 t = uTexelStep * uRadius;
            vec3 a = texture(uSource, TexCoords + vec2(-t.x,  t.y)).rgb;
            vec3 b = texture(uSource, TexCoords + vec2( 0.0,  t.y)).rgb;
            vec3 c = texture(uSource, TexCoords + vec2( t.x,  t.y)).rgb;
            vec3 d = texture(uSource, TexCoords + vec2(-t.x,  0.0)).rgb;
            vec3 e = texture(uSource, TexCoords).rgb;
            vec3 f = texture(uSource, TexCoords + vec2( t.x,  0.0)).rgb;
            vec3 g = texture(uSource, TexCoords + vec2(-t.x, -t.y)).rgb;
            vec3 h = texture(uSource, TexCoords + vec2( 0.0, -t.y)).rgb;
            vec3 i = texture(uSource, TexCoords + vec2( t.x, -t.y)).rgb;

            vec3 result = e * 4.0;
            result += (b + d + f + h) * 2.0;
            result += (a + c + g + i);
            FragColor = vec4(result * (1.0 / 16.0), 1.0);
        }
    )";

    bloom_prefilter_program = compile_shaders(quad_vert, prefilter_frag);
    bloom_down_program = compile_shaders(quad_vert, down_frag);
    bloom_up_program = compile_shaders(quad_vert, up_frag);
    report_program_link(bloom_prefilter_program, "bloom_prefilter");
    report_program_link(bloom_down_program, "bloom_downsample");
    report_program_link(bloom_up_program, "bloom_upsample");
}

void Renderer::render_bloom() {
    if (bloom_prefilter_program == 0 || bloom_fbo[0] == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(quad_vao);

    // 1. Prefilter the scene into the top of the pyramid.
    glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[0]);
    glViewport(0, 0, bloom_w[0], bloom_h[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(bloom_prefilter_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolve_texture);
    glUniform1i(glGetUniformLocation(bloom_prefilter_program, "uSource"), 0);
    glUniform2f(glGetUniformLocation(bloom_prefilter_program, "uTexelStep"),
                1.0f / static_cast<float>(fbo_width), 1.0f / static_cast<float>(fbo_height));
    glUniform1f(glGetUniformLocation(bloom_prefilter_program, "uThreshold"), 1.0f);
    glUniform1f(glGetUniformLocation(bloom_prefilter_program, "uKnee"), 0.6f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // 2. Downsample down the chain.
    glUseProgram(bloom_down_program);
    glUniform1i(glGetUniformLocation(bloom_down_program, "uSource"), 0);
    for (int i = 1; i < kBloomMips; ++i) {
        if (bloom_fbo[i] == 0) break;
        glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[i]);
        glViewport(0, 0, bloom_w[i], bloom_h[i]);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bloom_texture[i - 1]);
        glUniform2f(glGetUniformLocation(bloom_down_program, "uTexelStep"),
                    1.0f / static_cast<float>(bloom_w[i - 1]), 1.0f / static_cast<float>(bloom_h[i - 1]));
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // 3. Upsample back up, accumulating additively so every scale contributes.
    glUseProgram(bloom_up_program);
    glUniform1i(glGetUniformLocation(bloom_up_program, "uSource"), 0);
    glUniform1f(glGetUniformLocation(bloom_up_program, "uRadius"), 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int i = kBloomMips - 1; i > 0; --i) {
        if (bloom_fbo[i] == 0) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[i - 1]);
        glViewport(0, 0, bloom_w[i - 1], bloom_h[i - 1]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bloom_texture[i]);
        glUniform2f(glGetUniformLocation(bloom_up_program, "uTexelStep"),
                    1.0f / static_cast<float>(bloom_w[i]), 1.0f / static_cast<float>(bloom_h[i]));
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbo_width, fbo_height);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::render_ssao() {
    if (is_offline_rendering || ssao_shader_program == 0 || ssao_fbo == 0) return;

    // 1. Estimate occlusion and bounce light at half resolution.
    glBindFramebuffer(GL_FRAMEBUFFER, ssao_fbo);
    glViewport(0, 0, ssao_width, ssao_height);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(ssao_shader_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolve_depth_texture);
    glUniform1i(glGetUniformLocation(ssao_shader_program, "uDepthMap"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, resolve_texture);
    glUniform1i(glGetUniformLocation(ssao_shader_program, "uSceneColor"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gNormal);
    glUniform1i(glGetUniformLocation(ssao_shader_program, "uNormalMap"), 2);

    Matrix4x4 invProj = projection_matrix.inverse();
    glUniformMatrix4fv(glGetUniformLocation(ssao_shader_program, "uProj"), 1, GL_FALSE, projection_matrix.m.data());
    glUniformMatrix4fv(glGetUniformLocation(ssao_shader_program, "uInvProj"), 1, GL_FALSE, invProj.m.data());
    glUniformMatrix4fv(glGetUniformLocation(ssao_shader_program, "uView"), 1, GL_FALSE, view_matrix.m.data());
    glUniform1f(glGetUniformLocation(ssao_shader_program, "uRadius"), 0.6f);
    glUniform1i(glGetUniformLocation(ssao_shader_program, "uFrameIndex"), static_cast<int>(frame_index % 8));
    // VXGI and hardware RT have no backend here, so they run the SSGI path rather
    // than leaving the scene with no indirect light at all.
    glUniform1i(glGetUniformLocation(ssao_shader_program, "uGIMode"),
                gi_mode == GIMode::Off ? 0 : 1);

    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // 2. Depth-aware blur to remove the estimator's residual sampling noise.
    glBindFramebuffer(GL_FRAMEBUFFER, ssao_blur_fbo);
    glViewport(0, 0, ssao_width, ssao_height);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(ssao_blur_shader_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssao_texture);
    glUniform1i(glGetUniformLocation(ssao_blur_shader_program, "uSSAO"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, resolve_depth_texture);
    glUniform1i(glGetUniformLocation(ssao_blur_shader_program, "uDepthMap"), 1);
    glUniform2f(glGetUniformLocation(ssao_blur_shader_program, "uTexelStep"),
                1.0f / static_cast<float>(ssao_width), 1.0f / static_cast<float>(ssao_height));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    // From here the blur target holds a real result, so the next frame's lighting
    // pass may use it.
    ssao_history_valid = true;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbo_width, fbo_height);
}

void Renderer::render_god_rays() {
    if (is_offline_rendering) return;

    Vector3 sun_dir = {0, -1, 0};
    bool found_dir_light = false;
    for (auto* l : lights) {
        if (auto dir_l = dynamic_cast<DirectionalLightComponent*>(l)) {
            sun_dir = dir_l->get_direction().normalized();
            found_dir_light = true;
            break;
        }
    }
    // No directional light to catch rays from - skip rather than fabricate a
    // fallback direction, which previously projected to a nonsense screen
    // position and rendered a bright phantom artifact with nothing behind it.
    if (!found_dir_light) return;

    Vector3 towards_sun = { -sun_dir.x, -sun_dir.y, -sun_dir.z };
    Matrix4x4 view_rot = view_matrix;
    view_rot.m[12] = 0; view_rot.m[13] = 0; view_rot.m[14] = 0;
    
    // Multiply view_rot * towards_sun
    float vx = view_rot.m[0] * towards_sun.x + view_rot.m[4] * towards_sun.y + view_rot.m[8] * towards_sun.z;
    float vy = view_rot.m[1] * towards_sun.x + view_rot.m[5] * towards_sun.y + view_rot.m[9] * towards_sun.z;
    float vz = view_rot.m[2] * towards_sun.x + view_rot.m[6] * towards_sun.y + view_rot.m[10] * towards_sun.z;
    
    // Multiply projection_matrix * v (with w = 0.0 since it's a direction)
    float cx = projection_matrix.m[0] * vx + projection_matrix.m[4] * vy + projection_matrix.m[8] * vz;
    float cy = projection_matrix.m[1] * vx + projection_matrix.m[5] * vy + projection_matrix.m[9] * vz;
    float cz = projection_matrix.m[2] * vx + projection_matrix.m[6] * vy + projection_matrix.m[10] * vz;
    float cw = projection_matrix.m[3] * vx + projection_matrix.m[7] * vy + projection_matrix.m[11] * vz;
    
    // If the sun is behind the camera, don't bloom (w < 0)
    if (cw < 0.0f) {
        return; // Sun is behind
    }
    
    float ndc_x = cx / cw;
    float ndc_y = cy / cw;
    float screen_pos_x = (ndc_x + 1.0f) * 0.5f;
    float screen_pos_y = (ndc_y + 1.0f) * 0.5f;
    
    // 1. Render God Rays to god_rays_fbo
    glBindFramebuffer(GL_FRAMEBUFFER, god_rays_fbo);
    glViewport(0, 0, fbo_width, fbo_height);
    glClear(GL_COLOR_BUFFER_BIT);
    
    glUseProgram(god_rays_shader_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolve_texture);
    glUniform1i(god_rays_screen_texture_loc, 0);
    glUniform2f(god_rays_sun_pos_loc, screen_pos_x, screen_pos_y);
    Vector3 sun_color = {1.0f, 1.0f, 1.0f};
    for (auto* l : lights) {
        if (auto dir_l = dynamic_cast<DirectionalLightComponent*>(l)) { sun_color = dir_l->color; break; }
    }
    glUniform3f(glGetUniformLocation(god_rays_shader_program, "uSunColor"), sun_color.x, sun_color.y, sun_color.z);
    
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    // 2. Composite God Rays back onto resolve_texture using a ping-pong or just blending.
    // Wait, we can't read and write to resolve_texture at the same time.
    // Let's bind resolve_fbo and enable additive blending.
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE); // Additive
    
    glUseProgram(composite_shader_program);
    // Actually composite_shader_program adds base + blend, but since we enabled Additive Blending,
    // we can just draw a quad that outputs the god_rays_texture.
    // But wait, the composite shader adds them manually. So we disable GL_BLEND and do it manually,
    // OR we just use a simple shader that passes through and let GL_BLEND add it.
    // Let's just use the composite shader and write back to a temp, OR we can just use the fxaa_fbo as temp!
    // Since we need to composite, let's use fxaa_fbo as the destination, then copy it back to resolve_texture.
    
    // Disable blend just in case
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, fxaa_fbo); // Borrow fxaa_fbo as temp buffer
    
    glUseProgram(composite_shader_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolve_texture);
    glUniform1i(composite_base_texture_loc, 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, god_rays_texture);
    glUniform1i(composite_blend_texture_loc, 1);
    
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    // Copy fxaa_fbo color back to resolve_texture
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fxaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
    glBlitFramebuffer(0, 0, fbo_width, fbo_height, 0, 0, fbo_width, fbo_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void Renderer::resolve_fxaa() {
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // Ensure post-processing quads are not drawn as wireframe
    if (!enable_msaa) return; // Note: We use enable_msaa flag for FXAA toggle
    unsigned int fxaa_input_texture = resolve_texture;

    if (enable_taa) {
        int current_history_index = frame_index % 2;
        int prev_history_index = (frame_index + 1) % 2;

        glBindFramebuffer(GL_FRAMEBUFFER, history_fbo[current_history_index]);
        glViewport(0, 0, fbo_width, fbo_height);
        
        glUseProgram(taa_shader_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, resolve_texture);
        glUniform1i(glGetUniformLocation(taa_shader_program, "currentColor"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, history_texture[prev_history_index]);
        glUniform1i(glGetUniformLocation(taa_shader_program, "historyColor"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, resolve_depth_texture);
        glUniform1i(glGetUniformLocation(taa_shader_program, "uDepthMap"), 2);

        Matrix4x4 current_view_proj = projection_matrix * view_matrix;
        Matrix4x4 reproject = prev_view_projection * current_view_proj.inverse();
        
        glUniformMatrix4fv(glGetUniformLocation(taa_shader_program, "uReproject"), 1, GL_FALSE, reproject.m.data());
        glUniform1f(glGetUniformLocation(taa_shader_program, "uBlendFactor"), 0.75f);

        glBindVertexArray(quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        
        fxaa_input_texture = history_texture[current_history_index];
        prev_view_projection = current_view_proj;
        frame_index++;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fxaa_fbo);
    glViewport(0, 0, fbo_width, fbo_height);
    glClear(GL_COLOR_BUFFER_BIT);
    
    glUseProgram(fxaa_shader_program);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fxaa_input_texture);
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "screenTexture"), 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, resolve_depth_texture);
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "uDepthMap"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, exposure_texture[exposure_current_index]);
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "uAdaptedLuminance"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ssao_blur_texture);
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "uSSAOMap"), 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, bloom_texture[0]);
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "uBloomMap"), 4);
    glUniform1f(glGetUniformLocation(fxaa_shader_program, "uBloomIntensity"), 0.9f);
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "uEnableSSR"), enable_ssr ? 1 : 0);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, gPBR);
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "uPBRMap"), 5);

    glUniform2f(glGetUniformLocation(fxaa_shader_program, "texelStep"), 1.0f / fbo_width, 1.0f / fbo_height);
    
    glUniformMatrix4fv(glGetUniformLocation(fxaa_shader_program, "uProj"), 1, GL_FALSE, projection_matrix.m.data());
    Matrix4x4 invProj = projection_matrix.inverse();
    glUniformMatrix4fv(glGetUniformLocation(fxaa_shader_program, "uInvProj"), 1, GL_FALSE, invProj.m.data());

    // Post-process debug views: set LITHIUM_DEBUG_VIEW to
    // 1=scene color in, 2=AO, 3=SSGI, 4=depth, 5=reconstructed normal,
    // 6=color before exposure, 7=exposure, 8=smoothness(alpha), 9=adapted luminance.
    // Any view also paints NaN/Inf pixels magenta.
    static const int debug_view = [] {
        const char* v = std::getenv("LITHIUM_DEBUG_VIEW");
        return v ? std::atoi(v) : 0;
    }();
    glUniform1i(glGetUniformLocation(fxaa_shader_program, "uDebugView"), debug_view);
    
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::init_exposure_resources() {
    // Two ping-ponged 1x1 textures holding the temporally-smoothed scene
    // log-luminance, used to drive automatic exposure (eye adaptation).
    const std::string adapt_frag = R"(
        #version 450 core
        out vec4 FragColor;
        uniform sampler2D screenTexture;
        uniform sampler2D uPrevLuminance;
        uniform float uDeltaTime;
        uniform bool uFirstFrame;

        void main() {
            // Cheap spatial average over a sparse grid (no mipmap dependency,
            // so it can't be affected by resolve_texture's filtering elsewhere).
            const int GRID = 8;
            float sum = 0.0;
            for (int y = 0; y < GRID; y++) {
                for (int x = 0; x < GRID; x++) {
                    vec2 uv = (vec2(x, y) + 0.5) / float(GRID);
                    vec3 c = texture(screenTexture, uv).rgb;
                    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
                    sum += log(max(luma, 0.0001));
                }
            }
            float currentLogLum = sum / float(GRID * GRID);

            float prevLogLum = texture(uPrevLuminance, vec2(0.5)).r;
            // Exponential temporal smoothing so exposure adapts gradually
            // instead of snapping instantly frame to frame.
            float adaptSpeed = 1.5;
            float t = uFirstFrame ? 1.0 : clamp(1.0 - exp(-uDeltaTime * adaptSpeed), 0.0, 1.0);
            float newLogLum = mix(prevLogLum, currentLogLum, t);

            FragColor = vec4(newLogLum, 0.0, 0.0, 1.0);
        }
    )";

    const char* v_src = R"(
        #version 450 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aTexCoords;
        void main() { gl_Position = vec4(aPos.x, aPos.y, 0.0, 1.0); }
    )";
    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &v_src, NULL);
    glCompileShader(vertex);

    const char* f_src = adapt_frag.c_str();
    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &f_src, NULL);
    glCompileShader(fragment);

    int success;
    char infoLog[512];
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment, 512, NULL, infoLog);
        std::cerr << "Luminance adapt shader compilation failed:\n" << infoLog << std::endl;
    }

    luminance_adapt_shader_program = glCreateProgram();
    glAttachShader(luminance_adapt_shader_program, vertex);
    glAttachShader(luminance_adapt_shader_program, fragment);
    glLinkProgram(luminance_adapt_shader_program);
    report_program_link(luminance_adapt_shader_program, "luminance_adapt");

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    luminance_adapt_screen_loc = glGetUniformLocation(luminance_adapt_shader_program, "screenTexture");
    luminance_adapt_prev_loc = glGetUniformLocation(luminance_adapt_shader_program, "uPrevLuminance");
    luminance_adapt_dt_loc = glGetUniformLocation(luminance_adapt_shader_program, "uDeltaTime");

    glGenFramebuffers(2, exposure_fbo);
    glGenTextures(2, exposure_texture);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, exposure_texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, 1, 1, 0, GL_RED, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, exposure_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, exposure_texture[i], 0);

        // Seed with log(0.18) (a standard "middle grey" key value) so exposure
        // starts sane instead of ramping up from a zero-initialized texture.
        glViewport(0, 0, 1, 1);
        glClearColor(std::log(0.18f), 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    exposure_current_index = 0;
    exposure_initialized = false;
}

void Renderer::update_exposure(float delta_time) {
    if (!luminance_adapt_shader_program) return;

    int target_index = 1 - exposure_current_index;
    int prev_index = exposure_current_index;

    glBindFramebuffer(GL_FRAMEBUFFER, exposure_fbo[target_index]);
    glViewport(0, 0, 1, 1);

    glUseProgram(luminance_adapt_shader_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolve_texture);
    glUniform1i(luminance_adapt_screen_loc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, exposure_texture[prev_index]);
    glUniform1i(luminance_adapt_prev_loc, 1);

    glUniform1f(luminance_adapt_dt_loc, delta_time);
    glUniform1i(glGetUniformLocation(luminance_adapt_shader_program, "uFirstFrame"), exposure_initialized ? 0 : 1);
    exposure_initialized = true;

    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    exposure_current_index = target_index;
}

void Renderer::create_fbo(int width, int height) {
    // Every recorded occlusion result refers to depth in the framebuffer about to be
    // replaced, and any query still in flight was issued against it. Both are
    // meaningless from here on, and the query objects would otherwise leak when the
    // graphics API is swapped out from under them.
    reset_occlusion_state();

    if (gBuffer_fbo) {
        destroy_fbo();
    }

    fbo_width = width;
    fbo_height = height;

    // 1. G-Buffer Framebuffer
    glGenFramebuffers(1, &gBuffer_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gBuffer_fbo);
    
    // Position color buffer
    glGenTextures(1, &gPosition);
    glBindTexture(GL_TEXTURE_2D, gPosition);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gPosition, 0);
    
    // Normal color buffer
    glGenTextures(1, &gNormal);
    glBindTexture(GL_TEXTURE_2D, gNormal);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gNormal, 0);
    
    // Albedo + Specular (a) color buffer
    glGenTextures(1, &gAlbedoSpec);
    glBindTexture(GL_TEXTURE_2D, gAlbedoSpec);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gAlbedoSpec, 0);

    // PBR Properties (r: metallic, g: roughness, b: emission)
    glGenTextures(1, &gPBR);
    glBindTexture(GL_TEXTURE_2D, gPBR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, gPBR, 0);

    // Baked indirect light. A fifth target rather than a spare channel because the
    // other four are full and baked GI is a colour: folding it into the existing
    // scalar emissive channel would throw away exactly the colour bleed that makes
    // a lightmap worth having.
    glGenTextures(1, &gBakedGI);
    glBindTexture(GL_TEXTURE_2D, gBakedGI);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, gBakedGI, 0);

    // Tell OpenGL which color attachments we'll use (of this framebuffer) for rendering 
    unsigned int attachments[5] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                                    GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4 };
    void (*glDrawBuffers)(int, const unsigned int*) = (void (*)(int, const unsigned int*))SDL_GL_GetProcAddress("glDrawBuffers");
    if (glDrawBuffers) {
        glDrawBuffers(5, attachments);
    }

    // Depth buffer
    glGenTextures(1, &gDepth);
    glBindTexture(GL_TEXTURE_2D, gDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gDepth, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Error: G-Buffer Framebuffer is not complete!" << std::endl;
    }

    // 2. Resolve Framebuffer
    glGenFramebuffers(1, &resolve_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);

    glGenTextures(1, &resolve_texture);
    glBindTexture(GL_TEXTURE_2D, resolve_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_texture, 0);
    glGenTextures(1, &resolve_depth_texture);
    glBindTexture(GL_TEXTURE_2D, resolve_depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, resolve_depth_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Error: Resolve Framebuffer is not complete!" << std::endl;
    }

    // 3. FXAA Framebuffer
    if (fxaa_fbo) { glDeleteFramebuffers(1, &fxaa_fbo); glDeleteTextures(1, &fxaa_texture); }
    glGenFramebuffers(1, &fxaa_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fxaa_fbo);
    glGenTextures(1, &fxaa_texture);
    glBindTexture(GL_TEXTURE_2D, fxaa_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fxaa_texture, 0);

    // 3.5. God Rays Framebuffer
    if (god_rays_fbo) { glDeleteFramebuffers(1, &god_rays_fbo); glDeleteTextures(1, &god_rays_texture); }
    glGenFramebuffers(1, &god_rays_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, god_rays_fbo);
    glGenTextures(1, &god_rays_texture);
    glBindTexture(GL_TEXTURE_2D, god_rays_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, god_rays_texture, 0);

    // 3.6. SSAO / SSGI buffers at full resolution - AO and screen-space bounce
    // light carry real contact detail, and running them at native resolution keeps
    // crisp occlusion in creases and around thin geometry.
    ssao_width = std::max(1, width);
    ssao_height = std::max(1, height);
    if (ssao_fbo) { glDeleteFramebuffers(1, &ssao_fbo); glDeleteTextures(1, &ssao_texture); }
    if (ssao_blur_fbo) { glDeleteFramebuffers(1, &ssao_blur_fbo); glDeleteTextures(1, &ssao_blur_texture); }
    unsigned int* ssao_fbos[2] = { &ssao_fbo, &ssao_blur_fbo };
    ssao_history_valid = false;
    unsigned int* ssao_texes[2] = { &ssao_texture, &ssao_blur_texture };
    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, ssao_fbos[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, *ssao_fbos[i]);
        glGenTextures(1, ssao_texes[i]);
        glBindTexture(GL_TEXTURE_2D, *ssao_texes[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, ssao_width, ssao_height, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *ssao_texes[i], 0);
    }

    // 3.7. Bloom mip pyramid.
    for (int i = 0; i < kBloomMips; ++i) {
        if (bloom_fbo[i]) { glDeleteFramebuffers(1, &bloom_fbo[i]); bloom_fbo[i] = 0; }
        if (bloom_texture[i]) { glDeleteTextures(1, &bloom_texture[i]); bloom_texture[i] = 0; }
    }
    {
        int bw = std::max(1, width / 2);
        int bh = std::max(1, height / 2);
        for (int i = 0; i < kBloomMips; ++i) {
            bloom_w[i] = bw;
            bloom_h[i] = bh;
            glGenFramebuffers(1, &bloom_fbo[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[i]);
            glGenTextures(1, &bloom_texture[i]);
            glBindTexture(GL_TEXTURE_2D, bloom_texture[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_FLOAT, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            // Clamp matters here: a repeating wrap would smear the glow from one
            // screen edge onto the opposite one during the upsample taps.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloom_texture[i], 0);
            bw = std::max(1, bw / 2);
            bh = std::max(1, bh / 2);
        }
    }

    // 4. TAA History Buffers
    for (int i = 0; i < 2; ++i) {
        if (history_fbo[i]) { glDeleteFramebuffers(1, &history_fbo[i]); glDeleteTextures(1, &history_texture[i]); }
        glGenFramebuffers(1, &history_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, history_fbo[i]);
        glGenTextures(1, &history_texture[i]);
        glBindTexture(GL_TEXTURE_2D, history_texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, history_texture[i], 0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::destroy_fbo() {
    if (gBakedGI) { glDeleteTextures(1, &gBakedGI); gBakedGI = 0; }
    if (gBuffer_fbo) glDeleteFramebuffers(1, &gBuffer_fbo);
    if (gPosition) glDeleteTextures(1, &gPosition);
    if (gNormal) glDeleteTextures(1, &gNormal);
    if (gAlbedoSpec) glDeleteTextures(1, &gAlbedoSpec);
    if (gPBR) glDeleteTextures(1, &gPBR);
    if (gDepth) glDeleteTextures(1, &gDepth);
    
    for (int i = 0; i < kBloomMips; ++i) {
        if (bloom_fbo[i]) { glDeleteFramebuffers(1, &bloom_fbo[i]); bloom_fbo[i] = 0; }
        if (bloom_texture[i]) { glDeleteTextures(1, &bloom_texture[i]); bloom_texture[i] = 0; }
    }

    if (ssao_fbo) { glDeleteFramebuffers(1, &ssao_fbo); ssao_fbo = 0; }
    if (ssao_texture) { glDeleteTextures(1, &ssao_texture); ssao_texture = 0; }
    if (ssao_blur_fbo) { glDeleteFramebuffers(1, &ssao_blur_fbo); ssao_blur_fbo = 0; }
    if (ssao_blur_texture) { glDeleteTextures(1, &ssao_blur_texture); ssao_blur_texture = 0; }

    if (resolve_fbo) glDeleteFramebuffers(1, &resolve_fbo);
    if (resolve_texture) glDeleteTextures(1, &resolve_texture);
    if (resolve_depth_texture) glDeleteTextures(1, &resolve_depth_texture);
    for (int i=0; i<2; ++i) {
        if (history_fbo[i]) glDeleteFramebuffers(1, &history_fbo[i]);
        if (history_texture[i]) glDeleteTextures(1, &history_texture[i]);
        history_fbo[i] = history_texture[i] = 0;
    }
    gBuffer_fbo = gPosition = gNormal = gAlbedoSpec = gPBR = gDepth = resolve_fbo = resolve_texture = resolve_depth_texture = 0;
}

void Renderer::bind_fbo() {
    glBindFramebuffer(GL_FRAMEBUFFER, gBuffer_fbo);
    glViewport(0, 0, fbo_width, fbo_height);
}

void Renderer::unbind_fbo() {
    // 1. Unbind G-Buffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // 2. Perform Lighting Pass into resolve_fbo
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);
    glViewport(0, 0, fbo_width, fbo_height);
    glClearColor(0.10f, 0.11f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Copy depth buffer from gBuffer to resolve_fbo for forward rendering passes (e.g. Skybox)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gBuffer_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
    glBlitFramebuffer(0, 0, fbo_width, fbo_height, 0, 0, fbo_width, fbo_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);

    glUseProgram(lighting_shader_program);
    
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gPosition);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gNormal);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gAlbedoSpec);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, gPBR);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, shadow_depth_map);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, env_map_texture);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, env_irradiance_texture);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, env_prefiltered_texture);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, ssao_blur_texture);
    glUniform1i(glGetUniformLocation(lighting_shader_program, "uHasSSAO"),
                (ssao_history_valid && ssao_blur_texture != 0) ? 1 : 0);
    glUniform1f(glGetUniformLocation(lighting_shader_program, "uSSAOStrength"), ssao_strength);
    // Unit 12: baked indirect light from the lightmap atlas or the probe grid,
    // whichever the geometry pass wrote for each pixel.
    glActiveTexture(GL_TEXTURE12); glBindTexture(GL_TEXTURE_2D, gBakedGI);
    // Back to unit 0: this pass reaches higher than any other, and a later one that
    // binds without selecting a unit first would otherwise land on 12.
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(glGetUniformLocation(lighting_shader_program, "uPrefilteredMaxLod"), env_prefiltered_max_lod);
    if (has_env_map_location != -1) glUniform1i(has_env_map_location, has_env_map ? 1 : 0);
    if (env_map_max_lod_location != -1) glUniform1f(env_map_max_lod_location, env_map_max_lod);

    if (camera_pos_location != -1) {
        // Geometry is submitted camera-relative, so in the space the G-buffer stores
        // positions in, the camera is at the origin. Passing the absolute camera
        // position here made every view-dependent term - specular, Fresnel,
        // reflections, fog distance - drift as the camera moved away from world zero.
        glUniform3f(camera_pos_location, 0.0f, 0.0f, 0.0f);
    }
    // Absolute camera position, used only where real world-space height is needed.
    glUniform3f(glGetUniformLocation(lighting_shader_program, "uCameraWorldPos"),
                camera_position.x, camera_position.y, camera_position.z);
    
    glUniformMatrix4fv(glGetUniformLocation(lighting_shader_program, "uLightSpaceMatrix"), 1, GL_FALSE, light_space_matrix.m.data());
    glUniform1f(glGetUniformLocation(lighting_shader_program, "uShadowTexelWorldSize"), shadow_texel_world_size);
    glUniform1f(glGetUniformLocation(lighting_shader_program, "uShadowDepthRange"), shadow_depth_range);
    glUniform1f(glGetUniformLocation(lighting_shader_program, "uFogDensity"), fog_density);
    glUniform1f(glGetUniformLocation(lighting_shader_program, "uFogHeight"), fog_height);
    glUniform1f(glGetUniformLocation(lighting_shader_program, "uFogHeightFalloff"), fog_height_falloff);
    
    if (num_lights_location != -1) {
        glUniform1i(num_lights_location, std::min(static_cast<int>(lights.size()), 8));
    }
    
    for (size_t i = 0; i < std::min(lights.size(), (size_t)8); ++i) {
        LightComponent* light = lights[i];
        std::string prefix = "uLights[" + std::to_string(i) + "].";
        
        int type = 0;
        Vector3 pos = {0,0,0};
        Vector3 dir = {0,-1,0};
        float radius = 10.0f;
        float inner = 12.5f, outer = 17.5f;
        
        if (auto dir_light = dynamic_cast<DirectionalLightComponent*>(light)) {
            type = 0;
            dir = dir_light->get_direction();
        } else if (auto pt_light = dynamic_cast<PointLightComponent*>(light)) {
            type = 1;
            if (pt_light->get_owner()) pos = pt_light->get_owner()->get_actor_transform().position.to_vec3();
            radius = pt_light->radius;
        } else if (auto spt_light = dynamic_cast<SpotLightComponent*>(light)) {
            type = 2;
            if (spt_light->get_owner()) pos = spt_light->get_owner()->get_actor_transform().position.to_vec3();
            dir = spt_light->get_direction();
            inner = std::cos(spt_light->inner_angle * 3.14159f / 180.0f);
            outer = std::cos(spt_light->outer_angle * 3.14159f / 180.0f);
        } else if (auto area_light = dynamic_cast<AreaLightComponent*>(light)) {
            type = 3;
            if (area_light->get_owner()) pos = area_light->get_owner()->get_actor_transform().position.to_vec3();
        } else if (auto sky_light = dynamic_cast<SkyLightComponent*>(light)) {
            type = 4;
        }
        
        glUniform1i(glGetUniformLocation(lighting_shader_program, (prefix + "type").c_str()), type);
        glUniform3f(glGetUniformLocation(lighting_shader_program, (prefix + "position").c_str()), pos.x, pos.y, pos.z);
        glUniform3f(glGetUniformLocation(lighting_shader_program, (prefix + "direction").c_str()), dir.x, dir.y, dir.z);
        glUniform3f(glGetUniformLocation(lighting_shader_program, (prefix + "color").c_str()), light->color.x, light->color.y, light->color.z);
        glUniform1f(glGetUniformLocation(lighting_shader_program, (prefix + "intensity").c_str()), light->intensity);
        glUniform1f(glGetUniformLocation(lighting_shader_program, (prefix + "radius").c_str()), radius);
        glUniform1f(glGetUniformLocation(lighting_shader_program, (prefix + "innerCutOff").c_str()), inner);
        glUniform1f(glGetUniformLocation(lighting_shader_program, (prefix + "outerCutOff").c_str()), outer);
    }
    
    if (ray_tracing_location != -1) {
        glUniform1i(ray_tracing_location, enable_ray_tracing ? 1 : 0);
    }
    
    glUniform1i(glGetUniformLocation(lighting_shader_program, "uEnableUE4Lighting"), enable_ue4_lighting ? 1 : 0);

    // G-buffer / lighting debug views: set LITHIUM_DEBUG_LIGHTING to
    // 1=albedo, 2=normal, 3=world pos, 4=metallic, 5=roughness, 6=direct light,
    // 7=ambient, 8=diffuse ambient, 9=specular ambient, 10=shadow, 11=reflection
    // colour, 12=ambient fresnel, 13=sky irradiance, 14=volumetric.
    // NaN/Inf pixels are painted magenta in every view.
    static const int light_debug = [] {
        const char* v = std::getenv("LITHIUM_DEBUG_LIGHTING");
        return v ? std::atoi(v) : 0;
    }();
    glUniform1i(glGetUniformLocation(lighting_shader_program, "uLightDebug"), light_debug);

    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glUseProgram(0);
}

void Renderer::bind_resolve_fbo() {
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);
}

void Renderer::unbind_resolve_fbo() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::begin_frame() {
    if (pending_fbo_width > 0 && pending_fbo_height > 0) {
        if (pending_fbo_width != fbo_width || pending_fbo_height != fbo_height) {
            create_fbo(pending_fbo_width, pending_fbo_height);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, gBuffer_fbo);
    glViewport(0, 0, fbo_width, fbo_height);
    
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(geometry_shader_program);
}

void Renderer::end_frame() {
    glUseProgram(0);
}

void Renderer::render_skybox() {
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    
    Vector3 light_direction = {0.5f, -1.0f, 0.5f};
    bool enable_3d_clouds = false;
    int sky_mode = 0;
    Vector3 void_color = { 0.015f, 0.02f, 0.045f };
    for (auto* l : lights) {
        if (auto dir_l = dynamic_cast<DirectionalLightComponent*>(l)) {
            light_direction = dir_l->get_direction();
            enable_3d_clouds = dir_l->enable_3d_clouds;
            sky_mode = dir_l->sky_mode;
            void_color = dir_l->void_color;
            break;
        }
    }

    if (!enable_ue4_lighting) return; // Only draw beautiful sky in Sodium mode
    
    glDepthFunc(GL_LEQUAL);
    glUseProgram(sky_shader_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, env_map_texture);
    if (sky_env_map_location != -1) glUniform1i(sky_env_map_location, 0);
    if (sky_has_env_map_location != -1) glUniform1i(sky_has_env_map_location, has_env_map ? 1 : 0);

    float aspect = (float)fbo_width / (float)fbo_height;
    float fovRad = 45.0f * (3.14159265f / 180.0f);
    float tanHalfFov = std::tan(fovRad / 2.0f);
    
    Vector3 right = { view_matrix.m[0], view_matrix.m[4], view_matrix.m[8] };
    Vector3 up = { view_matrix.m[1], view_matrix.m[5], view_matrix.m[9] };
    Vector3 forward = { -view_matrix.m[2], -view_matrix.m[6], -view_matrix.m[10] };
    
    float fwd_arr[3] = {forward.x, forward.y, forward.z};
    float rgt_arr[3] = {right.x, right.y, right.z};
    float up_arr[3] = {up.x, up.y, up.z};
    float sun_arr[3] = {-light_direction.x, -light_direction.y, -light_direction.z};
    
    glUniform3fv(sky_forward_loc, 1, fwd_arr);
    glUniform3fv(sky_right_loc, 1, rgt_arr);
    glUniform3fv(sky_up_loc, 1, up_arr);
    glUniform1f(sky_fov_tan_loc, tanHalfFov);
    glUniform1f(sky_aspect_loc, aspect);
    glUniform3fv(sky_sun_dir_loc, 1, sun_arr);
    
    float time_sec = sky_time_override >= 0.0f ? sky_time_override : SDL_GetTicks() / 1000.0f;
    glUniform1f(sky_time_loc, time_sec);
    
    glUniform1i(sky_enable_3d_clouds_loc, enable_3d_clouds ? 1 : 0);
    if (sky_mode_loc != -1) glUniform1i(sky_mode_loc, sky_mode);
    if (sky_void_color_loc != -1) glUniform3f(sky_void_color_loc, void_color.x, void_color.y, void_color.z);
    
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDepthFunc(GL_LESS);
}

bool Renderer::load_environment_map(const std::string& hdr_path) {
    int width, height, channels;
    stbi_set_flip_vertically_on_load(true);
    float* data = stbi_loadf(hdr_path.c_str(), &width, &height, &channels, 3);
    // stbi_set_flip_vertically_on_load is global, not scoped to this call - reset
    // it immediately so it doesn't silently flip unrelated 2D UI/icon images
    // loaded elsewhere afterward - this exact bug once flipped an embedded UI image.
    stbi_set_flip_vertically_on_load(false);
    if (!data) {
        std::cerr << "[Renderer] Failed to load HDRI environment map: " << hdr_path << std::endl;
        return false;
    }

    if (env_map_texture) {
        glDeleteTextures(1, &env_map_texture);
        env_map_texture = 0;
    }

    // The texture is GL_RGB16F, whose largest representable value is 65504. HDRIs
    // routinely store sun radiance well above that (the bundled DefaultSky.hdr peaks
    // at ~75776), and anything over the limit becomes +Inf on upload. glGenerateMipmap
    // then averages that Inf outward until it covers whole mip levels, so every
    // surface sampling the environment for ambient light received Inf, every lit
    // pixel became NaN, and the entire scene rendered as garbage. Clamp on the way in
    // so no single texel can poison the mip chain, and drop any non-finite values the
    // source file itself may contain.
    const float kMaxRadiance = 8192.0f; // far brighter than any surface, safely inside half-float
    size_t texel_count = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    size_t clamped_texels = 0;
    for (size_t i = 0; i < texel_count; ++i) {
        float v = data[i];
        if (!std::isfinite(v)) { data[i] = 0.0f; ++clamped_texels; continue; }
        if (v < 0.0f) { data[i] = 0.0f; continue; }
        if (v > kMaxRadiance) { data[i] = kMaxRadiance; ++clamped_texels; }
    }

    glGenTextures(1, &env_map_texture);
    glBindTexture(GL_TEXTURE_2D, env_map_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);

    build_irradiance_map(data, width, height);
    build_prefiltered_specular(data, width, height);

    // Keep a copy for the CPU path tracer, which cannot read a GL texture.
    env_map_path = hdr_path;
    env_map_width = width;
    env_map_height = height;
    env_map_cpu.assign(data, data + texel_count);

    stbi_image_free(data);

    // Roughest reflections sample the coarsest mip; used to drive the
    // roughness -> LOD mapping in sampleEnvironmentSpecular/Diffuse.
    env_map_max_lod = std::floor(std::log2(static_cast<float>(std::max(width, height))));
    has_env_map = true;

    std::cout << "[Renderer] Loaded HDRI environment map: " << hdr_path << " (" << width << "x" << height << ")";
    if (clamped_texels > 0) {
        std::cout << " [" << clamped_texels << " out-of-range texels clamped]";
    }
    std::cout << std::endl;
    return true;
}

// Builds the GGX-prefiltered specular environment chain (the "prefiltered
// environment map" term of Karis' split-sum IBL approximation). Each mip is the
// environment convolved with the GGX NDF for a fixed roughness, using cosine
// weighted importance sampling. Mip 0 is the sharp environment (mirror
// reflections); roughness increases linearly with mip index.
//
// This replaces sampling a plain box-filtered mip chain, which is the wrong kernel:
// it blurs isotropically rather than along the GGX lobe, and it leaves an HDRI's sun
// as a near-point highlight at low roughness, so reflections sparkled between
// neighbouring pixels depending on whether a given ray happened to hit it.
void Renderer::build_prefiltered_specular(const float* src, int src_width, int src_height) {
    const int kBaseW = 512, kBaseH = 256;
    const int kMips = 7;
    const float kPi = 3.14159265358979f;

    // Work from a moderately downsampled copy so each importance sample is a cheap
    // bilinear fetch; the convolution is wide enough that source detail beyond this
    // cannot survive it anyway (mip 0, which keeps full sharpness, is resampled
    // straight from the source instead).
    const int kSrcW = 512, kSrcH = 256;
    std::vector<float> small(static_cast<size_t>(kSrcW) * kSrcH * 3, 0.0f);
    for (int y = 0; y < kSrcH; ++y) {
        int y0 = y * src_height / kSrcH;
        int y1 = std::max(y0 + 1, (y + 1) * src_height / kSrcH);
        for (int x = 0; x < kSrcW; ++x) {
            int x0 = x * src_width / kSrcW;
            int x1 = std::max(x0 + 1, (x + 1) * src_width / kSrcW);
            float r = 0, g = 0, b = 0; int n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    size_t i = (static_cast<size_t>(sy) * src_width + sx) * 3;
                    r += src[i]; g += src[i + 1]; b += src[i + 2]; ++n;
                }
            }
            size_t o = (static_cast<size_t>(y) * kSrcW + x) * 3;
            small[o] = r / n; small[o + 1] = g / n; small[o + 2] = b / n;
        }
    }

    // Bilinear equirect lookup, wrapping in longitude and clamping in latitude.
    auto sample_env = [&](float dx, float dy, float dz, float* out) {
        float u = std::atan2(dz, dx) / (2.0f * kPi) + 0.5f;
        float v = std::asin(std::max(-1.0f, std::min(1.0f, dy))) / kPi + 0.5f;
        float fx = u * kSrcW - 0.5f;
        float fy = v * kSrcH - 0.5f;
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        float tx = fx - x0, ty = fy - y0;
        for (int k = 0; k < 3; ++k) out[k] = 0.0f;
        for (int j = 0; j < 2; ++j) {
            int yy = std::max(0, std::min(kSrcH - 1, y0 + j));
            float wy = j ? ty : (1.0f - ty);
            for (int i = 0; i < 2; ++i) {
                int xx = ((x0 + i) % kSrcW + kSrcW) % kSrcW;
                float wx = i ? tx : (1.0f - tx);
                size_t o = (static_cast<size_t>(yy) * kSrcW + xx) * 3;
                float w = wx * wy;
                out[0] += small[o] * w;
                out[1] += small[o + 1] * w;
                out[2] += small[o + 2] * w;
            }
        }
    };

    // Van der Corput radical inverse -> Hammersley point set.
    auto radical_inverse = [](unsigned int bits) {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    };

    // Compute every level on the CPU first. The loading callback runs between mips
    // and draws a frame, which rebinds textures - so no GL upload may be in flight
    // across it.
    std::vector<std::vector<float>> levels(kMips);
    std::vector<int> level_w(kMips), level_h(kMips);

    std::vector<float> level;
    for (int mip = 0; mip < kMips; ++mip) {
        int mw = std::max(1, kBaseW >> mip);
        int mh = std::max(1, kBaseH >> mip);
        float roughness = static_cast<float>(mip) / static_cast<float>(kMips - 1);
        level.assign(static_cast<size_t>(mw) * mh * 3, 0.0f);

        // More samples as the lobe widens; a mirror needs none at all.
        int sample_count = (mip == 0) ? 1 : (64 + 32 * mip);
        float a = roughness * roughness;

        // One row of the output map. Rows write disjoint ranges of `level` and only
        // read shared data, so they parallelise cleanly.
        auto compute_row = [&](int y) {
            float v = (y + 0.5f) / mh;
            float lat = (v - 0.5f) * kPi;
            float cos_lat = std::cos(lat);
            float ny = std::sin(lat);
            for (int x = 0; x < mw; ++x) {
                float u = (x + 0.5f) / mw;
                float lon = (u - 0.5f) * 2.0f * kPi;
                float nx = cos_lat * std::cos(lon);
                float nz = cos_lat * std::sin(lon);
                size_t o = (static_cast<size_t>(y) * mw + x) * 3;

                if (mip == 0) {
                    sample_env(nx, ny, nz, &level[o]);
                    continue;
                }

                // Build a tangent frame around N.
                float upx = (std::fabs(ny) < 0.999f) ? 0.0f : 1.0f;
                float upy = (std::fabs(ny) < 0.999f) ? 1.0f : 0.0f;
                float upz = 0.0f;
                float tx = upy * nz - upz * ny;
                float ty = upz * nx - upx * nz;
                float tz = upx * ny - upy * nx;
                float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
                if (tl < 1e-6f) { tx = 1.0f; ty = 0.0f; tz = 0.0f; tl = 1.0f; }
                tx /= tl; ty /= tl; tz /= tl;
                float bx = ny * tz - nz * ty;
                float by = nz * tx - nx * tz;
                float bz = nx * ty - ny * tx;

                float acc[3] = {0.0f, 0.0f, 0.0f};
                float total_weight = 0.0f;
                for (int i = 0; i < sample_count; ++i) {
                    float xi1 = (i + 0.5f) / sample_count;
                    float xi2 = radical_inverse(static_cast<unsigned int>(i));

                    // GGX importance sample: concentrate directions in the lobe.
                    float phi = 2.0f * kPi * xi1;
                    float cos_theta = std::sqrt((1.0f - xi2) / (1.0f + (a * a - 1.0f) * xi2));
                    float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
                    float hx = sin_theta * std::cos(phi);
                    float hy = sin_theta * std::sin(phi);
                    float hz = cos_theta;

                    // Tangent -> world.
                    float wx = tx * hx + bx * hy + nx * hz;
                    float wy = ty * hx + by * hy + ny * hz;
                    float wz = tz * hx + bz * hy + nz * hz;

                    // With the usual V = N = R simplification, L = reflect(-N, H).
                    float ndoth = nx * wx + ny * wy + nz * wz;
                    float lx = 2.0f * ndoth * wx - nx;
                    float ly = 2.0f * ndoth * wy - ny;
                    float lz = 2.0f * ndoth * wz - nz;
                    float ndotl = nx * lx + ny * ly + nz * lz;
                    if (ndotl <= 0.0f) continue;

                    float c[3];
                    sample_env(lx, ly, lz, c);
                    acc[0] += c[0] * ndotl;
                    acc[1] += c[1] * ndotl;
                    acc[2] += c[2] * ndotl;
                    total_weight += ndotl;
                }
                if (total_weight > 0.0f) {
                    level[o]     = acc[0] / total_weight;
                    level[o + 1] = acc[1] / total_weight;
                    level[o + 2] = acc[2] / total_weight;
                } else {
                    sample_env(nx, ny, nz, &level[o]);
                }
            }
        };

        // Spread rows across cores. This convolution is the bulk of the CPU cost at
        // startup and it is embarrassingly parallel; running it single-threaded made
        // the user sit through it for no reason. The main thread deliberately does
        // not block on a join - it keeps pumping the loading frame so the window
        // stays responsive while the workers run, instead of freezing for the duration.
        unsigned int worker_count = std::max(1u, std::thread::hardware_concurrency());
        if (worker_count > 1 && mh > 1) {
            std::atomic<int> next_row{0};
            std::atomic<int> rows_done{0};
            std::vector<std::thread> workers;
            workers.reserve(worker_count);
            for (unsigned int w = 0; w < worker_count; ++w) {
                workers.emplace_back([&]() {
                    for (;;) {
                        int y = next_row.fetch_add(1);
                        if (y >= mh) break;
                        compute_row(y);
                        rows_done.fetch_add(1);
                    }
                });
            }
            while (rows_done.load() < mh) {
                report_loading_progress("Prefiltering environment map");
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            for (auto& t : workers) t.join();
        } else {
            for (int y = 0; y < mh; ++y) {
                compute_row(y);
                report_loading_progress();
            }
        }

        levels[mip] = level;
        level_w[mip] = mw;
        level_h[mip] = mh;
        report_loading_progress();
    }

    if (env_prefiltered_texture) {
        glDeleteTextures(1, &env_prefiltered_texture);
        env_prefiltered_texture = 0;
    }
    glGenTextures(1, &env_prefiltered_texture);
    glBindTexture(GL_TEXTURE_2D, env_prefiltered_texture);
    for (int mip = 0; mip < kMips; ++mip) {
        glTexImage2D(GL_TEXTURE_2D, mip, GL_RGB16F, level_w[mip], level_h[mip], 0, GL_RGB, GL_FLOAT, levels[mip].data());
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, kMips - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    env_prefiltered_max_lod = static_cast<float>(kMips - 1);
}

// Cosine-convolves the equirectangular environment map into a small irradiance map,
// i.e. for each output direction N it integrates incoming radiance over the
// hemisphere weighted by max(dot(N, L), 0) and the texel's solid angle. That is the
// quantity Lambertian ambient actually wants; sampling a blurred mip of the
// environment only approximates it and, at the coarsest mip, degenerates to one
// constant colour for every normal.
//
// Irradiance is extremely low frequency, so a 32x16 output is plenty, and the source
// is downsampled first to keep the double loop cheap enough to run synchronously at
// load time.
void Renderer::build_irradiance_map(const float* src, int src_width, int src_height) {
    const int kOutW = 32, kOutH = 16;
    const int kInW = 128, kInH = 64;

    // Box-downsample the source to kInW x kInH.
    std::vector<float> small(static_cast<size_t>(kInW) * kInH * 3, 0.0f);
    for (int y = 0; y < kInH; ++y) {
        int y0 = y * src_height / kInH;
        int y1 = std::max(y0 + 1, (y + 1) * src_height / kInH);
        for (int x = 0; x < kInW; ++x) {
            int x0 = x * src_width / kInW;
            int x1 = std::max(x0 + 1, (x + 1) * src_width / kInW);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            int n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    size_t i = (static_cast<size_t>(sy) * src_width + sx) * 3;
                    r += src[i]; g += src[i + 1]; b += src[i + 2];
                    ++n;
                }
            }
            size_t o = (static_cast<size_t>(y) * kInW + x) * 3;
            small[o] = r / n; small[o + 1] = g / n; small[o + 2] = b / n;
        }
    }

    // Precompute each source texel's direction and solid angle. Equirect rows shrink
    // toward the poles, so the sin(theta) term is required or the poles get weighted
    // far too heavily.
    struct Sample { float dx, dy, dz, r, g, b, w; };
    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(kInW) * kInH);
    const float kPi = 3.14159265358979f;
    for (int y = 0; y < kInH; ++y) {
        // v maps to latitude the same way directionToEquirectUV does: v = asin(dir.y)/PI + 0.5
        float v = (y + 0.5f) / kInH;
        float lat = (v - 0.5f) * kPi;          // -PI/2 .. PI/2
        float cos_lat = std::cos(lat);
        float dy = std::sin(lat);
        float solid_angle = cos_lat * (kPi / kInH) * (2.0f * kPi / kInW);
        for (int x = 0; x < kInW; ++x) {
            float u = (x + 0.5f) / kInW;
            float lon = (u - 0.5f) * 2.0f * kPi;
            Sample sm;
            sm.dx = cos_lat * std::cos(lon);
            sm.dy = dy;
            sm.dz = cos_lat * std::sin(lon);
            size_t i = (static_cast<size_t>(y) * kInW + x) * 3;
            sm.r = small[i]; sm.g = small[i + 1]; sm.b = small[i + 2];
            sm.w = solid_angle;
            samples.push_back(sm);
        }
    }

    std::vector<float> out(static_cast<size_t>(kOutW) * kOutH * 3, 0.0f);
    for (int y = 0; y < kOutH; ++y) {
        float v = (y + 0.5f) / kOutH;
        float lat = (v - 0.5f) * kPi;
        float cos_lat = std::cos(lat);
        float ny = std::sin(lat);
        for (int x = 0; x < kOutW; ++x) {
            float u = (x + 0.5f) / kOutW;
            float lon = (u - 0.5f) * 2.0f * kPi;
            float nx = cos_lat * std::cos(lon);
            float nz = cos_lat * std::sin(lon);

            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (const Sample& sm : samples) {
                float ndotl = nx * sm.dx + ny * sm.dy + nz * sm.dz;
                if (ndotl <= 0.0f) continue;
                float w = ndotl * sm.w;
                r += sm.r * w; g += sm.g * w; b += sm.b * w;
            }
            // Lambertian normalisation: E = (1/PI) * integral(L * cos(theta) dw)
            size_t o = (static_cast<size_t>(y) * kOutW + x) * 3;
            out[o]     = r / kPi;
            out[o + 1] = g / kPi;
            out[o + 2] = b / kPi;
        }
        report_loading_progress("Convolving irradiance");
    }

    if (env_irradiance_texture) {
        glDeleteTextures(1, &env_irradiance_texture);
        env_irradiance_texture = 0;
    }
    glGenTextures(1, &env_irradiance_texture);
    glBindTexture(GL_TEXTURE_2D, env_irradiance_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, kOutW, kOutH, 0, GL_RGB, GL_FLOAT, out.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Renderer::clear_environment_map() {
    if (env_map_texture) {
        glDeleteTextures(1, &env_map_texture);
        env_map_texture = 0;
    }
    if (env_irradiance_texture) {
        glDeleteTextures(1, &env_irradiance_texture);
        env_irradiance_texture = 0;
    }
    if (env_prefiltered_texture) {
        glDeleteTextures(1, &env_prefiltered_texture);
        env_prefiltered_texture = 0;
    }
    has_env_map = false;
    env_map_max_lod = 0.0f;
    env_prefiltered_max_lod = 0.0f;
    env_map_path.clear();
    env_map_cpu.clear();
    env_map_cpu.shrink_to_fit();
    env_map_width = 0;
    env_map_height = 0;
}

void Renderer::render_mesh(const StaticMeshComponent& mesh_component, const Transform& transform, const Vector3& color_override,
                            float metallic, float roughness, float clearcoat, float clearcoat_roughness, float sheen, float subsurface, float emissive,
                            bool is_invisible, bool is_selected,
                            const std::vector<Matrix4x4>* bone_matrices,
                            const MeshResource* lod_mesh,
                            const MaterialShader* custom_shader,
                            const std::vector<MaterialShader::Value>* custom_shader_values) {
    if (is_invisible) return; // Do not draw if actor is marked invisible

    // A material with its own surface shader takes a separate path: the program, and
    // therefore every uniform location, is the shader's rather than the engine's.
    // The selection outline is not drawn for these - it is an editor-only highlight
    // and reproducing it here would mean duplicating the whole outline pass.
    if (custom_shader && custom_shader->is_valid()) {
        profiler.draw_calls++;
        profiler.triangles += static_cast<int>(mesh_component.get_indices_count_internal() / 3);

        glUseProgram(custom_shader->get_program());

        const Matrix4x4 model = transform.get_relative_matrix(camera_pos);
        const Matrix4x4 mvp = projection_matrix * view_matrix * model;
        if (custom_shader->mvp_location != -1) {
            glUniformMatrix4fv(custom_shader->mvp_location, 1, GL_FALSE, mvp.m.data());
        }
        if (custom_shader->model_location != -1) {
            glUniformMatrix4fv(custom_shader->model_location, 1, GL_FALSE, model.m.data());
        }
        if (custom_shader->light_space_location != -1) {
            glUniformMatrix4fv(custom_shader->light_space_location, 1, GL_FALSE, light_space_matrix.m.data());
        }
        if (custom_shader->ue4_location != -1) {
            glUniform1i(custom_shader->ue4_location, enable_ue4_lighting ? 1 : 0);
        }
        if (custom_shader->time_location != -1) {
            // Seconds since the process started, so an animated shader has a clock
            // without every material having to be told the time.
            glUniform1f(custom_shader->time_location,
                        static_cast<float>(SDL_GetTicks()) / 1000.0f);
        }

        // The engine's own vertex stage, so a custom-shaded mesh still skins.
        apply_skinning_uniforms(custom_shader->skinned_location, custom_shader->bones_location, bone_matrices);

        if (custom_shader->ambient_cube_location != -1) {
            glUniform3fv(custom_shader->ambient_cube_location, 6, &current_ambient_cube[0].x);
        }

        static const std::vector<MaterialShader::Value> kNoValues;
        custom_shader->apply_properties(custom_shader_values ? *custom_shader_values : kNoValues);

        glPolygonMode(GL_FRONT_AND_BACK, wireframe_mode ? GL_LINE : GL_FILL);
        mesh_component.render(lod_mesh);
        if (wireframe_mode) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // The geometry pass leaves its own program bound between draws.
        glUseProgram(geometry_shader_program);
        return;
    }

    profiler.draw_calls++;
    profiler.triangles += static_cast<int>(mesh_component.get_indices_count_internal() / 3);

    glUseProgram(geometry_shader_program);

    Matrix4x4 model = transform.get_relative_matrix(camera_pos); // LWC: Compute relative to camera
    Matrix4x4 mvp = projection_matrix * view_matrix * model;

    if (mvp_location != -1) {
        glUniformMatrix4fv(mvp_location, 1, GL_FALSE, mvp.m.data());
    }

    if (model_location != -1) {
        glUniformMatrix4fv(model_location, 1, GL_FALSE, model.m.data());
    }

    if (color_override_location != -1) {
        float color_val[4] = { color_override.x, color_override.y, color_override.z, 1.0f };
        glUniform4fv(color_override_location, 1, color_val);
    }

    glUniformMatrix4fv(glGetUniformLocation(geometry_shader_program, "uLightSpaceMatrix"), 1, GL_FALSE, light_space_matrix.m.data());

    apply_skinning_uniforms(skinned_location, bones_location, bone_matrices);

    // Baked lighting. A lightmapped actor samples the atlas; everything else takes
    // the ambient cube the engine sampled from the probe grid for this object.
    const bool use_lightmap = mesh_component.has_lightmap();
    unsigned int lightmap_texture = use_lightmap ? Lightmapper::get().get_atlas_texture() : 0;
    if (has_lightmap_location != -1) {
        glUniform1i(has_lightmap_location, (use_lightmap && lightmap_texture != 0) ? 1 : 0);
    }
    if (lightmap_texture != 0 && lightmap_location != -1) {
        glActiveTexture(GL_TEXTURE11);
        glBindTexture(GL_TEXTURE_2D, lightmap_texture);
        glUniform1i(lightmap_location, 11);
        glActiveTexture(GL_TEXTURE0);
    }
    if (ambient_cube_location != -1) {
        glUniform3fv(ambient_cube_location, 6, &current_ambient_cube[0].x);
    }

    if (ue4_lighting_location != -1) {
        glUniform1i(ue4_lighting_location, enable_ue4_lighting ? 1 : 0);
    }

    if (metallic_location != -1) {
        glUniform1f(metallic_location, metallic);
    }
    
    if (roughness_location != -1) {
        glUniform1f(roughness_location, roughness);
    }

    if (clearcoat_location != -1) {
        glUniform1f(clearcoat_location, clearcoat);
    }
    if (clearcoat_roughness_location != -1) {
        glUniform1f(clearcoat_roughness_location, clearcoat_roughness);
    }
    if (sheen_location != -1) {
        glUniform1f(sheen_location, sheen);
    }
    if (emissive_location != -1) {
        glUniform1f(emissive_location, emissive);
    }

    if (subsurface_location != -1) {
        glUniform1f(subsurface_location, subsurface);
    }

    auto diffuse_texture = mesh_component.get_diffuse_texture();
    bool has_texture = diffuse_texture && diffuse_texture->get_state() == ResourceState::LoadedGPU;
    if (has_diffuse_texture_location != -1) {
        glUniform1i(has_diffuse_texture_location, has_texture ? 1 : 0);
    }
    if (has_texture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, diffuse_texture->get_texture_id());
        if (diffuse_texture_location != -1) {
            glUniform1i(diffuse_texture_location, 0);
        }
    }

    if (wireframe_mode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // An LOD substitute brings its own clusters and VAO; falling back to the full
    // mesh's would index the wrong buffer.
    const bool lod_ready = lod_mesh && lod_mesh->get_state() == ResourceState::LoadedGPU;
    const auto& clusters = lod_ready ? lod_mesh->get_clusters()
                                     : (mesh_component.get_mesh_resource() ? mesh_component.get_mesh_resource()->get_clusters()
                                                                           : mesh_component.get_clusters());
    if (enable_lithite && culling_compute_program != 0 && !clusters.empty()) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, cluster_ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, clusters.size() * sizeof(MeshCluster), clusters.data(), GL_DYNAMIC_DRAW);
        
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, command_ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, clusters.size() * 5 * sizeof(unsigned int), nullptr, GL_DYNAMIC_DRAW);
        
        glUseProgram(culling_compute_program);
        
        // Pass plane normals and distances as vec4s
        float planes_data[24];
        for (int i = 0; i < 6; ++i) {
            planes_data[i * 4 + 0] = frustum_planes[i].normal.x;
            planes_data[i * 4 + 1] = frustum_planes[i].normal.y;
            planes_data[i * 4 + 2] = frustum_planes[i].normal.z;
            planes_data[i * 4 + 3] = frustum_planes[i].distance;
        }
        glUniform4fv(glGetUniformLocation(culling_compute_program, "uFrustumPlanes"), 6, planes_data);
        glUniform1ui(glGetUniformLocation(culling_compute_program, "uNumClusters"), static_cast<unsigned int>(clusters.size()));
        glUniformMatrix4fv(glGetUniformLocation(culling_compute_program, "uModelMatrix"), 1, GL_FALSE, model.m.data());
        
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, cluster_ssbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, command_ssbo);
        
        glDispatchCompute((clusters.size() + 63) / 64, 1, 1);
        glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        
        glUseProgram(geometry_shader_program);
        
        unsigned int vao = lod_ready ? lod_mesh->get_vao()
                                     : (mesh_component.get_mesh_resource() ? mesh_component.get_mesh_resource()->get_vao()
                                                                           : mesh_component.get_vao());
        if (vao) {
            glBindVertexArray(vao);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, command_ssbo);
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, 0, static_cast<GLsizei>(clusters.size()), 0);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            glBindVertexArray(0);
        }
    } else {
        mesh_component.render(lod_mesh);
    }

    // If selected, render a yellow wireframe outline slightly larger. Uses a
    // small constant world-space margin rather than a percentage of the
    // object's own scale - a percentage blows up on large objects (e.g. 3% of
    // a 20-unit floor plane is a 0.6-unit margin, causing severe z-fighting
    // and grazing-angle clipping artifacts on thin/large geometry).
    //
    // Room/world-scale objects (large floors, walls, terrain) skip the
    // outline entirely: the camera is frequently very close to or effectively
    // inside such geometry, where even a well-behaved thin wireframe outline
    // rasterizes as broken fill/streak artifacts. The Details panel and the
    // transform gizmo already make the selection obvious for these.
    const float max_scale_axis = std::max({transform.scale.x, transform.scale.y, transform.scale.z});
    if (is_selected && max_scale_axis <= 5.0f) {
        Transform outline_transform = transform;
        const float outline_margin = 0.03f;
        outline_transform.scale = {
            transform.scale.x + outline_margin,
            transform.scale.y + outline_margin,
            transform.scale.z + outline_margin
        };
        Matrix4x4 outline_model = outline_transform.get_relative_matrix(camera_pos); // LWC
        Matrix4x4 outline_mvp = projection_matrix * view_matrix * outline_model;

        if (mvp_location != -1) {
            glUniformMatrix4fv(mvp_location, 1, GL_FALSE, outline_mvp.m.data());
        }

        if (model_location != -1) {
            glUniformMatrix4fv(model_location, 1, GL_FALSE, outline_model.m.data());
        }

        if (color_override_location != -1) {
            float yellow[4] = { 1.0f, 0.85f, 0.0f, 1.0f };
            glUniform4fv(color_override_location, 1, yellow);
        }
        if (has_diffuse_texture_location != -1) {
            glUniform1i(has_diffuse_texture_location, 0);
        }

        // Draw in wireframe mode
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(2.0f); // Make the line thicker
        if (enable_lithite && culling_compute_program != 0 && !clusters.empty()) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, cluster_ssbo);
            glBufferData(GL_SHADER_STORAGE_BUFFER, clusters.size() * sizeof(MeshCluster), clusters.data(), GL_DYNAMIC_DRAW);
            
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, command_ssbo);
            glBufferData(GL_SHADER_STORAGE_BUFFER, clusters.size() * 5 * sizeof(unsigned int), nullptr, GL_DYNAMIC_DRAW);
            
            glUseProgram(culling_compute_program);
            
            // Pass plane normals and distances as vec4s
            float planes_data[24];
            for (int i = 0; i < 6; ++i) {
                planes_data[i * 4 + 0] = frustum_planes[i].normal.x;
                planes_data[i * 4 + 1] = frustum_planes[i].normal.y;
                planes_data[i * 4 + 2] = frustum_planes[i].normal.z;
                planes_data[i * 4 + 3] = frustum_planes[i].distance;
            }
            
            glUniform4fv(glGetUniformLocation(culling_compute_program, "uFrustumPlanes"), 6, planes_data);
            glUniform1ui(glGetUniformLocation(culling_compute_program, "uNumClusters"), static_cast<unsigned int>(clusters.size()));
            glUniformMatrix4fv(glGetUniformLocation(culling_compute_program, "uModelMatrix"), 1, GL_FALSE, outline_model.m.data());
            
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, cluster_ssbo);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, command_ssbo);
            
            glDispatchCompute((clusters.size() + 63) / 64, 1, 1);
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
            
            glUseProgram(geometry_shader_program);
            
            unsigned int vao = lod_ready ? lod_mesh->get_vao()
                                         : (mesh_component.get_mesh_resource() ? mesh_component.get_mesh_resource()->get_vao()
                                                                               : mesh_component.get_vao());
            if (vao) {
                glBindVertexArray(vao);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, command_ssbo);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, 0, static_cast<GLsizei>(clusters.size()), 0);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
                glBindVertexArray(0);
            }
        } else {
            mesh_component.render(lod_mesh);
        }
        
        if (!wireframe_mode) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // Restore fill mode
        }
    }
}

namespace {

// ---------------------------------------------------------------------------
// Shader program binary cache
//
// Startup is dominated by shader compilation - measured at ~3.7s of a ~4s launch on
// a Mesa/Intel driver. GL 4.1 can hand back a linked program as an opaque binary and
// reload it later, skipping the compiler entirely on every run after the first.
//
// The cache key covers the shader source *and* the driver identity: a program binary
// is only valid for the exact driver that produced it, so a GPU or driver update must
// invalidate every entry. glProgramBinary is also permitted to reject a binary for
// any reason, so a failed load always falls back to compiling from source.
// ---------------------------------------------------------------------------
const char* kShaderCacheDir = "ShaderCache";

std::string shader_cache_key(const std::string& vs, const std::string& fs) {
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer_name = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    std::string material = vs + "\x1f" + fs + "\x1f";
    material += vendor ? reinterpret_cast<const char*>(vendor) : "?";
    material += "\x1f";
    material += renderer_name ? reinterpret_cast<const char*>(renderer_name) : "?";
    material += "\x1f";
    material += version ? reinterpret_cast<const char*>(version) : "?";

    // FNV-1a. Not cryptographic - it only has to distinguish shader variants, and a
    // collision would at worst load a binary the driver then rejects, which falls
    // back to compiling.
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : material) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

bool shader_cache_enabled() {
    // Escape hatch for driver bugs, and for anyone debugging shader changes.
    static const bool disabled = (std::getenv("LITHIUM_NO_SHADER_CACHE") != nullptr);
    return !disabled && glGetProgramBinary && glProgramBinary && glProgramParameteri;
}

unsigned int load_cached_program(const std::string& key) {
    if (!shader_cache_enabled()) return 0;
    std::filesystem::path file = std::filesystem::path(kShaderCacheDir) / (key + ".bin");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return 0;

    std::ifstream in(file, std::ios::binary);
    if (!in) return 0;
    GLenum format = 0;
    in.read(reinterpret_cast<char*>(&format), sizeof(format));
    if (!in) return 0;
    std::vector<char> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (blob.empty()) return 0;

    unsigned int program = glCreateProgram();
    glProgramBinary(program, format, blob.data(), static_cast<GLsizei>(blob.size()));

    int linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        // Stale or rejected (driver update, different GPU). Drop it and recompile.
        glDeleteProgram(program);
        std::filesystem::remove(file, ec);
        return 0;
    }
    return program;
}

void store_cached_program(const std::string& key, unsigned int program) {
    if (!shader_cache_enabled() || program == 0) return;
    int length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0) return;   // driver supports no binary formats

    std::vector<char> blob(static_cast<size_t>(length));
    GLenum format = 0;
    GLsizei written = 0;
    glGetProgramBinary(program, length, &written, &format, blob.data());
    if (written <= 0) return;

    std::error_code ec;
    std::filesystem::create_directories(kShaderCacheDir, ec);
    std::ofstream out(std::filesystem::path(kShaderCacheDir) / (key + ".bin"), std::ios::binary);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(&format), sizeof(format));
    out.write(blob.data(), written);
}

} // namespace

unsigned int Renderer::compile_shaders(const std::string& vertex_src, const std::string& fragment_src) {
    // Try the on-disk cache before invoking the driver's compiler.
    const std::string cache_key = shader_cache_key(vertex_src, fragment_src);
    if (unsigned int cached = load_cached_program(cache_key)) {
        return cached;
    }

    const char* v_src = vertex_src.c_str();
    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &v_src, NULL);
    glCompileShader(vertex);

    int success;
    char infoLog[512];
    glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertex, 512, NULL, infoLog);
        std::cerr << "Vertex shader compilation failed:\n" << infoLog << std::endl;
        return false;
    }

    const char* f_src = fragment_src.c_str();
    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &f_src, NULL);
    glCompileShader(fragment);
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment, 512, NULL, infoLog);
        std::cerr << "Fragment shader compilation failed:\n" << infoLog << std::endl;
        return false;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    // Must be set before linking, or the driver is free to discard the binary.
    if (shader_cache_enabled()) {
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(program, 512, NULL, infoLog);
        std::cerr << "Shader program linking failed:\n" << infoLog << std::endl;
        return 0;
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    store_cached_program(cache_key, program);
    return program;
}

// ===========================================================================
// TESLA - unbiased path-traced viewport
//
// All of the actual work lives in TeslaRenderer (renderer/tesla.hpp). What is left
// here is the viewport plumbing: sizing the target, noticing when the camera has
// invalidated the accumulated samples, and presenting the result.
//
// The Embree scene that used to live here is gone. Both TESLA backends now traverse
// the same binned-SAH BVH, which is what lets the CPU and GPU integrators produce
// the same image instead of two different ones.
// ===========================================================================

void Renderer::ensure_tesla_present_target(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (tesla_present_texture != 0 && width == tesla_present_width && height == tesla_present_height) return;

    tesla_present_width = width;
    tesla_present_height = height;

    if (tesla_present_texture == 0) glGenTextures(1, &tesla_present_texture);
    glBindTexture(GL_TEXTURE_2D, tesla_present_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (tesla_present_fbo == 0) glGenFramebuffers(1, &tesla_present_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, tesla_present_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tesla_present_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Fixed-size luminance pyramid; 64x64 is plenty to average a frame and it keeps
    // the reduction independent of the render resolution.
    if (tesla_lum_texture == 0) {
        glGenTextures(1, &tesla_lum_texture);
        glBindTexture(GL_TEXTURE_2D, tesla_lum_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 64, 64, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &tesla_lum_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, tesla_lum_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tesla_lum_texture, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void Renderer::start_offline_render(int width, int height) {
    if (is_offline_rendering) return;
    is_offline_rendering = true;

    tesla.initialize_gpu();
    tesla.resize(width, height);
    ensure_tesla_present_target(width, height);

    // Force a camera-change reset on the first step.
    tesla_last_camera_pos = { 1e30f, 1e30f, 1e30f };
}

void Renderer::cancel_offline_render() {
    is_offline_rendering = false;
}

void Renderer::step_offline_render() {
    if (!is_offline_rendering) return;

    tesla.resize(fbo_width, fbo_height);
    ensure_tesla_present_target(fbo_width, fbo_height);

    // A camera change invalidates every sample gathered so far; anything else
    // (a longer render, a resumed pass) must keep accumulating, because throwing
    // samples away is the one thing that stops the estimate converging.
    Matrix4x4 view_proj = projection_matrix * view_matrix;
    bool camera_changed = (camera_position - tesla_last_camera_pos).length() > 1e-4f;
    if (!camera_changed) {
        for (int i = 0; i < 16; ++i) {
            if (std::abs(view_proj.m[i] - tesla_last_view_proj.m[i]) > 1e-6f) { camera_changed = true; break; }
        }
    }

    // Trace in the same scene-centred frame the geometry was built in.
    tesla.set_camera(view_matrix, projection_matrix, camera_position - tesla_world_origin.to_vec3());

    if (camera_changed) {
        tesla_last_view_proj = view_proj;
        tesla_last_camera_pos = camera_position;
        tesla.reset_accumulation();
    }

    tesla.step();
    present_tesla();
}

void Renderer::present_tesla() {
    if (tesla_present_program == 0 || tesla_present_fbo == 0) return;
    if (tesla.accumulation_texture() == 0) return;

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(quad_vao);

    // Reduce the frame to a log-luminance pyramid so the present pass can pick an
    // exposure. Without this a dim scene tonemaps to solid black.
    if (tesla_auto_exposure && tesla_lum_program != 0 && tesla_lum_fbo != 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, tesla_lum_fbo);
        glViewport(0, 0, 64, 64);
        glUseProgram(tesla_lum_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tesla.accumulation_texture());
        glUniform1i(glGetUniformLocation(tesla_lum_program, "uAccum"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindTexture(GL_TEXTURE_2D, tesla_lum_texture);
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, tesla_present_fbo);
    glViewport(0, 0, tesla_present_width, tesla_present_height);

    glUseProgram(tesla_present_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tesla.accumulation_texture());
    glUniform1i(glGetUniformLocation(tesla_present_program, "uAccum"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tesla_lum_texture);
    glUniform1i(glGetUniformLocation(tesla_present_program, "uLuminance"), 1);

    glUniform1f(glGetUniformLocation(tesla_present_program, "uExposure"), tesla_exposure);
    glUniform1i(glGetUniformLocation(tesla_present_program, "uAutoExposure"), tesla_auto_exposure ? 1 : 0);

    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);


    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_DEPTH_TEST);
}