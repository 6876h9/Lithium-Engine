#include "core/material_graph.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

// The material graph: data model, type inference and GLSL emission.
//
// Read material_graph.hpp first for what this generates and why the Material Output
// pin list is fixed. The three things a future engineer is most likely to trip over:
//
//  1. Emission goes through emit(), which records the node behind every generated
//     line. That map is what turns a driver's "0:37: error" into "node 12 (Append)".
//     A statement emitted with raw string concatenation instead of emit() breaks the
//     mapping for every line after it, silently.
//  2. Type promotion is only ever float -> vecN, and it is written out as an explicit
//     vecN(x). GLSL would accept some of these implicitly, but not all of them
//     (pow(vec3, float) is a compile error), and writing it out means one rule covers
//     every node instead of a per-builtin table of what GLSL happens to allow.
//  3. Property names and texture paths reach the generated source as text, so both
//     are validated against a whitelist before they get there. A property named
//     "uTime" or a path containing a newline would otherwise produce a shader whose
//     error makes no sense next to the graph that produced it.

using nlohmann::json;

namespace {

// --- Small helpers ---------------------------------------------------------

int dim(MatType t) { return static_cast<int>(t); }

MatType type_of_dim(int d) { return static_cast<MatType>(d); }

// A GLSL float literal. Locale-independent on purpose: the engine does not set a
// locale, but a plugin or a library could, and a decimal comma in generated GLSL is
// a compile error a long way from its cause.
std::string gl_float(float value) {
    if (!std::isfinite(value)) value = 0.0f;   // GLSL has no inf or nan literal
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(6) << value;
    std::string text = out.str();
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

std::string gl_literal(MatType type, const float* v) {
    if (type == MatType::Float) return gl_float(v[0]);
    std::string out = std::string(material_type_name(type)) + "(";
    for (int i = 0; i < dim(type); ++i) {
        if (i) out += ", ";
        out += gl_float(v[i]);
    }
    return out + ")";
}

// float -> vecN is the one implicit conversion the graph performs, and it is always
// written out. Anything else is a type error caught before this point.
std::string promote(const std::string& expr, MatType from, MatType to) {
    if (from == to) return expr;
    return std::string(material_type_name(to)) + "(" + expr + ")";
}

// The common type two operands share. Equal types combine to themselves; a float
// broadcasts to whatever it meets. vec2 and vec3 have no common type - masking one
// down would be a guess about which components the author meant.
bool combine(MatType a, MatType b, MatType& out) {
    if (a == b) { out = a; return true; }
    if (a == MatType::Float) { out = b; return true; }
    if (b == MatType::Float) { out = a; return true; }
    return false;
}

const char* kSwizzle = "xyzw";
const char* kColorSwizzle = "rgba";

// --- Node descriptor table -------------------------------------------------

MatPinDesc pin(const char* name, MatType type, float x = 0.0f, float y = 0.0f,
               float z = 0.0f, float w = 0.0f, const char* fallback = nullptr) {
    MatPinDesc p;
    p.name = name;
    p.type = type;
    p.def[0] = x;
    p.def[1] = y;
    p.def[2] = z;
    p.def[3] = w;
    p.fallback = fallback;
    return p;
}

std::vector<MatNodeDesc> build_descriptors() {
    std::vector<MatNodeDesc> table(static_cast<size_t>(MatNodeType::Count));

    auto set = [&](MatNodeType type, const char* key, const char* display,
                   const char* category, std::vector<MatPinDesc> inputs,
                   std::vector<MatPinDesc> outputs) {
        MatNodeDesc& d = table[static_cast<size_t>(type)];
        d.key = key;
        d.display = display;
        d.category = category;
        d.inputs = std::move(inputs);
        d.outputs = std::move(outputs);
    };

    // The Material Output pins are exactly the variables the surface template
    // pre-declares in material_shader.cpp. Nothing else may appear here.
    set(MatNodeType::MaterialOutput, "MaterialOutput", "Material Output", "Output",
        {
            pin("Base Colour", MatType::Vec3, 1.0f, 1.0f, 1.0f),
            pin("Metallic", MatType::Float),
            pin("Roughness", MatType::Float, 0.5f),
            pin("Emissive", MatType::Float),
            pin("Clearcoat", MatType::Float),
            pin("Clearcoat Roughness", MatType::Float, 0.05f),
            pin("Sheen", MatType::Float),
            pin("Subsurface", MatType::Float),
            pin("Normal", MatType::Vec3, 0.0f, 0.0f, 1.0f),
        },
        {});

    set(MatNodeType::TextureSample, "TextureSample", "Texture Sample", "Input",
        { pin("UV", MatType::Vec2, 0.0f, 0.0f, 0.0f, 0.0f, "TexCoord") },
        {
            pin("RGB", MatType::Vec3), pin("R", MatType::Float), pin("G", MatType::Float),
            pin("B", MatType::Float), pin("A", MatType::Float),
        });

    set(MatNodeType::ConstantFloat, "ConstantFloat", "Constant", "Input", {},
        { pin("Out", MatType::Float) });
    set(MatNodeType::ConstantVec2, "ConstantVec2", "Constant2", "Input", {},
        { pin("Out", MatType::Vec2) });
    set(MatNodeType::ConstantVec3, "ConstantVec3", "Constant3", "Input", {},
        { pin("Out", MatType::Vec3) });
    set(MatNodeType::ConstantVec4, "ConstantVec4", "Constant4", "Input", {},
        { pin("Out", MatType::Vec4) });

    set(MatNodeType::VertexColor, "VertexColor", "Vertex Colour", "Input", {},
        { pin("Out", MatType::Vec3) });
    set(MatNodeType::TexCoord, "TexCoord", "UV Coordinates", "Input", {},
        { pin("Out", MatType::Vec2) });
    set(MatNodeType::Time, "Time", "Time", "Input", {}, { pin("Out", MatType::Float) });
    set(MatNodeType::Fresnel, "Fresnel", "Fresnel", "Input",
        {
            pin("Exponent", MatType::Float, 5.0f),
            pin("Base Reflect", MatType::Float, 0.04f),
        },
        { pin("Out", MatType::Float) });
    set(MatNodeType::Normal, "Normal", "Normal", "Input", {}, { pin("Out", MatType::Vec3) });
    set(MatNodeType::WorldPosition, "WorldPosition", "World Position", "Input", {},
        { pin("Out", MatType::Vec3) });
    set(MatNodeType::CameraVector, "CameraVector", "Camera Vector", "Input", {},
        { pin("Out", MatType::Vec3) });

    // Math inputs are declared float so that an unconnected pin never forces the
    // result wider than the author asked for: Add(vec3, <unconnected>) is vec3.
    const auto binary = [&](MatNodeType type, const char* key, const char* display,
                            float default_b = 0.0f) {
        set(type, key, display, "Math",
            { pin("A", MatType::Float), pin("B", MatType::Float, default_b) },
            { pin("Out", MatType::Float) });
    };
    binary(MatNodeType::Add, "Add", "Add");
    binary(MatNodeType::Subtract, "Subtract", "Subtract");
    binary(MatNodeType::Multiply, "Multiply", "Multiply", 1.0f);
    binary(MatNodeType::Divide, "Divide", "Divide", 1.0f);
    binary(MatNodeType::Dot, "Dot", "Dot Product");
    binary(MatNodeType::Cross, "Cross", "Cross Product");
    binary(MatNodeType::Power, "Power", "Power", 2.0f);
    binary(MatNodeType::Min, "Min", "Min");
    binary(MatNodeType::Max, "Max", "Max");
    binary(MatNodeType::Step, "Step", "Step", 0.5f);

    set(MatNodeType::Lerp, "Lerp", "Lerp", "Math",
        {
            pin("A", MatType::Float), pin("B", MatType::Float, 1.0f),
            pin("Alpha", MatType::Float, 0.5f),
        },
        { pin("Out", MatType::Float) });
    set(MatNodeType::Clamp, "Clamp", "Clamp", "Math",
        {
            pin("Value", MatType::Float), pin("Min", MatType::Float, 0.0f),
            pin("Max", MatType::Float, 1.0f),
        },
        { pin("Out", MatType::Float) });
    set(MatNodeType::Smoothstep, "Smoothstep", "Smoothstep", "Math",
        {
            pin("Edge0", MatType::Float, 0.0f), pin("Edge1", MatType::Float, 1.0f),
            pin("Value", MatType::Float, 0.5f),
        },
        { pin("Out", MatType::Float) });

    const auto unary = [&](MatNodeType type, const char* key, const char* display) {
        set(type, key, display, "Math", { pin("In", MatType::Float) },
            { pin("Out", MatType::Float) });
    };
    unary(MatNodeType::Normalize, "Normalize", "Normalize");
    unary(MatNodeType::Saturate, "Saturate", "Saturate");
    unary(MatNodeType::OneMinus, "OneMinus", "One Minus");
    unary(MatNodeType::Sine, "Sine", "Sine");
    unary(MatNodeType::Cosine, "Cosine", "Cosine");

    set(MatNodeType::ComponentMask, "ComponentMask", "Component Mask", "Utility",
        { pin("In", MatType::Vec4) }, { pin("Out", MatType::Float) });
    set(MatNodeType::Append, "Append", "Append", "Utility",
        { pin("A", MatType::Float), pin("B", MatType::Float) },
        { pin("Out", MatType::Vec2) });
    set(MatNodeType::Split, "Split", "Split", "Utility",
        { pin("In", MatType::Vec4) },
        {
            pin("X", MatType::Float), pin("Y", MatType::Float),
            pin("Z", MatType::Float), pin("W", MatType::Float),
        });
    set(MatNodeType::Panner, "Panner", "Panner", "Utility",
        {
            pin("UV", MatType::Vec2, 0.0f, 0.0f, 0.0f, 0.0f, "TexCoord"),
            pin("Time", MatType::Float, 0.0f, 0.0f, 0.0f, 0.0f, "uTime"),
        },
        { pin("Out", MatType::Vec2) });
    set(MatNodeType::Rotator, "Rotator", "Rotator", "Utility",
        {
            pin("UV", MatType::Vec2, 0.0f, 0.0f, 0.0f, 0.0f, "TexCoord"),
            pin("Time", MatType::Float, 0.0f, 0.0f, 0.0f, 0.0f, "uTime"),
        },
        { pin("Out", MatType::Vec2) });

    return table;
}

const std::vector<MatNodeDesc>& descriptors() {
    static const std::vector<MatNodeDesc> table = build_descriptors();
    return table;
}

// --- Identifier and path validation ----------------------------------------

// Everything the surface template already declares, plus the generator's own
// temporaries. A property that shadows one of these compiles into something that
// looks like a driver bug, so it is refused with a name the author can see.
bool is_reserved_property_name(const std::string& name) {
    static const char* kReserved[] = {
        "uTime", "uEnableUE4Lighting", "uAmbientCube", "uMVP", "uModel",
        "uLightSpaceMatrix", "uSkinned", "uBones",
        "gPosition", "gNormal", "gAlbedoSpec", "gPBR", "gBakedGI",
        "FragPos", "Normal", "ourColor", "TexCoord", "FragPosLightSpace",
        "Albedo", "Metallic", "Roughness", "Emissive", "Clearcoat",
        "ClearcoatRoughness", "Sheen", "Subsurface", "ShadingNormal",
        "main", "texture", "vec2", "vec3", "vec4", "float", "int", "bool",
    };
    for (const char* reserved : kReserved) {
        if (name == reserved) return true;
    }
    // Generated temporaries are mgN / mgN_x. Anything shaped like one is refused
    // rather than allowed to shadow a node's value halfway down the shader.
    if (name.size() > 2 && name[0] == 'm' && name[1] == 'g' &&
        std::isdigit(static_cast<unsigned char>(name[2]))) {
        return true;
    }
    return name.rfind("gl_", 0) == 0;
}

bool valid_identifier(const std::string& name) {
    if (name.empty() || name.size() > 60) return false;
    if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_') return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

// The path is copied verbatim into `properties { texture uX = <path> }`, which the
// property parser reads to end of line. Anything that could end the line, end the
// block or start a comment would let a pasted path rewrite the shader around it.
bool valid_texture_path(const std::string& path, std::string& out_error) {
    if (path.size() > 512) { out_error = "texture path is too long"; return false; }
    for (unsigned char c : path) {
        if (c < 0x20 || c == 0x7f) {
            out_error = "texture path contains a control character";
            return false;
        }
        if (c == '{' || c == '}' || c == '"' || c == '\'' || c == '\\') {
            out_error = std::string("texture path contains '") + static_cast<char>(c) +
                        "'; use forward slashes and no quotes";
            return false;
        }
    }
    if (path.find("//") != std::string::npos || path.find("/*") != std::string::npos) {
        out_error = "texture path contains a comment marker";
        return false;
    }
    return true;
}

} // namespace

// --- Descriptor accessors --------------------------------------------------

const MatNodeDesc& material_node_desc(MatNodeType type) {
    const std::vector<MatNodeDesc>& table = descriptors();
    const size_t index = static_cast<size_t>(type);
    return table[index < table.size() ? index : 0];
}

bool material_node_type_from_key(const std::string& key, MatNodeType& out_type) {
    const std::vector<MatNodeDesc>& table = descriptors();
    for (size_t i = 0; i < table.size(); ++i) {
        if (key == table[i].key) {
            out_type = static_cast<MatNodeType>(i);
            return true;
        }
    }
    return false;
}

const char* material_type_name(MatType type) {
    switch (type) {
        case MatType::Float: return "float";
        case MatType::Vec2: return "vec2";
        case MatType::Vec3: return "vec3";
        case MatType::Vec4: return "vec4";
    }
    return "float";
}

// --- Graph -----------------------------------------------------------------

MaterialGraph::MaterialGraph() { reset(); }

void MaterialGraph::reset() {
    nodes.clear();
    links.clear();
    surface_line_nodes.clear();
    next_id = 1;
    output_node_id = add_node(MatNodeType::MaterialOutput, 420.0f, 120.0f);
}

int MaterialGraph::add_node(MatNodeType type, float x, float y) {
    const MatNodeDesc& desc = material_node_desc(type);

    MatNode node;
    node.id = next_id++;
    node.type = type;
    node.x = x;
    node.y = y;
    node.input_defaults.resize(desc.inputs.size());
    for (size_t i = 0; i < desc.inputs.size(); ++i) {
        for (int c = 0; c < 4; ++c) node.input_defaults[i][c] = desc.inputs[i].def[c];
    }

    switch (type) {
        case MatNodeType::ConstantFloat: node.params = { 1.0f, 0.0f, 0.0f, 0.0f }; break;
        case MatNodeType::ConstantVec2:  node.params = { 0.0f, 0.0f, 0.0f, 0.0f }; break;
        case MatNodeType::ConstantVec3:  node.params = { 1.0f, 1.0f, 1.0f, 0.0f }; break;
        case MatNodeType::ConstantVec4:  node.params = { 1.0f, 1.0f, 1.0f, 1.0f }; break;
        case MatNodeType::TexCoord:      node.params = { 1.0f, 1.0f, 0.0f, 0.0f }; break;
        case MatNodeType::Time:          node.params = { 1.0f, 0.0f, 0.0f, 0.0f }; break;
        case MatNodeType::Panner:        node.params = { 0.1f, 0.0f, 0.0f, 0.0f }; break;
        case MatNodeType::Rotator:       node.params = { 0.5f, 0.5f, 1.0f, 0.0f }; break;
        default: break;
    }

    nodes.push_back(std::move(node));
    return nodes.back().id;
}

bool MaterialGraph::remove_node(int node_id) {
    if (node_id == output_node_id) return false;
    const int index = node_index(node_id);
    if (index < 0) return false;

    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const MatLink& l) {
                                   return l.from_node == node_id || l.to_node == node_id;
                               }),
                links.end());
    nodes.erase(nodes.begin() + index);
    return true;
}

bool MaterialGraph::remove_link(int link_id) {
    const size_t before = links.size();
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const MatLink& l) { return l.id == link_id; }),
                links.end());
    return links.size() != before;
}

MatNode* MaterialGraph::find_node(int node_id) {
    const int index = node_index(node_id);
    return index < 0 ? nullptr : &nodes[static_cast<size_t>(index)];
}

const MatNode* MaterialGraph::find_node(int node_id) const {
    const int index = node_index(node_id);
    return index < 0 ? nullptr : &nodes[static_cast<size_t>(index)];
}

int MaterialGraph::node_index(int node_id) const {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == node_id) return static_cast<int>(i);
    }
    return -1;
}

const MatLink* MaterialGraph::link_into(int node_id, int slot) const {
    for (const MatLink& link : links) {
        if (link.to_node == node_id && link.to_slot == slot) return &link;
    }
    return nullptr;
}

bool MaterialGraph::output_is_used(int node_id, int slot) const {
    for (const MatLink& link : links) {
        if (link.from_node == node_id && link.from_slot == slot) return true;
    }
    return false;
}

bool MaterialGraph::decode_pin(int pin_id, int& out_node, int& out_slot, bool& out_is_output) {
    if (pin_id < kPinStride) return false;
    out_node = pin_id / kPinStride;
    int slot = pin_id % kPinStride;
    out_is_output = slot >= kOutputPinBase;
    out_slot = out_is_output ? slot - kOutputPinBase : slot;
    return true;
}

// Depth-first reachability over the existing links. Used before adding an edge, so
// the graph it walks is always acyclic and the recursion always terminates.
bool MaterialGraph::reaches(int from_node, int target_node) const {
    if (from_node == target_node) return true;
    std::vector<int> stack{ from_node };
    std::vector<int> seen;
    while (!stack.empty()) {
        const int current = stack.back();
        stack.pop_back();
        if (std::find(seen.begin(), seen.end(), current) != seen.end()) continue;
        seen.push_back(current);
        for (const MatLink& link : links) {
            if (link.from_node != current) continue;
            if (link.to_node == target_node) return true;
            stack.push_back(link.to_node);
        }
    }
    return false;
}

// --- Type inference --------------------------------------------------------

namespace {

// Works out one node's output types (and the type it promotes its operands to) from
// the types arriving at its inputs. The single place the rules live: link validation,
// whole-graph validation and code generation all call this rather than each having
// their own idea of what an Append produces.
bool infer_node(const MatNode& node, const std::vector<MatType>& in,
                std::vector<MatType>& out, MatType& op, std::string& error) {
    const MatNodeDesc& desc = material_node_desc(node.type);
    out.assign(desc.outputs.size(), MatType::Float);
    for (size_t i = 0; i < desc.outputs.size(); ++i) out[i] = desc.outputs[i].type;
    op = in.empty() ? MatType::Float : in[0];

    const auto combine_all = [&](MatType& result) -> bool {
        result = in.empty() ? MatType::Float : in[0];
        for (size_t i = 1; i < in.size(); ++i) {
            MatType merged = MatType::Float;
            if (!combine(result, in[i], merged)) {
                error = std::string(desc.display) + ": cannot combine " +
                        material_type_name(result) + " and " + material_type_name(in[i]) +
                        ". Mask or append one of them so both sides are the same width.";
                return false;
            }
            result = merged;
        }
        return true;
    };

    switch (node.type) {
        case MatNodeType::MaterialOutput:
            // Output pins have fixed types; the check that an incoming value fits
            // happens per link, in accepts_input().
            op = MatType::Float;
            return true;

        case MatNodeType::TextureSample:
            if (in[0] != MatType::Vec2) {
                error = "Texture Sample: UV must be a vec2.";
                return false;
            }
            op = MatType::Vec2;
            return true;

        case MatNodeType::ConstantFloat:
        case MatNodeType::ConstantVec2:
        case MatNodeType::ConstantVec3:
        case MatNodeType::ConstantVec4:
        case MatNodeType::VertexColor:
        case MatNodeType::TexCoord:
        case MatNodeType::Time:
        case MatNodeType::Normal:
        case MatNodeType::WorldPosition:
        case MatNodeType::CameraVector:
            return true;

        case MatNodeType::Fresnel:
            if (in[0] != MatType::Float || in[1] != MatType::Float) {
                error = "Fresnel: exponent and base reflectance are scalars.";
                return false;
            }
            op = MatType::Float;
            return true;

        case MatNodeType::Add:
        case MatNodeType::Subtract:
        case MatNodeType::Multiply:
        case MatNodeType::Divide:
        case MatNodeType::Power:
        case MatNodeType::Min:
        case MatNodeType::Max:
        case MatNodeType::Step:
        case MatNodeType::Lerp:
        case MatNodeType::Clamp:
        case MatNodeType::Smoothstep:
            if (!combine_all(op)) return false;
            out[0] = op;
            return true;

        case MatNodeType::Dot:
            if (!combine_all(op)) return false;
            out[0] = MatType::Float;
            return true;

        case MatNodeType::Cross:
            for (size_t i = 0; i < in.size(); ++i) {
                if (in[i] != MatType::Vec3 && in[i] != MatType::Float) {
                    error = "Cross Product: both inputs must be vec3.";
                    return false;
                }
            }
            op = MatType::Vec3;
            out[0] = MatType::Vec3;
            return true;

        case MatNodeType::Normalize:
            if (in[0] == MatType::Float) {
                error = "Normalize: a scalar has no direction. Feed it a vector.";
                return false;
            }
            op = in[0];
            out[0] = in[0];
            return true;

        case MatNodeType::Saturate:
        case MatNodeType::OneMinus:
        case MatNodeType::Sine:
        case MatNodeType::Cosine:
            op = in[0];
            out[0] = in[0];
            return true;

        case MatNodeType::ComponentMask: {
            int width = 0;
            for (int c = 0; c < 4; ++c) {
                if (!node.mask[static_cast<size_t>(c)]) continue;
                ++width;
                if (c >= dim(in[0])) {
                    error = std::string("Component Mask: a ") + material_type_name(in[0]) +
                            " has no ." + kSwizzle[c] + " component.";
                    return false;
                }
            }
            if (width == 0) {
                error = "Component Mask: select at least one channel.";
                return false;
            }
            op = in[0];
            out[0] = type_of_dim(width);
            return true;
        }

        case MatNodeType::Append: {
            const int width = dim(in[0]) + dim(in[1]);
            if (width > 4) {
                error = std::string("Append: ") + material_type_name(in[0]) + " and " +
                        material_type_name(in[1]) + " make " + std::to_string(width) +
                        " components; the widest type is vec4.";
                return false;
            }
            out[0] = type_of_dim(width);
            return true;
        }

        case MatNodeType::Split:
            // Every output stays float; whether a given one exists depends on the
            // input width and is checked where the link is, in accepts_output().
            op = in[0];
            return true;

        case MatNodeType::Panner:
        case MatNodeType::Rotator:
            if (in[0] != MatType::Vec2) {
                error = std::string(desc.display) + ": UV must be a vec2.";
                return false;
            }
            if (in[1] != MatType::Float) {
                error = std::string(desc.display) + ": Time must be a scalar.";
                return false;
            }
            op = MatType::Vec2;
            return true;

        case MatNodeType::Count:
            break;
    }

    error = "Unknown node type.";
    return false;
}

} // namespace

bool MaterialGraph::resolve(MatResolved& out, std::string& out_error,
                            int& out_error_node) const {
    out_error.clear();
    out_error_node = 0;
    const size_t count = nodes.size();
    out.inputs.assign(count, {});
    out.outputs.assign(count, {});
    out.op.assign(count, MatType::Float);
    out.order.clear();
    out.order.reserve(count);

    // Kahn's algorithm over the whole graph, not just the part feeding the output.
    // A cycle anywhere is rejected: it cannot be generated later and finding it now
    // means the message names the nodes the author just wired, not ones they forgot
    // about three sessions ago.
    std::vector<int> remaining_inputs(count, 0);
    for (const MatLink& link : links) {
        const int to = node_index(link.to_node);
        if (to >= 0) ++remaining_inputs[static_cast<size_t>(to)];
    }

    std::vector<int> ready;
    for (size_t i = 0; i < count; ++i) {
        if (remaining_inputs[i] == 0) ready.push_back(static_cast<int>(i));
    }

    while (!ready.empty()) {
        const int index = ready.back();
        ready.pop_back();
        out.order.push_back(index);

        const MatNode& node = nodes[static_cast<size_t>(index)];
        const MatNodeDesc& desc = material_node_desc(node.type);

        std::vector<MatType> in(desc.inputs.size(), MatType::Float);
        for (size_t slot = 0; slot < desc.inputs.size(); ++slot) {
            const MatLink* link = link_into(node.id, static_cast<int>(slot));
            if (!link) {
                in[slot] = desc.inputs[slot].type;
                continue;
            }
            const int source = node_index(link->from_node);
            if (source < 0 ||
                static_cast<size_t>(link->from_slot) >= out.outputs[static_cast<size_t>(source)].size()) {
                out_error = "A link points at a pin that no longer exists.";
                out_error_node = node.id;
                return false;
            }
            in[slot] = out.outputs[static_cast<size_t>(source)][static_cast<size_t>(link->from_slot)];
        }

        std::string error;
        std::vector<MatType> produced;
        MatType op = MatType::Float;
        if (!infer_node(node, in, produced, op, error)) {
            out_error = error;
            out_error_node = node.id;
            return false;
        }

        out.inputs[static_cast<size_t>(index)] = in;
        out.outputs[static_cast<size_t>(index)] = produced;
        out.op[static_cast<size_t>(index)] = op;

        for (const MatLink& link : links) {
            if (link.from_node != node.id) continue;
            const int to = node_index(link.to_node);
            if (to < 0) continue;
            if (--remaining_inputs[static_cast<size_t>(to)] == 0) ready.push_back(to);
        }
    }

    if (out.order.size() != count) {
        // Name one of the nodes still stuck, so the panel can point at the loop.
        for (size_t i = 0; i < count; ++i) {
            if (std::find(out.order.begin(), out.order.end(), static_cast<int>(i)) ==
                out.order.end()) {
                out_error_node = nodes[i].id;
                break;
            }
        }
        out_error = "The graph contains a cycle. A material is evaluated once per "
                    "pixel, so a value cannot depend on itself.";
        return false;
    }

    // Split's outputs exist only up to the width of its input. Checked after
    // inference because it depends on links leaving the node, not entering it.
    for (size_t i = 0; i < count; ++i) {
        if (nodes[i].type != MatNodeType::Split) continue;
        const int width = dim(out.inputs[i][0]);
        for (const MatLink& link : links) {
            if (link.from_node != nodes[i].id) continue;
            if (link.from_slot < width) continue;
            out_error = std::string("Split: a ") + material_type_name(out.inputs[i][0]) +
                        " has no ." + kSwizzle[link.from_slot] + " component.";
            out_error_node = nodes[i].id;
            return false;
        }
    }

    return true;
}

// --- Linking ---------------------------------------------------------------

bool MaterialGraph::try_add_link(int from_node, int from_slot, int to_node, int to_slot,
                                 std::string& out_error) {
    out_error.clear();

    const MatNode* source = find_node(from_node);
    const MatNode* destination = find_node(to_node);
    if (!source || !destination) {
        out_error = "One end of the link is not a node in this graph.";
        return false;
    }
    if (from_node == to_node) {
        out_error = "A node cannot feed itself.";
        return false;
    }
    if (static_cast<size_t>(from_slot) >= material_node_desc(source->type).outputs.size() ||
        static_cast<size_t>(to_slot) >= material_node_desc(destination->type).inputs.size()) {
        out_error = "That pin does not exist.";
        return false;
    }
    if (reaches(to_node, from_node)) {
        out_error = "That link would create a cycle.";
        return false;
    }

    // How the graph looked before, so a pre-existing error (an author mid-edit on a
    // mask, say) is not blamed on the link they are drawing right now.
    MatResolved before_state;
    std::string before_error;
    int before_node = 0;
    const bool was_valid = resolve(before_state, before_error, before_node);

    std::vector<MatLink> saved = links;
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const MatLink& l) {
                                   return l.to_node == to_node && l.to_slot == to_slot;
                               }),
                links.end());

    MatLink link;
    link.id = next_id++;
    link.from_node = from_node;
    link.from_slot = from_slot;
    link.to_node = to_node;
    link.to_slot = to_slot;
    links.push_back(link);

    MatResolved after_state;
    std::string after_error;
    int after_node = 0;
    if (resolve(after_state, after_error, after_node)) {
        // The destination's own rules pass. The output node is the one place a pin
        // has a fixed width that inference cannot widen, so it is checked here.
        if (destination->type == MatNodeType::MaterialOutput) {
            const MatType want = material_node_desc(MatNodeType::MaterialOutput)
                                     .inputs[static_cast<size_t>(to_slot)].type;
            const MatType got = after_state.inputs[static_cast<size_t>(node_index(to_node))]
                                                  [static_cast<size_t>(to_slot)];
            if (got != want && got != MatType::Float) {
                out_error = std::string("Material Output '") +
                            material_node_desc(MatNodeType::MaterialOutput)
                                .inputs[static_cast<size_t>(to_slot)].name +
                            "' takes a " + material_type_name(want) + ", not a " +
                            material_type_name(got) +
                            ". Use a Component Mask or Append to reshape it.";
                links = std::move(saved);
                return false;
            }
        }
        return true;
    }

    if (was_valid || after_error != before_error || after_node != before_node) {
        out_error = after_error;
        links = std::move(saved);
        --next_id;
        return false;
    }

    // The graph was already failing this way before the link. Let it through rather
    // than blaming the author's new link for an error somewhere else.
    return true;
}

// --- Code generation -------------------------------------------------------

namespace {

std::string node_var(int node_id) { return "mg" + std::to_string(node_id); }

// The expression that reads one of a node's outputs. Most nodes declare a single
// variable; texture samples and splits declare one wide variable and hand out
// swizzles of it, which keeps the generated code to one statement per node.
std::string output_expr(const MatNode& node, int slot) {
    const std::string var = node_var(node.id);
    if (node.type == MatNodeType::TextureSample) {
        static const char* kParts[] = { ".rgb", ".r", ".g", ".b", ".a" };
        if (slot >= 0 && slot < 5) return var + kParts[slot];
        return var;
    }
    if (node.type == MatNodeType::Split) {
        if (slot >= 0 && slot < 4) return var + "." + std::string(1, kSwizzle[slot]);
        return var;
    }
    return var;
}

std::string sanitized_property_name(const MatNode& node, const char* prefix,
                                    std::string& out_error) {
    if (node.param_name.empty()) return prefix + std::to_string(node.id);
    if (!valid_identifier(node.param_name)) {
        out_error = "'" + node.param_name +
                    "' is not a usable property name. Use letters, digits and "
                    "underscores, starting with a letter.";
        return "";
    }
    if (is_reserved_property_name(node.param_name)) {
        out_error = "'" + node.param_name +
                    "' is already used by the engine's own shader plumbing. Pick "
                    "another name.";
        return "";
    }
    return node.param_name;
}

} // namespace

MatCompileResult MaterialGraph::generate() {
    MatCompileResult result;
    surface_line_nodes.clear();

    MatResolved types;
    if (!resolve(types, result.error, result.error_node)) return result;

    const int output_index = node_index(output_node_id);
    if (output_index < 0) {
        result.error = "The graph has no Material Output node.";
        return result;
    }

    // Only the part of the graph that reaches the output is generated. An orphaned
    // subgraph is a work in progress, not an error, and emitting it would put dead
    // code and unused texture units into every material.
    std::vector<int> contributing;
    {
        std::vector<int> stack{ output_index };
        while (!stack.empty()) {
            const int index = stack.back();
            stack.pop_back();
            if (std::find(contributing.begin(), contributing.end(), index) != contributing.end()) {
                continue;
            }
            contributing.push_back(index);
            const MatNode& node = nodes[static_cast<size_t>(index)];
            for (const MatLink& link : links) {
                if (link.to_node != node.id) continue;
                const int source = node_index(link.from_node);
                if (source >= 0) stack.push_back(source);
            }
        }
    }

    // resolve() already produced a topological order for the whole graph; keeping
    // its relative order guarantees every value is declared before it is read.
    std::vector<int> emit_order;
    for (int index : types.order) {
        if (std::find(contributing.begin(), contributing.end(), index) != contributing.end()) {
            emit_order.push_back(index);
        }
    }

    // --- Properties --------------------------------------------------------
    std::string properties;
    std::vector<std::string> property_names;
    const auto claim_name = [&](const std::string& name, int node_id) -> bool {
        for (const std::string& existing : property_names) {
            if (existing != name) continue;
            result.error = "Two nodes both want the property name '" + name + "'.";
            result.error_node = node_id;
            return false;
        }
        property_names.push_back(name);
        return true;
    };

    for (int index : emit_order) {
        const MatNode& node = nodes[static_cast<size_t>(index)];
        if (node.type == MatNodeType::TextureSample) {
            std::string error;
            const std::string name = sanitized_property_name(node, "uTex", error);
            if (name.empty()) {
                result.error = error;
                result.error_node = node.id;
                return result;
            }
            if (!claim_name(name, node.id)) return result;
            if (!valid_texture_path(node.texture_path, error)) {
                result.error = "Texture Sample: " + error;
                result.error_node = node.id;
                return result;
            }
            properties += "    texture " + name;
            if (!node.texture_path.empty()) properties += " = " + node.texture_path;
            properties += "\n";
        } else if (node.exposed && (node.type == MatNodeType::ConstantFloat ||
                                    node.type == MatNodeType::ConstantVec3)) {
            std::string error;
            const std::string name = sanitized_property_name(node, "uParam", error);
            if (name.empty()) {
                result.error = error;
                result.error_node = node.id;
                return result;
            }
            if (!claim_name(name, node.id)) return result;
            if (node.type == MatNodeType::ConstantFloat) {
                properties += "    float " + name + " = " + gl_float(node.params[0]) + "\n";
            } else {
                properties += "    color " + name + " = " + gl_float(node.params[0]) + ", " +
                              gl_float(node.params[1]) + ", " + gl_float(node.params[2]) + "\n";
            }
        }
    }

    // --- Surface body ------------------------------------------------------
    std::string body;
    const auto emit = [&](int node_id, const std::string& statement) {
        body += "    " + statement + "\n";
        surface_line_nodes.push_back(node_id);
    };

    // Expression arriving at one input pin, before promotion.
    const auto raw_input = [&](const MatNode& node, int slot) -> std::string {
        const MatNodeDesc& desc = material_node_desc(node.type);
        if (const MatLink* link = link_into(node.id, slot)) {
            const MatNode* source = find_node(link->from_node);
            if (source) return output_expr(*source, link->from_slot);
        }
        const MatPinDesc& pin_desc = desc.inputs[static_cast<size_t>(slot)];
        if (pin_desc.fallback) return pin_desc.fallback;
        return gl_literal(pin_desc.type, node.input_defaults[static_cast<size_t>(slot)].data());
    };

    // ...and the same expression widened to the type the node operates in.
    const auto operand = [&](const MatNode& node, int index, int slot, MatType want) {
        const MatType have = types.inputs[static_cast<size_t>(index)][static_cast<size_t>(slot)];
        return promote(raw_input(node, slot), have, want);
    };

    // Which property name each texture node ended up with, so the sample statement
    // and the properties block agree.
    size_t property_cursor = 0;
    std::vector<std::pair<int, std::string>> texture_names;
    for (int index : emit_order) {
        const MatNode& node = nodes[static_cast<size_t>(index)];
        if (node.type != MatNodeType::TextureSample) continue;
        (void)property_cursor;
        std::string error;
        texture_names.emplace_back(node.id, sanitized_property_name(node, "uTex", error));
    }
    // Uniform an exposed constant reads from, rather than its literal value. The
    // properties block above already validated and claimed this name, so a failure
    // here cannot be new - the name is all that is wanted.
    const auto exposed_param_name = [](const MatNode& node) -> std::string {
        std::string ignored;
        return sanitized_property_name(node, "uParam", ignored);
    };

    const auto texture_name_for = [&](int node_id) -> std::string {
        for (const auto& entry : texture_names) {
            if (entry.first == node_id) return entry.second;
        }
        return "uTex" + std::to_string(node_id);
    };

    for (int index : emit_order) {
        const MatNode& node = nodes[static_cast<size_t>(index)];
        if (node.type == MatNodeType::MaterialOutput) continue;

        const std::string var = node_var(node.id);
        const MatType op = types.op[static_cast<size_t>(index)];
        const std::vector<MatType>& outs = types.outputs[static_cast<size_t>(index)];
        const std::string out_type = material_type_name(outs.empty() ? MatType::Float : outs[0]);

        switch (node.type) {
            case MatNodeType::TextureSample:
                emit(node.id, "vec4 " + var + " = texture(" + texture_name_for(node.id) + ", " +
                                  raw_input(node, 0) + ");");
                break;

            case MatNodeType::ConstantFloat:
                emit(node.id, "float " + var + " = " +
                                  (node.exposed ? exposed_param_name(node) : gl_float(node.params[0])) +
                                  ";");
                break;

            case MatNodeType::ConstantVec2:
                emit(node.id, "vec2 " + var + " = " + gl_literal(MatType::Vec2, node.params.data()) + ";");
                break;

            case MatNodeType::ConstantVec3:
                emit(node.id, "vec3 " + var + " = " +
                                  (node.exposed ? exposed_param_name(node)
                                                : gl_literal(MatType::Vec3, node.params.data())) +
                                  ";");
                break;

            case MatNodeType::ConstantVec4:
                emit(node.id, "vec4 " + var + " = " + gl_literal(MatType::Vec4, node.params.data()) + ";");
                break;

            case MatNodeType::VertexColor:
                emit(node.id, "vec3 " + var + " = ourColor;");
                break;

            case MatNodeType::TexCoord:
                emit(node.id, "vec2 " + var + " = TexCoord * vec2(" + gl_float(node.params[0]) +
                                  ", " + gl_float(node.params[1]) + ");");
                break;

            case MatNodeType::Time:
                emit(node.id, "float " + var + " = uTime * " + gl_float(node.params[0]) + ";");
                break;

            case MatNodeType::Fresnel:
                // Camera-relative space: the geometry vertex stage multiplies by a
                // model matrix already relative to the camera, so the eye is at the
                // origin and the view vector is just -FragPos.
                emit(node.id, "float " + var + " = " + operand(node, index, 1, MatType::Float) +
                                  " + (1.0 - " + operand(node, index, 1, MatType::Float) +
                                  ") * pow(1.0 - clamp(dot(normalize(Normal), "
                                  "normalize(-FragPos)), 0.0, 1.0), max(" +
                                  operand(node, index, 0, MatType::Float) + ", 0.0001));");
                break;

            case MatNodeType::Normal:
                emit(node.id, "vec3 " + var + " = normalize(Normal);");
                break;

            case MatNodeType::WorldPosition:
                emit(node.id, "vec3 " + var + " = FragPos;   // camera-relative (large-world coords)");
                break;

            case MatNodeType::CameraVector:
                emit(node.id, "vec3 " + var + " = normalize(-FragPos);");
                break;

            case MatNodeType::Add:
                emit(node.id, out_type + " " + var + " = " + operand(node, index, 0, op) + " + " +
                                  operand(node, index, 1, op) + ";");
                break;
            case MatNodeType::Subtract:
                emit(node.id, out_type + " " + var + " = " + operand(node, index, 0, op) + " - " +
                                  operand(node, index, 1, op) + ";");
                break;
            case MatNodeType::Multiply:
                emit(node.id, out_type + " " + var + " = " + operand(node, index, 0, op) + " * " +
                                  operand(node, index, 1, op) + ";");
                break;
            case MatNodeType::Divide:
                emit(node.id, out_type + " " + var + " = " + operand(node, index, 0, op) + " / " +
                                  operand(node, index, 1, op) + ";");
                break;

            case MatNodeType::Lerp:
                emit(node.id, out_type + " " + var + " = mix(" + operand(node, index, 0, op) + ", " +
                                  operand(node, index, 1, op) + ", " + operand(node, index, 2, op) + ");");
                break;

            case MatNodeType::Dot:
                emit(node.id, "float " + var + " = dot(" + operand(node, index, 0, op) + ", " +
                                  operand(node, index, 1, op) + ");");
                break;

            case MatNodeType::Cross:
                emit(node.id, "vec3 " + var + " = cross(" + operand(node, index, 0, MatType::Vec3) +
                                  ", " + operand(node, index, 1, MatType::Vec3) + ");");
                break;

            case MatNodeType::Normalize:
                emit(node.id, out_type + " " + var + " = normalize(" + operand(node, index, 0, op) + ");");
                break;

            case MatNodeType::Saturate:
                emit(node.id, out_type + " " + var + " = clamp(" + operand(node, index, 0, op) + ", " +
                                  out_type + "(0.0), " + out_type + "(1.0));");
                break;

            case MatNodeType::Power:
                // pow() of a negative base is undefined in GLSL and comes back as
                // nan on this driver, which shows up as black speckle rather than
                // an error. Clamping the base is cheaper than explaining that.
                emit(node.id, out_type + " " + var + " = pow(max(" + operand(node, index, 0, op) +
                                  ", " + out_type + "(0.0)), " + operand(node, index, 1, op) + ");");
                break;

            case MatNodeType::Clamp:
                emit(node.id, out_type + " " + var + " = clamp(" + operand(node, index, 0, op) + ", " +
                                  operand(node, index, 1, op) + ", " + operand(node, index, 2, op) + ");");
                break;

            case MatNodeType::Min:
                emit(node.id, out_type + " " + var + " = min(" + operand(node, index, 0, op) + ", " +
                                  operand(node, index, 1, op) + ");");
                break;
            case MatNodeType::Max:
                emit(node.id, out_type + " " + var + " = max(" + operand(node, index, 0, op) + ", " +
                                  operand(node, index, 1, op) + ");");
                break;

            case MatNodeType::OneMinus:
                emit(node.id, out_type + " " + var + " = " + out_type + "(1.0) - " +
                                  operand(node, index, 0, op) + ";");
                break;

            case MatNodeType::Sine:
                emit(node.id, out_type + " " + var + " = sin(" + operand(node, index, 0, op) + ");");
                break;
            case MatNodeType::Cosine:
                emit(node.id, out_type + " " + var + " = cos(" + operand(node, index, 0, op) + ");");
                break;

            case MatNodeType::Step:
                emit(node.id, out_type + " " + var + " = step(" + operand(node, index, 0, op) + ", " +
                                  operand(node, index, 1, op) + ");");
                break;

            case MatNodeType::Smoothstep:
                emit(node.id, out_type + " " + var + " = smoothstep(" + operand(node, index, 0, op) +
                                  ", " + operand(node, index, 1, op) + ", " +
                                  operand(node, index, 2, op) + ");");
                break;

            case MatNodeType::ComponentMask: {
                std::string swizzle;
                for (int c = 0; c < 4; ++c) {
                    if (node.mask[static_cast<size_t>(c)]) swizzle += kSwizzle[c];
                }
                emit(node.id, out_type + " " + var + " = " + raw_input(node, 0) + "." + swizzle + ";");
                break;
            }

            case MatNodeType::Append:
                emit(node.id, out_type + " " + var + " = " + out_type + "(" + raw_input(node, 0) +
                                  ", " + raw_input(node, 1) + ");");
                break;

            case MatNodeType::Split:
                emit(node.id, std::string(material_type_name(op)) + " " + var + " = " +
                                  raw_input(node, 0) + ";");
                break;

            case MatNodeType::Panner:
                emit(node.id, "vec2 " + var + " = " + raw_input(node, 0) + " + " +
                                  raw_input(node, 1) + " * vec2(" + gl_float(node.params[0]) + ", " +
                                  gl_float(node.params[1]) + ");");
                break;

            case MatNodeType::Rotator: {
                const std::string angle = var + "_a";
                const std::string centre = "vec2(" + gl_float(node.params[0]) + ", " +
                                           gl_float(node.params[1]) + ")";
                emit(node.id, "float " + angle + " = " + raw_input(node, 1) + " * " +
                                  gl_float(node.params[2]) + ";");
                emit(node.id, "vec2 " + var + "_d = " + raw_input(node, 0) + " - " + centre + ";");
                emit(node.id, "vec2 " + var + " = " + centre + " + vec2(" + var + "_d.x * cos(" +
                                  angle + ") - " + var + "_d.y * sin(" + angle + "), " + var +
                                  "_d.x * sin(" + angle + ") + " + var + "_d.y * cos(" + angle + "));");
                break;
            }

            case MatNodeType::MaterialOutput:
            case MatNodeType::Count:
                break;
        }
    }

    // --- Output assignments ------------------------------------------------
    // Only connected pins are assigned. An unconnected pin keeps the value the
    // surface template gave it, which is how a graph that only sets base colour
    // still gets sensible roughness and a correct shading normal.
    static const char* kOutputVariables[] = {
        "Albedo", "Metallic", "Roughness", "Emissive", "Clearcoat",
        "ClearcoatRoughness", "Sheen", "Subsurface", "ShadingNormal",
    };
    const MatNode& output_node = nodes[static_cast<size_t>(output_index)];
    const MatNodeDesc& output_desc = material_node_desc(MatNodeType::MaterialOutput);
    bool wrote_any = false;
    for (size_t slot = 0; slot < output_desc.inputs.size(); ++slot) {
        const MatLink* link = link_into(output_node.id, static_cast<int>(slot));
        if (!link) continue;
        const MatType want = output_desc.inputs[slot].type;
        const MatType have = types.inputs[static_cast<size_t>(output_index)][slot];
        if (have != want && have != MatType::Float) {
            result.error = std::string("Material Output '") + output_desc.inputs[slot].name +
                           "' takes a " + material_type_name(want) + ", not a " +
                           material_type_name(have) + ".";
            result.error_node = output_node.id;
            return result;
        }
        const MatNode* source = find_node(link->from_node);
        if (!source) continue;
        emit(output_node.id, std::string(kOutputVariables[slot]) + " = " +
                                 promote(output_expr(*source, link->from_slot), have, want) + ";");
        wrote_any = true;
    }
    if (!wrote_any) {
        emit(output_node.id, "// Nothing is connected to the Material Output yet.");
    }

    // --- Assemble ----------------------------------------------------------
    std::string source =
        "// Generated by the Lithium material graph. Edit the .lgraph beside it,\n"
        "// not this file - the next save overwrites whatever is here.\n\n";
    if (!properties.empty()) source += "properties {\n" + properties + "}\n\n";
    source += "surface {\n" + body + "}\n";

    result.ok = true;
    result.source = std::move(source);
    return result;
}

int MaterialGraph::node_for_surface_line(int line) const {
    if (line < 1 || static_cast<size_t>(line) > surface_line_nodes.size()) return 0;
    return surface_line_nodes[static_cast<size_t>(line - 1)];
}

// --- Serialisation ---------------------------------------------------------

std::string MaterialGraph::to_json() const {
    json doc;
    doc["version"] = 1;
    doc["next_id"] = next_id;
    doc["output_node"] = output_node_id;

    json node_array = json::array();
    for (const MatNode& node : nodes) {
        json entry;
        entry["id"] = node.id;
        entry["type"] = material_node_desc(node.type).key;
        entry["pos"] = json::array({ node.x, node.y });
        json defaults = json::array();
        for (const std::array<float, 4>& value : node.input_defaults) {
            defaults.push_back(json::array({ value[0], value[1], value[2], value[3] }));
        }
        entry["inputs"] = defaults;
        entry["params"] = json::array({ node.params[0], node.params[1], node.params[2],
                                        node.params[3] });
        entry["mask"] = json::array({ node.mask[0], node.mask[1], node.mask[2], node.mask[3] });
        entry["texture"] = node.texture_path;
        entry["exposed"] = node.exposed;
        entry["param_name"] = node.param_name;
        node_array.push_back(entry);
    }
    doc["nodes"] = node_array;

    json link_array = json::array();
    for (const MatLink& link : links) {
        json entry;
        entry["id"] = link.id;
        entry["from_node"] = link.from_node;
        entry["from_slot"] = link.from_slot;
        entry["to_node"] = link.to_node;
        entry["to_slot"] = link.to_slot;
        link_array.push_back(entry);
    }
    doc["links"] = link_array;

    return doc.dump(2) + "\n";
}

bool MaterialGraph::from_json(const std::string& text, std::string& out_error) {
    out_error.clear();

    json doc;
    try {
        doc = json::parse(text);
    } catch (const std::exception& error) {
        out_error = std::string("Not a valid material graph: ") + error.what();
        return false;
    }

    if (!doc.is_object() || !doc.contains("nodes") || !doc["nodes"].is_array()) {
        out_error = "Not a material graph: no node list.";
        return false;
    }

    std::vector<MatNode> loaded_nodes;
    std::vector<MatLink> loaded_links;
    int loaded_output = 0;

    try {
        for (const json& entry : doc["nodes"]) {
            MatNode node;
            node.id = entry.value("id", 0);
            if (node.id <= 0) {
                out_error = "A node has no id.";
                return false;
            }
            for (const MatNode& existing : loaded_nodes) {
                if (existing.id != node.id) continue;
                out_error = "Two nodes share id " + std::to_string(node.id) + ".";
                return false;
            }
            const std::string key = entry.value("type", std::string());
            if (!material_node_type_from_key(key, node.type)) {
                out_error = "Unknown node type '" + key + "'.";
                return false;
            }
            if (entry.contains("pos") && entry["pos"].is_array() && entry["pos"].size() == 2) {
                node.x = entry["pos"][0].get<float>();
                node.y = entry["pos"][1].get<float>();
            }

            const size_t input_count = material_node_desc(node.type).inputs.size();
            node.input_defaults.assign(input_count, { 0.0f, 0.0f, 0.0f, 0.0f });
            for (size_t i = 0; i < input_count; ++i) {
                for (int c = 0; c < 4; ++c) {
                    node.input_defaults[i][static_cast<size_t>(c)] =
                        material_node_desc(node.type).inputs[i].def[c];
                }
            }
            if (entry.contains("inputs") && entry["inputs"].is_array()) {
                const json& defaults = entry["inputs"];
                for (size_t i = 0; i < defaults.size() && i < input_count; ++i) {
                    if (!defaults[i].is_array()) continue;
                    for (size_t c = 0; c < 4 && c < defaults[i].size(); ++c) {
                        node.input_defaults[i][c] = defaults[i][c].get<float>();
                    }
                }
            }
            if (entry.contains("params") && entry["params"].is_array()) {
                for (size_t c = 0; c < 4 && c < entry["params"].size(); ++c) {
                    node.params[c] = entry["params"][c].get<float>();
                }
            }
            if (entry.contains("mask") && entry["mask"].is_array()) {
                for (size_t c = 0; c < 4 && c < entry["mask"].size(); ++c) {
                    node.mask[c] = entry["mask"][c].get<bool>();
                }
            }
            node.texture_path = entry.value("texture", std::string());
            node.exposed = entry.value("exposed", false);
            node.param_name = entry.value("param_name", std::string());

            if (node.type == MatNodeType::MaterialOutput && loaded_output == 0) {
                loaded_output = node.id;
            }
            loaded_nodes.push_back(std::move(node));
        }

        if (doc.contains("links") && doc["links"].is_array()) {
            for (const json& entry : doc["links"]) {
                MatLink link;
                link.id = entry.value("id", 0);
                link.from_node = entry.value("from_node", 0);
                link.from_slot = entry.value("from_slot", 0);
                link.to_node = entry.value("to_node", 0);
                link.to_slot = entry.value("to_slot", 0);
                loaded_links.push_back(link);
            }
        }
        loaded_output = doc.value("output_node", loaded_output);
        next_id = doc.value("next_id", 1);
    } catch (const std::exception& error) {
        out_error = std::string("Malformed material graph: ") + error.what();
        return false;
    }

    // Links are validated against the nodes that actually loaded, so a hand-edited
    // file cannot produce a link into a pin that does not exist - which would read
    // past the end of a type vector during inference.
    for (const MatLink& link : loaded_links) {
        const auto find = [&](int id) -> const MatNode* {
            for (const MatNode& node : loaded_nodes) {
                if (node.id == id) return &node;
            }
            return nullptr;
        };
        const MatNode* from = find(link.from_node);
        const MatNode* to = find(link.to_node);
        if (!from || !to) {
            out_error = "A link refers to a node that is not in the file.";
            return false;
        }
        if (static_cast<size_t>(link.from_slot) >= material_node_desc(from->type).outputs.size() ||
            link.from_slot < 0 ||
            static_cast<size_t>(link.to_slot) >= material_node_desc(to->type).inputs.size() ||
            link.to_slot < 0) {
            out_error = "A link refers to a pin that does not exist.";
            return false;
        }
    }

    bool has_output = false;
    for (const MatNode& node : loaded_nodes) {
        if (node.id == loaded_output && node.type == MatNodeType::MaterialOutput) has_output = true;
    }
    if (!has_output) {
        out_error = "The file has no Material Output node.";
        return false;
    }

    // Ids in the file may run ahead of next_id if it was written by hand.
    for (const MatNode& node : loaded_nodes) next_id = std::max(next_id, node.id + 1);
    for (const MatLink& link : loaded_links) next_id = std::max(next_id, link.id + 1);

    nodes = std::move(loaded_nodes);
    links = std::move(loaded_links);
    output_node_id = loaded_output;
    surface_line_nodes.clear();
    return true;
}

bool MaterialGraph::save(const std::string& path) const {
    std::ofstream file(path);
    if (!file) return false;
    file << to_json();
    return static_cast<bool>(file);
}

bool MaterialGraph::load(const std::string& path, std::string& out_error) {
    std::ifstream file(path);
    if (!file) {
        out_error = "Could not open " + path;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return from_json(buffer.str(), out_error);
}
