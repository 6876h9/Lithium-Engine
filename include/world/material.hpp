#pragma once

#include "core/math.hpp"
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>

class Material {
public:
    // --- Custom surface shader -----------------------------------------------
    // Path to a .lshader. Empty uses the engine's standard PBR shading, which is
    // what every material did before shader authoring existed.
    std::string shader_path;
    // Values for the shader's declared properties. Only what this material actually
    // overrides; anything absent falls back to the shader's own default.
    struct ShaderValue {
        std::string name;
        float number[3] = { 0.0f, 0.0f, 0.0f };
        std::string texture_path;
    };
    std::vector<ShaderValue> shader_values;

    Vector3 albedo = {1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.8f;
    float emission = 0.0f;
    Vector3 emission_color = {1.0f, 1.0f, 1.0f};
    
    // Advanced PBR
    float specular = 0.5f;
    float specular_tint = 0.0f;
    float anisotropy = 0.0f;
    float anisotropy_rotation = 0.0f;
    float clearcoat = 0.0f;
    float clearcoat_roughness = 0.1f;
    float sheen = 0.0f;
    float sheen_tint = 0.0f;
    
    // Transmission / Subsurface
    float transmission = 0.0f;
    float ior = 1.45f;
    float subsurface = 0.0f;
    Vector3 subsurface_color = {1.0f, 1.0f, 1.0f};
    float thickness = 0.0f;
    
    // Volumes / Effects
    float alpha = 1.0f;
    float normal_strength = 1.0f;
    float displacement_scale = 0.0f;
    float fuzz = 0.0f;
    Vector3 absorption_color = {1.0f, 1.0f, 1.0f};
    Vector3 scatter_color = {1.0f, 1.0f, 1.0f};
    
    bool load_from_file(const std::string& path) {
        // Reset first: loading over a material that already had a shader would
        // otherwise keep values belonging to the previous one.
        shader_path.clear();
        shader_values.clear();

        std::ifstream file(path);
        if (!file.is_open()) return false;
        
        std::string line;
        while (std::getline(file, line)) {
            std::stringstream ss(line);
            std::string key;
            if (std::getline(ss, key, '=')) {
                std::string val;
                if (std::getline(ss, val)) {
                    if (key == "shader") { shader_path = val; }
                    // shader_value entries are "shader_value=<name>|<x>|<y>|<z>|<texture>",
                    // which keeps the whole thing on one line in a format that was
                    // already one key and one value per line.
                    else if (key == "shader_value") {
                        ShaderValue sv;
                        size_t start = 0;
                        int field = 0;
                        while (field < 5) {
                            const size_t bar = val.find('|', start);
                            const std::string piece = val.substr(start, (bar == std::string::npos)
                                                                        ? std::string::npos : bar - start);
                            if (field == 0) sv.name = piece;
                            else if (field <= 3) {
                                try { sv.number[field - 1] = std::stof(piece); }
                                catch (const std::exception&) { sv.number[field - 1] = 0.0f; }
                            } else sv.texture_path = piece;
                            ++field;
                            if (bar == std::string::npos) break;
                            start = bar + 1;
                        }
                        if (!sv.name.empty()) shader_values.push_back(sv);
                    }
                    else if (key == "albedo_x") albedo.x = std::stof(val);
                    else if (key == "albedo_y") albedo.y = std::stof(val);
                    else if (key == "albedo_z") albedo.z = std::stof(val);
                    else if (key == "metallic") metallic = std::stof(val);
                    else if (key == "roughness") roughness = std::stof(val);
                    else if (key == "emission") emission = std::stof(val);
                    else if (key == "emission_color_x") emission_color.x = std::stof(val);
                    else if (key == "emission_color_y") emission_color.y = std::stof(val);
                    else if (key == "emission_color_z") emission_color.z = std::stof(val);
                    else if (key == "specular") specular = std::stof(val);
                    else if (key == "specular_tint") specular_tint = std::stof(val);
                    else if (key == "anisotropy") anisotropy = std::stof(val);
                    else if (key == "anisotropy_rotation") anisotropy_rotation = std::stof(val);
                    else if (key == "clearcoat") clearcoat = std::stof(val);
                    else if (key == "clearcoat_roughness") clearcoat_roughness = std::stof(val);
                    else if (key == "sheen") sheen = std::stof(val);
                    else if (key == "sheen_tint") sheen_tint = std::stof(val);
                    else if (key == "transmission") transmission = std::stof(val);
                    else if (key == "ior") ior = std::stof(val);
                    else if (key == "subsurface") subsurface = std::stof(val);
                    else if (key == "subsurface_color_x") subsurface_color.x = std::stof(val);
                    else if (key == "subsurface_color_y") subsurface_color.y = std::stof(val);
                    else if (key == "subsurface_color_z") subsurface_color.z = std::stof(val);
                    else if (key == "thickness") thickness = std::stof(val);
                    else if (key == "alpha") alpha = std::stof(val);
                    else if (key == "normal_strength") normal_strength = std::stof(val);
                    else if (key == "displacement_scale") displacement_scale = std::stof(val);
                    else if (key == "fuzz") fuzz = std::stof(val);
                    else if (key == "absorption_color_x") absorption_color.x = std::stof(val);
                    else if (key == "absorption_color_y") absorption_color.y = std::stof(val);
                    else if (key == "absorption_color_z") absorption_color.z = std::stof(val);
                    else if (key == "scatter_color_x") scatter_color.x = std::stof(val);
                    else if (key == "scatter_color_y") scatter_color.y = std::stof(val);
                    else if (key == "scatter_color_z") scatter_color.z = std::stof(val);
                }
            }
        }
        return true;
    }
    
    void save_to_file(const std::string& path) const {
        std::ofstream file(path);
        if (!file.is_open()) return;
        
        if (!shader_path.empty()) file << "shader=" << shader_path << "\n";
        for (const ShaderValue& sv : shader_values) {
            file << "shader_value=" << sv.name << "|" << sv.number[0] << "|" << sv.number[1]
                 << "|" << sv.number[2] << "|" << sv.texture_path << "\n";
        }
        file << "albedo_x=" << albedo.x << "\n";
        file << "albedo_y=" << albedo.y << "\n";
        file << "albedo_z=" << albedo.z << "\n";
        file << "metallic=" << metallic << "\n";
        file << "roughness=" << roughness << "\n";
        file << "emission=" << emission << "\n";
        file << "emission_color_x=" << emission_color.x << "\n";
        file << "emission_color_y=" << emission_color.y << "\n";
        file << "emission_color_z=" << emission_color.z << "\n";
        file << "specular=" << specular << "\n";
        file << "specular_tint=" << specular_tint << "\n";
        file << "anisotropy=" << anisotropy << "\n";
        file << "anisotropy_rotation=" << anisotropy_rotation << "\n";
        file << "clearcoat=" << clearcoat << "\n";
        file << "clearcoat_roughness=" << clearcoat_roughness << "\n";
        file << "sheen=" << sheen << "\n";
        file << "sheen_tint=" << sheen_tint << "\n";
        file << "transmission=" << transmission << "\n";
        file << "ior=" << ior << "\n";
        file << "subsurface=" << subsurface << "\n";
        file << "subsurface_color_x=" << subsurface_color.x << "\n";
        file << "subsurface_color_y=" << subsurface_color.y << "\n";
        file << "subsurface_color_z=" << subsurface_color.z << "\n";
        file << "thickness=" << thickness << "\n";
        file << "alpha=" << alpha << "\n";
        file << "normal_strength=" << normal_strength << "\n";
        file << "displacement_scale=" << displacement_scale << "\n";
        file << "fuzz=" << fuzz << "\n";
        file << "absorption_color_x=" << absorption_color.x << "\n";
        file << "absorption_color_y=" << absorption_color.y << "\n";
        file << "absorption_color_z=" << absorption_color.z << "\n";
        file << "scatter_color_x=" << scatter_color.x << "\n";
        file << "scatter_color_y=" << scatter_color.y << "\n";
        file << "scatter_color_z=" << scatter_color.z << "\n";
    }
};
