#include "renderer/material_shader.hpp"
#include "renderer/gl_loader.hpp"
#include "renderer/shader_sources.hpp"
#include "core/resource_manager.hpp"
#include "core/texture_resource.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

// Units 0 through 3 belong to the engine's own bindings in the geometry pass
// (diffuse, shadow map, environment). Custom shader textures start clear of them.
constexpr int kFirstCustomTextureUnit = 4;

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Contents of `name { ... }`, found by brace matching so a block containing braces -
// which any non-trivial surface function will - is not cut short at the first one.
bool extract_block(const std::string& source, const std::string& name, std::string& out_body) {
    size_t at = source.find(name);
    while (at != std::string::npos) {
        // Must be a standalone word, or "surface" would match inside "subsurface".
        const bool starts_clean = (at == 0) || (!std::isalnum(static_cast<unsigned char>(source[at - 1])) &&
                                                source[at - 1] != '_');
        const size_t after = at + name.size();
        if (starts_clean) {
            size_t brace = source.find('{', after);
            if (brace != std::string::npos && trim(source.substr(after, brace - after)).empty()) {
                int depth = 1;
                size_t scan = brace + 1;
                while (scan < source.size() && depth > 0) {
                    if (source[scan] == '{') ++depth;
                    else if (source[scan] == '}') --depth;
                    ++scan;
                }
                if (depth != 0) return false; // unbalanced
                out_body = source.substr(brace + 1, scan - brace - 2);
                return true;
            }
        }
        at = source.find(name, at + 1);
    }
    return false;
}

bool compile_stage(unsigned int shader, const std::string& source, std::string& out_error) {
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return true;
    char log[4096];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    out_error = log;
    return false;
}

} // namespace

// --- MaterialShader --------------------------------------------------------

void MaterialShader::destroy() {
    if (program != 0) {
        glDeleteProgram(program);
        program = 0;
    }
    properties.clear();
}

bool MaterialShader::compile(const std::string& source, const std::string& debug_name,
                             std::string& out_error) {
    destroy();
    last_error.clear();
    out_error.clear();

    std::string surface_body;
    if (!extract_block(source, "surface", surface_body)) {
        out_error = "No 'surface { ... }' block found.";
        last_error = out_error;
        return false;
    }

    // --- Properties --------------------------------------------------------
    std::string properties_body;
    std::string uniform_declarations;
    if (extract_block(source, "properties", properties_body)) {
        std::istringstream stream(properties_body);
        std::string line;
        int next_texture_unit = kFirstCustomTextureUnit;

        while (std::getline(stream, line)) {
            // Strip a trailing comment so `float x = 1.0 // speed` parses.
            const size_t comment = line.find("//");
            if (comment != std::string::npos) line = line.substr(0, comment);
            line = trim(line);
            if (line.empty()) continue;

            std::istringstream parts(line);
            std::string type_name, property_name;
            parts >> type_name >> property_name;
            if (property_name.empty()) {
                out_error = "Malformed property line: " + line;
                last_error = out_error;
                return false;
            }

            Property property;
            property.name = property_name;

            // Everything after the '=' is the default, if there is one.
            std::string defaults;
            const size_t equals = line.find('=');
            if (equals != std::string::npos) defaults = trim(line.substr(equals + 1));

            if (type_name == "float") {
                property.type = Property::Type::Float;
                if (!defaults.empty()) {
                    try { property.default_value[0] = std::stof(defaults); }
                    catch (const std::exception&) {
                        out_error = "Bad float default for '" + property_name + "'.";
                        last_error = out_error;
                        return false;
                    }
                }
                uniform_declarations += "uniform float " + property_name + ";\n";
            } else if (type_name == "color") {
                property.type = Property::Type::Color;
                if (!defaults.empty()) {
                    // Commas are separators; turning them into spaces lets one
                    // stream read all three components whichever the author used.
                    std::replace(defaults.begin(), defaults.end(), ',', ' ');
                    std::istringstream numbers(defaults);
                    numbers >> property.default_value[0] >> property.default_value[1]
                            >> property.default_value[2];
                }
                uniform_declarations += "uniform vec3 " + property_name + ";\n";
            } else if (type_name == "texture") {
                property.type = Property::Type::Texture;
                property.default_texture = defaults;
                property.texture_unit = next_texture_unit++;
                uniform_declarations += "uniform sampler2D " + property_name + ";\n";
            } else {
                out_error = "Unknown property type '" + type_name + "'. Use float, color or texture.";
                last_error = out_error;
                return false;
            }

            properties.push_back(property);
        }
    }

    // --- Generate ----------------------------------------------------------
    // The surface body runs inside its own scope with every output pre-declared, so
    // a shader that sets only Albedo compiles and everything else keeps its default.
    std::string fragment_source =
        "#version 450 core\n"
        "layout (location = 0) out vec4 gPosition;\n"
        "layout (location = 1) out vec4 gNormal;\n"
        "layout (location = 2) out vec4 gAlbedoSpec;\n"
        "layout (location = 3) out vec4 gPBR;\n"
        "layout (location = 4) out vec4 gBakedGI;\n"
        "\n"
        "in vec3 FragPos;\n"
        "in vec3 Normal;\n"
        "in vec3 ourColor;\n"
        "in vec2 TexCoord;\n"
        "in vec4 FragPosLightSpace;\n"
        "\n"
        "uniform float uTime;\n"
        "uniform bool uEnableUE4Lighting;\n"
        "uniform vec3 uAmbientCube[6];\n"
        + uniform_declarations +
        "\n"
        "void main() {\n"
        "    vec3  Albedo = ourColor;\n"
        "    float Metallic = 0.0;\n"
        "    float Roughness = 0.5;\n"
        "    float Emissive = 0.0;\n"
        "    float Clearcoat = 0.0;\n"
        "    float ClearcoatRoughness = 0.05;\n"
        "    float Sheen = 0.0;\n"
        "    float Subsurface = 0.0;\n"
        "    vec3  ShadingNormal = normalize(Normal);\n"
        "\n"
        "    {\n"
        + surface_body +
        "\n    }\n"
        "\n"
        "    vec3 albedo = uEnableUE4Lighting ? pow(max(Albedo, vec3(0.0)), vec3(2.2)) : Albedo;\n"
        "    gPosition   = vec4(FragPos, Emissive);\n"
        "    gNormal     = vec4(normalize(ShadingNormal), Subsurface);\n"
        "    gAlbedoSpec = vec4(albedo, Sheen);\n"
        "    gPBR        = vec4(Metallic, max(Roughness, 0.1), Clearcoat, max(ClearcoatRoughness, 0.05));\n"
        "\n"
        "    // Every geometry-pass shader has to write this target: an enabled draw\n"
        "    // buffer a shader leaves alone holds undefined data, not zero.\n"
        "    vec3 n = normalize(ShadingNormal);\n"
        "    vec3 sq = n * n;\n"
        "    vec3 baked  = (n.x >= 0.0 ? uAmbientCube[0] : uAmbientCube[1]) * sq.x;\n"
        "         baked += (n.y >= 0.0 ? uAmbientCube[2] : uAmbientCube[3]) * sq.y;\n"
        "         baked += (n.z >= 0.0 ? uAmbientCube[4] : uAmbientCube[5]) * sq.z;\n"
        "    gBakedGI = vec4(baked, 1.0);\n"
        "}\n";

    // --- Compile -----------------------------------------------------------
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    std::string stage_error;
    if (!compile_stage(vertex, ShaderSources::geometry_vertex(), stage_error)) {
        glDeleteShader(vertex);
        out_error = "Engine vertex stage failed to compile: " + stage_error;
        last_error = out_error;
        return false;
    }

    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_stage(fragment, fragment_source, stage_error)) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        // The reported line numbers are for the generated shader, not the .lshader
        // file, so say so rather than sending the author hunting for a line that is
        // not in what they wrote.
        out_error = "Surface shader failed:\n" + stage_error +
                    "\n(line numbers are in the generated shader; the surface block "
                    "starts around line " +
                    std::to_string(23 + std::count(uniform_declarations.begin(),
                                                   uniform_declarations.end(), '\n')) + ")";
        last_error = out_error;
        return false;
    }

    GLuint new_program = glCreateProgram();
    glAttachShader(new_program, vertex);
    glAttachShader(new_program, fragment);
    glLinkProgram(new_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = 0;
    glGetProgramiv(new_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(new_program, sizeof(log), nullptr, log);
        glDeleteProgram(new_program);
        out_error = std::string("Link failed: ") + log;
        last_error = out_error;
        return false;
    }

    program = new_program;

    mvp_location = glGetUniformLocation(program, "uMVP");
    model_location = glGetUniformLocation(program, "uModel");
    light_space_location = glGetUniformLocation(program, "uLightSpaceMatrix");
    skinned_location = glGetUniformLocation(program, "uSkinned");
    bones_location = glGetUniformLocation(program, "uBones");
    time_location = glGetUniformLocation(program, "uTime");
    ambient_cube_location = glGetUniformLocation(program, "uAmbientCube");
    ue4_location = glGetUniformLocation(program, "uEnableUE4Lighting");

    for (Property& property : properties) {
        property.location = glGetUniformLocation(program, property.name.c_str());
    }

    std::cout << "[Shader] Compiled " << debug_name << " (" << properties.size()
              << " properties)" << std::endl;
    return true;
}

void MaterialShader::apply_properties(const std::vector<Value>& values) const {
    for (const Property& property : properties) {
        if (property.location == -1) continue;

        // The material's value if it set one, the declared default otherwise. A
        // material never has to list a property it is happy with.
        const Value* value = nullptr;
        for (const Value& candidate : values) {
            if (candidate.name == property.name) { value = &candidate; break; }
        }

        switch (property.type) {
            case Property::Type::Float:
                glUniform1f(property.location, value ? value->number[0] : property.default_value[0]);
                break;

            case Property::Type::Color: {
                const float* rgb = value ? value->number : property.default_value;
                glUniform3f(property.location, rgb[0], rgb[1], rgb[2]);
                break;
            }

            case Property::Type::Texture: {
                const std::string& path = (value && !value->texture_path.empty())
                    ? value->texture_path : property.default_texture;
                unsigned int texture_id = 0;
                if (!path.empty()) {
                    auto texture = ResourceManager::get().load_async<TextureResource>(path);
                    if (texture && texture->get_state() == ResourceState::LoadedGPU) {
                        texture_id = texture->get_texture_id();
                    }
                }
                glActiveTexture(GL_TEXTURE0 + property.texture_unit);
                glBindTexture(GL_TEXTURE_2D, texture_id);
                glUniform1i(property.location, property.texture_unit);
                break;
            }
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

// --- MaterialShaderLibrary -------------------------------------------------

MaterialShaderLibrary& MaterialShaderLibrary::get() {
    static MaterialShaderLibrary instance;
    return instance;
}

std::shared_ptr<MaterialShader> MaterialShaderLibrary::load(const std::string& path) {
    if (path.empty()) return nullptr;
    for (const auto& entry : cache) {
        if (entry.first == path) return entry.second;
    }
    std::string error;
    return reload(path, error);
}

std::shared_ptr<MaterialShader> MaterialShaderLibrary::reload(const std::string& path,
                                                              std::string& out_error) {
    out_error.clear();
    if (path.empty()) return nullptr;

    std::ifstream file(path);
    if (!file) {
        out_error = "Could not open " + path;
    }

    std::shared_ptr<MaterialShader> shader;
    if (file) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        shader = std::make_shared<MaterialShader>();
        if (!shader->compile(buffer.str(), path, out_error)) {
            // A shader that failed to compile is cached as null so the renderer falls
            // back to the standard material rather than retrying the compile - and
            // logging the same error - every frame.
            shader.reset();
        }
    }

    bool replaced = false;
    for (auto& entry : cache) {
        if (entry.first != path) continue;
        entry.second = shader;
        replaced = true;
        break;
    }
    if (!replaced) cache.emplace_back(path, shader);

    bool error_recorded = false;
    for (auto& entry : errors) {
        if (entry.first != path) continue;
        entry.second = out_error;
        error_recorded = true;
        break;
    }
    if (!error_recorded) errors.emplace_back(path, out_error);

    if (!out_error.empty()) std::cerr << "[Shader] " << path << ": " << out_error << std::endl;
    return shader;
}

const std::string& MaterialShaderLibrary::last_error_for(const std::string& path) const {
    static const std::string none;
    for (const auto& entry : errors) {
        if (entry.first == path) return entry.second;
    }
    return none;
}
