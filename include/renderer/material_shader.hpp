#pragma once

#include <memory>
#include <string>
#include <vector>

// A user-authored surface shader.
//
// Authors write only what makes their material different - how to compute albedo,
// roughness, emission - and the engine wraps that in the G-buffer boilerplate. A
// full fragment shader would mean every author had to know the exact layout of four
// render targets, and every change to that layout would break every shader in every
// project.
//
// The file is two blocks:
//
//     properties {
//         float  uGlow  = 1.0
//         color  uTint  = 1.0, 0.4, 0.1
//         texture uDetail
//     }
//
//     surface {
//         Albedo   = uTint * texture(uDetail, TexCoord * 4.0).rgb;
//         Emissive = uGlow * (0.5 + 0.5 * sin(uTime * 2.0));
//     }
//
// The surface block is plain GLSL. It reads FragPos, Normal, TexCoord, ourColor and
// uTime, and writes Albedo, Metallic, Roughness, Emissive, Clearcoat,
// ClearcoatRoughness, Sheen, Subsurface and ShadingNormal - all pre-declared with
// sensible defaults, so a shader that sets only Albedo is valid.
//
// The vertex stage is the engine's own, which is what keeps skinning, shadows and
// large-world coordinates working on a custom-shaded mesh for free.
class MaterialShader {
public:
    struct Property {
        enum class Type { Float, Color, Texture };

        std::string name;
        Type type = Type::Float;
        // Defaults declared in the file. A material that overrides nothing uses these.
        float default_value[3] = { 0.0f, 0.0f, 0.0f };
        std::string default_texture;
        int location = -1;
        // Texture unit assigned at compile time. Units 0..3 are reserved by the
        // engine's own passes, so these start above them.
        int texture_unit = -1;
    };

    // One material's value for a property.
    struct Value {
        std::string name;
        float number[3] = { 0.0f, 0.0f, 0.0f };
        std::string texture_path;
    };

    // Compiles from source. Returns false and fills out_error with the parse or GLSL
    // error, with line numbers relative to the generated shader.
    bool compile(const std::string& source, const std::string& debug_name, std::string& out_error);
    void destroy();

    bool is_valid() const { return program != 0; }
    unsigned int get_program() const { return program; }
    const std::vector<Property>& get_properties() const { return properties; }
    const std::string& get_error() const { return last_error; }

    // Uniform locations for the parts of the vertex stage the engine drives. Cached
    // per program because a custom shader has its own.
    int mvp_location = -1;
    int model_location = -1;
    int light_space_location = -1;
    int skinned_location = -1;
    int bones_location = -1;
    int time_location = -1;
    int ue4_location = -1;
    int ambient_cube_location = -1;

    // Binds every declared property, taking each from `values` when present and from
    // the declared default otherwise. Must be called with the program already bound.
    void apply_properties(const std::vector<Value>& values) const;

private:
    unsigned int program = 0;
    std::vector<Property> properties;
    std::string last_error;
};

// Compiles and caches shaders by path, so twenty materials sharing one shader
// compile it once. Also the place a reload happens, which is what makes editing a
// shader in the editor show up without restarting.
class MaterialShaderLibrary {
public:
    static MaterialShaderLibrary& get();

    // Loads and compiles, or returns the cached copy. Null if the file is missing or
    // failed to compile; the reason is in last_error_for().
    std::shared_ptr<MaterialShader> load(const std::string& path);
    // Recompiles from disk, replacing what the cache holds.
    std::shared_ptr<MaterialShader> reload(const std::string& path, std::string& out_error);
    const std::string& last_error_for(const std::string& path) const;

private:
    MaterialShaderLibrary() = default;
    std::vector<std::pair<std::string, std::shared_ptr<MaterialShader>>> cache;
    std::vector<std::pair<std::string, std::string>> errors;
};
