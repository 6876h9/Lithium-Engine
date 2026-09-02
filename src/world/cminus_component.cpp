#include "world/cminus_component.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

CMinusComponent::CMinusComponent(Actor* owner, const std::string& name, const std::string& script_path) 
    : ActorComponent(owner, name), script_path(script_path), has_error(false) {
    load_and_compile();
}

void CMinusComponent::load_and_compile() {
    std::ifstream file(script_path);
    if (!file.is_open()) {
        std::cerr << "Failed to open script: " << script_path << std::endl;
        has_error = true;
        return;
    }
    
    try {
        last_modified_time = std::filesystem::last_write_time(script_path);
    } catch (...) {
        // If file doesn't exist or permission denied
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    try {
        CMinus::Lexer lexer(source);
        std::vector<CMinus::Token> tokens = lexer.tokenize();

        CMinus::Parser parser(tokens);
        parsed_program = parser.parse();
        has_error = false;
        std::cout << "[CMinus] Hot-reloaded " << script_path << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "CMinus Error in " << script_path << ": " << e.what() << std::endl;
        has_error = true;
    }
}

void CMinusComponent::tick(float delta_time) {
    try {
        if (std::filesystem::exists(script_path)) {
            auto current_time = std::filesystem::last_write_time(script_path);
            if (current_time > last_modified_time) {
                load_and_compile();
            }
        }
    } catch (...) {}

    if (has_error) return;

    interpreter.actor_owner = owner;
    // Values are tagged now, not bare floats, so the accumulator has to be read back
    // through the tag rather than added to directly. A "time" left over as some other
    // type (a script assigned it a string, say) restarts from zero instead of
    // corrupting the accumulation.
    CMinus::Value& elapsed = interpreter.variables["time"];
    const float previous = elapsed.is_number() ? elapsed.x : 0.0f;
    elapsed = CMinus::Value::number(previous + delta_time);
    interpreter.variables["dt"] = CMinus::Value::number(delta_time);

    try {
        interpreter.execute(parsed_program);
    } catch (const std::exception& e) {
        std::cerr << "CMinus Runtime Error in " << script_path << ": " << e.what() << std::endl;
        has_error = true;
    }
}
