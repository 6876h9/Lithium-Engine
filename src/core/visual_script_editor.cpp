#include "core/visual_script_editor.hpp"
#include "world/cminus_component.hpp"
#include <imgui.h>
#include <fstream>
#include <iostream>

VisualScriptEditor::VisualScriptEditor() : current_id(1) {
}

VisualScriptEditor::~VisualScriptEditor() {
    ImNodes::DestroyContext();
}

void VisualScriptEditor::initialize() {
    ImNodes::CreateContext();
    ImNodes::StyleColorsDark();
}

void VisualScriptEditor::render(std::vector<Actor*>& selected_actors) {
    ImGui::Begin("Visual Script Editor");

    ImGui::Text("Right-click to add nodes.");
    
    ImNodes::BeginNodeEditor();
    
    auto add_node = [&](const std::string& name, const std::string& code, const std::vector<std::string>& pnames, const std::vector<float>& defs, const std::vector<std::string>& out_names = {"Out"}) {
        int id = current_id++;
        int in_pin = (name.find("Event:") != std::string::npos) ? -1 : current_id++;
        std::vector<int> out_pins;
        for (size_t i = 0; i < out_names.size(); ++i) out_pins.push_back(current_id++);
        nodes.push_back({id, name, code, in_pin, out_pins, out_names, defs, pnames, {}, {}});
    };

    auto add_str_node = [&](const std::string& name, const std::string& code, const std::vector<std::string>& spnames, const std::vector<std::string>& sdefs, const std::vector<std::string>& out_names = {"Out"}) {
        int id = current_id++;
        int in_pin = current_id++;
        std::vector<int> out_pins;
        for (size_t i = 0; i < out_names.size(); ++i) out_pins.push_back(current_id++);
        nodes.push_back({id, name, code, in_pin, out_pins, out_names, {}, {}, sdefs, spnames});
    };

    // Handle right-click context menu
    if (ImGui::BeginPopupContextWindow("Node Context Menu", ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Add Event: OnBeginPlay")) add_node("Event: OnBeginPlay", "", {}, {});
        if (ImGui::MenuItem("Add Event: OnUpdate")) add_node("Event: OnUpdate", "", {}, {});
        
        if (ImGui::MenuItem("Add If / Else Node")) add_node("If / Else", "if (%f) {\n", {"Condition (0=F, 1=T)"}, {1.0}, {"True", "False"});
        if (ImGui::MenuItem("Add While Node")) add_node("While Loop", "while (%f) {\n", {"Condition"}, {1.0}, {"Loop Body", "Completed"});
        if (ImGui::MenuItem("Add If Key Pressed")) add_node("If Key Pressed", "if (is_key_pressed(%f)) {\n", {"Scancode"}, {26.0}, {"True", "False"});
        
        if (ImGui::MenuItem("Add Math Expression Node")) add_str_node("Math Expression", "%s;\n", {"Expression"}, {"x = x + 1"});
        if (ImGui::MenuItem("Add Set Variable Node")) add_str_node("Set Variable", "%s = %s;\n", {"Var Name", "Value"}, {"my_var", "0.0"});
        
        if (ImGui::MenuItem("Add Orbit Node")) add_node("Orbit", "orbit(%f, %f, %f, %f, %f);\n", {"Center X", "Center Y", "Center Z", "Radius", "Speed"}, {0, 0, 0, 5.0, 1.0});
        if (ImGui::MenuItem("Add Sine Bobbing Node")) add_node("Sine Bobbing", "oscillate(%f, %f);\n", {"Frequency", "Amplitude"}, {1.0, 2.0});
        if (ImGui::MenuItem("Add Look At Node")) add_node("Look At", "look_at(%f, %f, %f);\n", {"Target X", "Target Y", "Target Z"}, {0, 0, 0});
        if (ImGui::MenuItem("Add Random Wander Node")) add_node("Random Wander", "apply_force((rand()-0.5)*%f, 0, (rand()-0.5)*%f);\n", {"Speed X", "Speed Z"}, {1.0, 1.0});
        if (ImGui::MenuItem("Add Apply Force Node")) add_node("Apply Force", "apply_force(%f, %f, %f);\n", {"FX", "FY", "FZ"}, {0, 10.0, 0});
        if (ImGui::MenuItem("Add Rainbow Color Cycle")) add_node("Rainbow Color Cycle", "set_color(abs(sin(time*%f)), abs(cos(time*%f)), abs(sin(time*%f)));\n", {"Speed1", "Speed2", "Speed3"}, {1.0, 1.2, 0.8});
        if (ImGui::MenuItem("Add Set Position Node")) add_node("Set Position", "set_position(%f, %f, %f);\n", {"X", "Y", "Z"}, {0, 0, 0});
        if (ImGui::MenuItem("Add Set Rotation Node")) add_node("Set Rotation", "set_rotation(%f, %f, %f);\n", {"X", "Y", "Z"}, {0, 0, 0});
        if (ImGui::MenuItem("Add Set Scale Node")) add_node("Set Scale", "set_scale(%f, %f, %f);\n", {"X", "Y", "Z"}, {1, 1, 1});

        ImGui::Separator();
        if (ImGui::MenuItem("Add Clamp Node")) add_str_node("Clamp", "%s = clamp(%s, %s, %s);\n", {"VarName", "Val", "Min", "Max"}, {"x", "x", "0", "1"});
        if (ImGui::MenuItem("Add Spring Node")) add_str_node("Spring Node", "%s = spring(%s, %s, %s, %s);\n", {"VarName", "Cur", "Target", "Stiff", "Damp"}, {"x", "x", "10", "5.0", "0.5"});
        if (ImGui::MenuItem("Add Raycast Node")) add_str_node("Raycast", "%s = raycast(%s, %s, %s, %s, %s, %s, %s);\n", {"VarName", "X", "Y", "Z", "DX", "DY", "DZ", "MaxDist"}, {"hit", "0", "0", "0", "0", "-1", "0", "100"});
        if (ImGui::MenuItem("Add Get Nearby Actors")) add_str_node("Get Nearby Actors", "%s = get_nearby_actors(%s);\n", {"VarName (Count)", "Radius"}, {"count", "10"});
        if (ImGui::MenuItem("Add Array Access")) add_str_node("Array Access", "%s = %s[%s];\n", {"DestVar", "ArrayName", "Index"}, {"val", "arr", "0"});
        if (ImGui::MenuItem("Add Array Assign")) add_str_node("Array Assign", "%s[%s] = %s;\n", {"ArrayName", "Index", "Value"}, {"arr", "0", "1.0"});
        ImGui::Separator();
        if (ImGui::MenuItem("Add Set Wireframe")) add_node("Set Wireframe", "set_wireframe(%f);\n", {"Enabled"}, {1});
        if (ImGui::MenuItem("Add Set MSAA")) add_node("Set MSAA", "set_msaa(%f);\n", {"Enabled"}, {1});
        if (ImGui::MenuItem("Add Set Lighting")) add_node("Set Lighting", "set_lighting(%f);\n", {"Enabled"}, {1});
        if (ImGui::MenuItem("Add Set Time of Day")) add_node("Set Time of Day", "set_time_of_day(%f);\n", {"Time"}, {0.0f});
        if (ImGui::MenuItem("Add Set Camera Pos")) add_node("Set Camera Pos", "set_cam_pos(%f, %f, %f);\n", {"X", "Y", "Z"}, {0, 0, 5.0f});
        if (ImGui::MenuItem("Add Set Camera Rot")) add_node("Set Camera Rot", "set_cam_rot(%f, %f);\n", {"Pitch", "Yaw"}, {0, 0});
        if (ImGui::MenuItem("Add Spawn Actor")) add_node("Spawn Actor", "spawn_actor(%f);\n", {"Type (0=Cube, 1=Sphere, 2=Light, 3=Particles)"}, {0});
        if (ImGui::MenuItem("Add Destroy Self")) add_node("Destroy Self", "destroy_self();\n", {}, {});
        if (ImGui::MenuItem("Add Set Emission")) add_node("Set Emission", "set_emission(%f);\n", {"Val"}, {1.0f});
        if (ImGui::MenuItem("Add Set Metallic")) add_node("Set Metallic", "set_metallic(%f);\n", {"Val"}, {0.0f});
        if (ImGui::MenuItem("Add Set Roughness")) add_node("Set Roughness", "set_roughness(%f);\n", {"Val"}, {0.5f});

        ImGui::EndPopup();
    }
    
    // Draw nodes
    for (auto& node : nodes) {
        ImNodes::BeginNode(node.id);
        
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(node.name.c_str());
        ImNodes::EndNodeTitleBar();
        
        if (node.input_pin_id != -1) {
            ImNodes::BeginInputAttribute(node.input_pin_id);
            ImGui::Text("In");
            ImNodes::EndInputAttribute();
        }
        
        for (size_t i = 0; i < node.params.size(); ++i) {
            ImGui::PushItemWidth(100.0f);
            std::string label = node.param_names[i] + "##" + std::to_string(node.id) + "_" + std::to_string(i);
            ImGui::DragFloat(label.c_str(), &node.params[i], 0.1f);
            ImGui::PopItemWidth();
        }
        
        for (size_t i = 0; i < node.string_params.size(); ++i) {
            ImGui::PushItemWidth(150.0f);
            std::string label = node.string_param_names[i] + "##str" + std::to_string(node.id) + "_" + std::to_string(i);
            
            char buf[128];
            strncpy(buf, node.string_params[i].c_str(), sizeof(buf));
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                node.string_params[i] = buf;
            }
            ImGui::PopItemWidth();
        }
        
        for (size_t i = 0; i < node.output_pin_ids.size(); ++i) {
            ImNodes::BeginOutputAttribute(node.output_pin_ids[i]);
            ImGui::Text("%s", node.output_pin_names[i].c_str());
            ImNodes::EndOutputAttribute();
        }
        
        ImNodes::EndNode();
    }
    
    // Draw links
    for (auto& link : links) {
        ImNodes::Link(link.id, link.start_pin, link.end_pin);
    }
    
    ImNodes::EndNodeEditor();
    
    // Link creation logic
    int start_node, end_node;
    int start_pin, end_pin;
    if (ImNodes::IsLinkCreated(&start_node, &start_pin, &end_node, &end_pin)) {
        links.push_back({current_id++, start_pin, end_pin});
    }

    ImGui::Separator();
    if (ImGui::Button("Compile & Apply to Selected Actor")) {
        if (!selected_actors.empty()) {
            Actor* actor = selected_actors[0];
            std::string code = generate_cminus_code();
            std::string path = "Content/visual_script_" + actor->get_name() + ".cminus";
            std::ofstream file(path);
            if (file.is_open()) {
                file << code;
                file.close();
                // Attach component
                bool has_script = false;
                for (auto& comp : actor->get_components()) {
                    if (dynamic_cast<CMinusComponent*>(comp.get())) {
                        has_script = true;
                        break;
                    }
                }
                if (!has_script) {
                    actor->create_component<CMinusComponent>("VisualScript", path);
                } else {
                    std::cout << "Actor already has a script component! Reloading..." << std::endl;
                    // For simplicity, just let the user know, we would need to reload it.
                }
            }
        } else {
            std::cout << "No actor selected!" << std::endl;
        }
    }

    ImGui::End();
}

std::string VisualScriptEditor::generate_cminus_code() {
    std::string code = "if (_initialized == 0) {\n    _initialized = 1;\n";
    
    auto generate_node_tree = [&](auto& self, int node_id, int indent_level) -> std::string {
        if (node_id == -1) return "";
        std::string result = "";
        std::string indent(indent_level * 4, ' ');
        
        const Node* curr = nullptr;
        for (const auto& node : nodes) {
            if (node.id == node_id) {
                curr = &node;
                break;
            }
        }
        if (!curr) return "";

        char buf[256];
        if (!curr->string_params.empty()) {
            if (curr->string_params.size() == 1) snprintf(buf, sizeof(buf), curr->code_snippet.c_str(), curr->string_params[0].c_str());
            else if (curr->string_params.size() == 2) snprintf(buf, sizeof(buf), curr->code_snippet.c_str(), curr->string_params[0].c_str(), curr->string_params[1].c_str());
            else snprintf(buf, sizeof(buf), "%s", curr->code_snippet.c_str());
        } else {
            if (curr->params.size() == 1) snprintf(buf, sizeof(buf), curr->code_snippet.c_str(), curr->params[0]);
            else if (curr->params.size() == 2) snprintf(buf, sizeof(buf), curr->code_snippet.c_str(), curr->params[0], curr->params[1]);
            else if (curr->params.size() == 3) snprintf(buf, sizeof(buf), curr->code_snippet.c_str(), curr->params[0], curr->params[1], curr->params[2]);
            else if (curr->params.size() == 4) snprintf(buf, sizeof(buf), curr->code_snippet.c_str(), curr->params[0], curr->params[1], curr->params[2], curr->params[3]);
            else if (curr->params.size() == 5) snprintf(buf, sizeof(buf), curr->code_snippet.c_str(), curr->params[0], curr->params[1], curr->params[2], curr->params[3], curr->params[4]);
            else snprintf(buf, sizeof(buf), "%s", curr->code_snippet.c_str());
        }
        
        if (curr->name.find("Event:") == std::string::npos) {
            result += indent + buf;
        }

        if (curr->name == "If / Else" || curr->name == "If Key Pressed") {
            int true_next = -1;
            for (const auto& link : links) {
                if (link.start_pin == curr->output_pin_ids[0]) {
                    for (const auto& n : nodes) if (n.input_pin_id == link.end_pin) true_next = n.id;
                }
            }
            result += self(self, true_next, indent_level + 1);
            
            result += indent + "} else {\n";
            
            int false_next = -1;
            for (const auto& link : links) {
                if (link.start_pin == curr->output_pin_ids[1]) {
                    for (const auto& n : nodes) if (n.input_pin_id == link.end_pin) false_next = n.id;
                }
            }
            result += self(self, false_next, indent_level + 1);
            result += indent + "}\n";
        } else if (curr->name == "While Loop") {
            int body_next = -1;
            for (const auto& link : links) {
                if (link.start_pin == curr->output_pin_ids[0]) {
                    for (const auto& n : nodes) if (n.input_pin_id == link.end_pin) body_next = n.id;
                }
            }
            result += self(self, body_next, indent_level + 1);
            result += indent + "}\n";
            
            int comp_next = -1;
            for (const auto& link : links) {
                if (link.start_pin == curr->output_pin_ids[1]) {
                    for (const auto& n : nodes) if (n.input_pin_id == link.end_pin) comp_next = n.id;
                }
            }
            result += self(self, comp_next, indent_level);
        } else {
            if (curr->output_pin_ids.size() > 0) {
                int next_node_id = -1;
                for (const auto& link : links) {
                    if (link.start_pin == curr->output_pin_ids[0]) {
                        for (const auto& n : nodes) if (n.input_pin_id == link.end_pin) next_node_id = n.id;
                    }
                }
                result += self(self, next_node_id, indent_level);
            }
        }
        
        return result;
    };
    
    int begin_play_id = -1;
    for (const auto& node : nodes) {
        if (node.name == "Event: OnBeginPlay") begin_play_id = node.id;
    }
    
    if (begin_play_id != -1) {
        int next_node_id = -1;
        for (const auto& link : links) {
            if (nodes[begin_play_id].output_pin_ids.size() > 0 && link.start_pin == nodes[begin_play_id].output_pin_ids[0]) {
                for (const auto& n : nodes) if (n.input_pin_id == link.end_pin) next_node_id = n.id;
            }
        }
        code += generate_node_tree(generate_node_tree, next_node_id, 1);
    }
    
    code += "}\nwhile (1) {\n";
    
    int update_id = -1;
    for (const auto& node : nodes) {
        if (node.name == "Event: OnUpdate") update_id = node.id;
    }
    
    if (update_id != -1) {
        int next_node_id = -1;
        for (const auto& link : links) {
            if (nodes[update_id].output_pin_ids.size() > 0 && link.start_pin == nodes[update_id].output_pin_ids[0]) {
                for (const auto& n : nodes) if (n.input_pin_id == link.end_pin) next_node_id = n.id;
            }
        }
        code += generate_node_tree(generate_node_tree, next_node_id, 1);
    }
    
    code += "}\n";
    return code;
}
