#pragma once
#include <string>
#include <vector>
#include "imnodes.h"
#include "world/actor.hpp"

class VisualScriptEditor {
public:
    VisualScriptEditor();
    ~VisualScriptEditor();

    void initialize();
    void render(std::vector<Actor*>& selected_actors);
    std::string generate_cminus_code();

private:
    struct Node {
        int id;
        std::string name;
        std::string code_snippet;
        int input_pin_id;
        std::vector<int> output_pin_ids;
        std::vector<std::string> output_pin_names;
        std::vector<float> params;
        std::vector<std::string> param_names;
        std::vector<std::string> string_params;
        std::vector<std::string> string_param_names;
    };
    
    struct Link {
        int id;
        int start_pin;
        int end_pin;
    };
    
    std::vector<Node> nodes;
    std::vector<Link> links;
    int current_id;
};
