#pragma once

#include "world/component.hpp"
#include "scripting/cminus_interpreter.hpp"
#include <string>
#include <filesystem>

class CMinusComponent : public ActorComponent {
public:
    CMinusComponent(Actor* owner, const std::string& name, const std::string& script_path);

    void tick(float delta_time) override;

    std::string script_path;
    std::filesystem::file_time_type last_modified_time;
    CMinus::Interpreter interpreter;
    std::vector<std::unique_ptr<CMinus::ASTNode>> parsed_program;
    bool has_error;
    
    void load_and_compile();
};
