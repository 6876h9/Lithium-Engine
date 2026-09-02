#pragma once

#include <array>
#include <string>
#include <vector>

// Node-based material authoring.
//
// This is a *source generator*, not a renderer. A graph emits the two blocks of a
// .lshader - `properties { }` and `surface { }` - and hands them to MaterialShader,
// which already knows how to wrap a surface block in the five-target G-buffer
// boilerplate (including gBakedGI at location 4, which every geometry-pass fragment
// shader must write). Nothing in this file or its .cpp touches GL, ImGui or imnodes,
// which is what lets the code generator be exercised headlessly from --selftest.
//
// The trap worth knowing about up front: the Material Output pins are exactly the
// variables the surface template pre-declares, and nothing else. There is no ambient
// occlusion output because the G-buffer has no channel left to carry one - gPBR is
// (metallic, roughness, clearcoat, clearcoat roughness), gNormal.a is subsurface,
// gAlbedoSpec.a is sheen and gPosition.a is emissive. Adding an "AO" pin here would
// generate an assignment to an undeclared variable and fail to compile. Add outputs
// to the template in material_shader.cpp first, then here.

// The component count IS the enum value, so dimension arithmetic (Append widths,
// mask widths) needs no lookup table.
enum class MatType : int { Float = 1, Vec2 = 2, Vec3 = 3, Vec4 = 4 };

enum class MatNodeType : int {
    // --- Output -----------------------------------------------------------
    MaterialOutput = 0,

    // --- Inputs -----------------------------------------------------------
    TextureSample,
    ConstantFloat,
    ConstantVec2,
    ConstantVec3,
    ConstantVec4,
    VertexColor,
    TexCoord,
    Time,
    Fresnel,
    Normal,
    WorldPosition,
    CameraVector,

    // --- Math -------------------------------------------------------------
    Add,
    Subtract,
    Multiply,
    Divide,
    Lerp,
    Dot,
    Cross,
    Normalize,
    Saturate,
    Power,
    Clamp,
    Min,
    Max,
    OneMinus,
    Sine,
    Cosine,
    Step,
    Smoothstep,

    // --- Utility ----------------------------------------------------------
    ComponentMask,
    Append,
    Split,
    Panner,
    Rotator,

    Count
};

struct MatPinDesc {
    const char* name = "";
    MatType type = MatType::Float;
    float def[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // GLSL substituted when the pin is left unconnected, instead of the literal in
    // `def`. A texture sample with nothing wired into UV should read the mesh's own
    // texture coordinates, not the constant (0,0) - a black material with no error
    // is far harder to diagnose than a wrong-looking one.
    const char* fallback = nullptr;
};

struct MatNodeDesc {
    const char* key = "";       // stable identifier written to JSON
    const char* display = "";
    const char* category = "";
    std::vector<MatPinDesc> inputs;
    std::vector<MatPinDesc> outputs;
};

const MatNodeDesc& material_node_desc(MatNodeType type);
bool material_node_type_from_key(const std::string& key, MatNodeType& out_type);
const char* material_type_name(MatType type);

struct MatNode {
    int id = 0;
    MatNodeType type = MatNodeType::ConstantFloat;
    float x = 0.0f;
    float y = 0.0f;

    // One entry per input pin: the value used when the pin is unconnected and the
    // pin has no `fallback`.
    std::vector<std::array<float, 4>> input_defaults;

    // Node-local settings. Which slots mean what depends on the node type, and is
    // documented in material_graph.cpp beside the code that reads them:
    //   Constant*      value
    //   TexCoord       [0]=u tiling  [1]=v tiling
    //   Time           [0]=speed
    //   Panner         [0]=u speed   [1]=v speed
    //   Rotator        [0]=centre u  [1]=centre v  [2]=speed
    std::array<float, 4> params = { 0.0f, 0.0f, 0.0f, 0.0f };

    // ComponentMask channel selection.
    std::array<bool, 4> mask = { true, true, true, false };

    // TextureSample only.
    std::string texture_path;

    // A node whose value should become a shader `properties { }` entry rather than a
    // literal, so one compiled shader can serve many materials with different values.
    // Only float and vec3 can be exposed - those are the only property types the
    // backend has (float / color / texture).
    bool exposed = false;
    std::string param_name;
};

struct MatLink {
    int id = 0;
    int from_node = 0;
    int from_slot = 0;
    int to_node = 0;
    int to_slot = 0;
};

// Everything type inference works out about a graph, indexed by node *index* into
// MaterialGraph::get_nodes(), not by node id.
struct MatResolved {
    std::vector<std::vector<MatType>> inputs;
    std::vector<std::vector<MatType>> outputs;
    // The common operand type a node promotes its inputs to. Meaningless for nodes
    // that do not promote (masks, appends, constants).
    std::vector<MatType> op;
    // Topological order, as node indices. Every node appears exactly once; a graph
    // that cannot be ordered has a cycle and resolve() fails.
    std::vector<int> order;
};

struct MatCompileResult {
    bool ok = false;
    std::string error;
    // The node the error belongs to, or 0 when it belongs to the graph as a whole.
    int error_node = 0;
    std::string source;   // complete .lshader text
};

class MaterialGraph {
public:
    MaterialGraph();

    // Back to a single Material Output node.
    void reset();

    int add_node(MatNodeType type, float x, float y);
    // The Material Output node cannot be removed; there is nothing to generate from
    // a graph without one.
    bool remove_node(int node_id);
    bool remove_link(int link_id);

    // Adds a link, rejecting anything that would make the graph invalid: a link to
    // itself, a cycle, or a type the destination cannot accept. An input pin holds
    // at most one link, so connecting to an occupied pin replaces what was there.
    //
    // Rejecting is deliberate. A vec3 silently masked down to a float produces a
    // shader that either compiles into the wrong thing or fails with a line number
    // pointing at generated code the author never wrote. The one implicit conversion
    // is float -> vecN, which is written out as an explicit vecN(x) broadcast.
    bool try_add_link(int from_node, int from_slot, int to_node, int to_slot,
                      std::string& out_error);

    MatNode* find_node(int node_id);
    const MatNode* find_node(int node_id) const;
    int node_index(int node_id) const;              // -1 when absent
    const MatLink* link_into(int node_id, int slot) const;
    bool output_is_used(int node_id, int slot) const;

    // Resolves every node's pin types and a topological order. False on a cycle or a
    // type clash, with the offending node in out_error_node (0 = whole graph).
    bool resolve(MatResolved& out, std::string& out_error, int& out_error_node) const;

    // Generates the complete .lshader source. Also refreshes the surface line map.
    MatCompileResult generate();

    // Which node emitted line `line` of the surface block, counting the first
    // generated statement as line 1. 0 when the line belongs to no node.
    int node_for_surface_line(int line) const;

    std::string to_json() const;
    bool from_json(const std::string& text, std::string& out_error);
    bool save(const std::string& path) const;
    bool load(const std::string& path, std::string& out_error);

    // imnodes wants one integer per pin. Deriving it from the node id keeps pin ids
    // stable across a save/load rather than depending on the order pins were created
    // in, and imnodes keeps nodes, pins and links in separate id pools, so a pin id
    // colliding numerically with a node id is harmless.
    static constexpr int kPinStride = 64;
    static constexpr int kOutputPinBase = 32;
    static int input_pin_id(int node_id, int slot) { return node_id * kPinStride + slot; }
    static int output_pin_id(int node_id, int slot) {
        return node_id * kPinStride + kOutputPinBase + slot;
    }
    static bool decode_pin(int pin_id, int& out_node, int& out_slot, bool& out_is_output);

    const std::vector<MatNode>& get_nodes() const { return nodes; }
    std::vector<MatNode>& get_nodes() { return nodes; }
    const std::vector<MatLink>& get_links() const { return links; }
    int get_output_node() const { return output_node_id; }
    int peek_next_id() const { return next_id; }

private:
    std::vector<MatNode> nodes;
    std::vector<MatLink> links;
    int next_id = 1;
    int output_node_id = 0;
    // surface_line_nodes[i] is the node that emitted surface line i + 1.
    std::vector<int> surface_line_nodes;

    bool reaches(int from_node, int target_node) const;
};

