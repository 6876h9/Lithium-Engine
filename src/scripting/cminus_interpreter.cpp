#include "scripting/cminus_interpreter.hpp"
#include "core/input_map.hpp"
#include <iostream>
#include <cctype>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <SDL2/SDL.h>
#include "physics/physics_engine.hpp"
#include "core/engine.hpp"

namespace CMinus {

// --- Lexer ---

Lexer::Lexer(const std::string& source) : source(source), pos(0) {}

char Lexer::current_char() {
    if (pos >= source.length()) return '\0';
    return source[pos];
}

void Lexer::advance() {
    pos++;
}

void Lexer::skip_whitespace() {
    while (current_char() != '\0' && std::isspace(current_char())) {
        advance();
    }
}

void Lexer::skip_comment() {
    while (current_char() != '\0' && current_char() != '\n') {
        advance();
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (current_char() != '\0') {
        if (std::isspace(current_char())) {
            skip_whitespace();
            continue;
        }

        if (current_char() == '/' && pos + 1 < source.length() && source[pos+1] == '/') {
            skip_comment();
            continue;
        }

        if (std::isalpha(current_char()) || current_char() == '_') {
            std::string text;
            while (std::isalnum(current_char()) || current_char() == '_') {
                text += current_char();
                advance();
            }
            if (text == "while") {
                tokens.push_back({TokenType::WHILE, text});
            } else if (text == "if") {
                tokens.push_back({TokenType::IF, text});
            } else if (text == "else") {
                tokens.push_back({TokenType::ELSE, text});
            } else {
                tokens.push_back({TokenType::IDENTIFIER, text});
            }
            continue;
        }

        if (std::isdigit(current_char()) || current_char() == '.') {
            std::string text;
            while (std::isdigit(current_char()) || current_char() == '.') {
                text += current_char();
                advance();
            }
            tokens.push_back({TokenType::NUMBER, text});
            continue;
        }

        switch (current_char()) {
            case '=': 
                if (pos + 1 < source.length() && source[pos+1] == '=') {
                    tokens.push_back({TokenType::EQUAL_EQUAL, "=="}); advance(); advance();
                } else {
                    tokens.push_back({TokenType::EQUALS, "="}); advance(); 
                }
                break;
            case '!':
                if (pos + 1 < source.length() && source[pos+1] == '=') {
                    tokens.push_back({TokenType::NOT_EQUAL, "!="}); advance(); advance();
                } else {
                    tokens.push_back({TokenType::NOT, "!"}); advance(); 
                }
                break;
            case '<':
                if (pos + 1 < source.length() && source[pos+1] == '=') {
                    tokens.push_back({TokenType::LESS_EQUAL, "<="}); advance(); advance();
                } else {
                    tokens.push_back({TokenType::LESS, "<"}); advance(); 
                }
                break;
            case '>':
                if (pos + 1 < source.length() && source[pos+1] == '=') {
                    tokens.push_back({TokenType::GREATER_EQUAL, ">="}); advance(); advance();
                } else {
                    tokens.push_back({TokenType::GREATER, ">"}); advance(); 
                }
                break;
            case '&':
                if (pos + 1 < source.length() && source[pos+1] == '&') {
                    tokens.push_back({TokenType::AND, "&&"}); advance(); advance();
                } else {
                    tokens.push_back({TokenType::UNKNOWN, "&"}); advance(); 
                }
                break;
            case '|':
                if (pos + 1 < source.length() && source[pos+1] == '|') {
                    tokens.push_back({TokenType::OR, "||"}); advance(); advance();
                } else {
                    tokens.push_back({TokenType::UNKNOWN, "|"}); advance(); 
                }
                break;
            case '%': tokens.push_back({TokenType::MODULO, "%"}); advance(); break;
            case '+': tokens.push_back({TokenType::PLUS, "+"}); advance(); break;
            case '-': tokens.push_back({TokenType::MINUS, "-"}); advance(); break;
            case '*': tokens.push_back({TokenType::MULTIPLY, "*"}); advance(); break;
            case '/': tokens.push_back({TokenType::DIVIDE, "/"}); advance(); break;
            case '(': tokens.push_back({TokenType::LPAREN, "("}); advance(); break;
            case ')': tokens.push_back({TokenType::RPAREN, ")"}); advance(); break;
            case '{': tokens.push_back({TokenType::LBRACE, "{"}); advance(); break;
            case '}': tokens.push_back({TokenType::RBRACE, "}"}); advance(); break;
            case '[': tokens.push_back({TokenType::LBRACKET, "["}); advance(); break;
            case ']': tokens.push_back({TokenType::RBRACKET, "]"}); advance(); break;
            case ';': tokens.push_back({TokenType::SEMICOLON, ";"}); advance(); break;
            case ',': tokens.push_back({TokenType::COMMA, ","}); advance(); break;
            default:
                tokens.push_back({TokenType::UNKNOWN, std::string(1, current_char())});
                advance();
                break;
        }
    }
    tokens.push_back({TokenType::END_OF_FILE, ""});
    return tokens;
}

// --- Parser ---

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens), pos(0) {}

Token Parser::current_token() {
    if (pos >= tokens.size()) return tokens.back();
    return tokens[pos];
}

void Parser::advance() {
    if (pos < tokens.size()) pos++;
}

Token Parser::expect(TokenType type) {
    Token t = current_token();
    if (t.type == type) {
        advance();
        return t;
    }
    throw std::runtime_error("Unexpected token: " + t.text);
}

std::vector<std::unique_ptr<ASTNode>> Parser::parse() {
    std::vector<std::unique_ptr<ASTNode>> statements;
    while (current_token().type != TokenType::END_OF_FILE) {
        statements.push_back(parse_statement());
    }
    return statements;
}

std::unique_ptr<ASTNode> Parser::parse_statement() {
    if (current_token().type == TokenType::WHILE) {
        advance(); // consume 'while'
        expect(TokenType::LPAREN);
        auto cond = parse_expression();
        expect(TokenType::RPAREN);
        auto body = parse_statement();
        return std::make_unique<WhileNode>(std::move(cond), std::move(body));
    }
    
    if (current_token().type == TokenType::IF) {
        advance(); // consume 'if'
        expect(TokenType::LPAREN);
        auto cond = parse_expression();
        expect(TokenType::RPAREN);
        auto then_branch = parse_statement();
        std::unique_ptr<ASTNode> else_branch = nullptr;
        if (current_token().type == TokenType::ELSE) {
            advance();
            else_branch = parse_statement();
        }
        return std::make_unique<IfNode>(std::move(cond), std::move(then_branch), std::move(else_branch));
    }

    if (current_token().type == TokenType::LBRACE) {
        advance(); // consume '{'
        std::vector<std::unique_ptr<ASTNode>> stmts;
        while (current_token().type != TokenType::RBRACE && current_token().type != TokenType::END_OF_FILE) {
            stmts.push_back(parse_statement());
        }
        expect(TokenType::RBRACE);
        return std::make_unique<BlockNode>(std::move(stmts));
    }

    if (current_token().type == TokenType::IDENTIFIER) {
        if (pos + 1 < tokens.size() && tokens[pos+1].type == TokenType::EQUALS) {
            Token id_token = expect(TokenType::IDENTIFIER);
            advance(); // consume '='
            auto expr = parse_expression();
            expect(TokenType::SEMICOLON);
            return std::make_unique<AssignmentNode>(id_token.text, std::move(expr));
        } else if (pos + 1 < tokens.size() && tokens[pos+1].type == TokenType::LBRACKET) {
            Token id_token = expect(TokenType::IDENTIFIER);
            advance(); // consume '['
            auto index_expr = parse_expression();
            expect(TokenType::RBRACKET);
            expect(TokenType::EQUALS);
            auto val_expr = parse_expression();
            expect(TokenType::SEMICOLON);
            return std::make_unique<ArrayAssignmentNode>(id_token.text, std::move(index_expr), std::move(val_expr));
        }
    }

    auto expr = parse_expression();
    expect(TokenType::SEMICOLON);
    return expr;
}

std::unique_ptr<ExpressionNode> Parser::parse_expression() {
    return parse_logical_or();
}

std::unique_ptr<ExpressionNode> Parser::parse_logical_or() {
    auto left = parse_logical_and();
    while (current_token().type == TokenType::OR) {
        Token op = current_token();
        advance();
        auto right = parse_logical_and();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_logical_and() {
    auto left = parse_equality();
    while (current_token().type == TokenType::AND) {
        Token op = current_token();
        advance();
        auto right = parse_equality();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_equality() {
    auto left = parse_relational();
    while (current_token().type == TokenType::EQUAL_EQUAL || current_token().type == TokenType::NOT_EQUAL) {
        Token op = current_token();
        advance();
        auto right = parse_relational();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_relational() {
    auto left = parse_additive();
    while (current_token().type == TokenType::LESS || current_token().type == TokenType::LESS_EQUAL ||
           current_token().type == TokenType::GREATER || current_token().type == TokenType::GREATER_EQUAL) {
        Token op = current_token();
        advance();
        auto right = parse_additive();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_additive() {
    auto left = parse_multiplicative();
    while (current_token().type == TokenType::PLUS || current_token().type == TokenType::MINUS) {
        Token op = current_token();
        advance();
        auto right = parse_multiplicative();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_multiplicative() {
    auto left = parse_unary();
    while (current_token().type == TokenType::MULTIPLY || current_token().type == TokenType::DIVIDE || current_token().type == TokenType::MODULO) {
        Token op = current_token();
        advance();
        auto right = parse_unary();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right));
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_unary() {
    if (current_token().type == TokenType::NOT || current_token().type == TokenType::MINUS) {
        Token op = current_token();
        advance();
        return std::make_unique<UnaryOpNode>(op, parse_unary());
    }
    return parse_factor();
}

std::unique_ptr<ExpressionNode> Parser::parse_factor() {
    Token t = current_token();
    if (t.type == TokenType::NUMBER) {
        advance();
        return std::make_unique<NumberNode>(std::stof(t.text));
    } else if (t.type == TokenType::IDENTIFIER) {
        advance();
        if (current_token().type == TokenType::LPAREN) {
            advance();
            std::vector<std::unique_ptr<ExpressionNode>> args;
            if (current_token().type != TokenType::RPAREN) {
                args.push_back(parse_expression());
                while (current_token().type == TokenType::COMMA) {
                    advance();
                    args.push_back(parse_expression());
                }
            }
            expect(TokenType::RPAREN);
            return std::make_unique<FunctionCallNode>(t.text, std::move(args));
        } else if (current_token().type == TokenType::LBRACKET) {
            advance(); // consume '['
            auto index_expr = parse_expression();
            expect(TokenType::RBRACKET);
            return std::make_unique<ArrayAccessNode>(t.text, std::move(index_expr));
        }
        return std::make_unique<IdentifierNode>(t.text);
    } else if (t.type == TokenType::LPAREN) {
        advance();
        auto expr = parse_expression();
        expect(TokenType::RPAREN);
        return expr;
    }
    throw std::runtime_error("Unexpected token in expression: " + t.text);
}

// --- Interpreter ---

Interpreter::Interpreter(Actor* owner) : actor_owner(owner) {
    variables["x"] = 0.0f;
    variables["y"] = 0.0f;
    variables["z"] = 0.0f;
    variables["time"] = 0.0f;
}

void Interpreter::execute_statement(ASTNode* stmt) {
    if (auto assign = dynamic_cast<AssignmentNode*>(stmt)) {
        variables[assign->identifier] = evaluate(assign->expression.get());
    } else if (auto arr_assign = dynamic_cast<ArrayAssignmentNode*>(stmt)) {
        int idx = static_cast<int>(evaluate(arr_assign->index.get()));
        float val = evaluate(arr_assign->expression.get());
        if (arrays.find(arr_assign->array_name) == arrays.end()) {
            arrays[arr_assign->array_name] = std::vector<float>(100, 0.0f); // Default 100 capacity
        }
        if (idx >= 0 && idx < arrays[arr_assign->array_name].size()) {
            arrays[arr_assign->array_name][idx] = val;
        }
    } else if (auto block = dynamic_cast<BlockNode*>(stmt)) {
        for (const auto& s : block->statements) {
            execute_statement(s.get());
        }
    } else if (auto w = dynamic_cast<WhileNode*>(stmt)) {
        int max_iters = 10000;
        int iters = 0;
        while (evaluate(w->condition.get()) != 0.0f && iters < max_iters) {
            execute_statement(w->body.get());
            iters++;
        }
        if (iters >= max_iters) std::cerr << "CMinus warning: While loop hit 10000 iteration limit!" << std::endl;
    } else if (auto if_node = dynamic_cast<IfNode*>(stmt)) {
        if (evaluate(if_node->condition.get()) != 0.0f) {
            execute_statement(if_node->then_branch.get());
        } else if (if_node->else_branch) {
            execute_statement(if_node->else_branch.get());
        }
    } else if (auto expr = dynamic_cast<ExpressionNode*>(stmt)) {
        evaluate(expr);
    }
}

void Interpreter::execute(const std::vector<std::unique_ptr<ASTNode>>& program) {
    for (const auto& stmt : program) {
        execute_statement(stmt.get());
    }
}

float Interpreter::evaluate(ExpressionNode* node) {
    if (auto num = dynamic_cast<NumberNode*>(node)) {
        return num->value;
    } else if (auto id = dynamic_cast<IdentifierNode*>(node)) {
        return variables[id->name];
    } else if (auto arr_access = dynamic_cast<ArrayAccessNode*>(node)) {
        int idx = static_cast<int>(evaluate(arr_access->index.get()));
        if (arrays.find(arr_access->array_name) != arrays.end()) {
            auto& arr = arrays[arr_access->array_name];
            if (idx >= 0 && idx < arr.size()) {
                return arr[idx];
            }
        }
        return 0.0f;
    } else if (auto bin = dynamic_cast<BinaryOpNode*>(node)) {
        float left = evaluate(bin->left.get());
        float right = evaluate(bin->right.get());
        if (bin->op.type == TokenType::PLUS) return left + right;
        if (bin->op.type == TokenType::MINUS) return left - right;
        if (bin->op.type == TokenType::MULTIPLY) return left * right;
        if (bin->op.type == TokenType::DIVIDE) return left / right;
        if (bin->op.type == TokenType::MODULO) return std::fmod(left, right);
        if (bin->op.type == TokenType::EQUAL_EQUAL) return (left == right) ? 1.0f : 0.0f;
        if (bin->op.type == TokenType::NOT_EQUAL) return (left != right) ? 1.0f : 0.0f;
        if (bin->op.type == TokenType::LESS) return (left < right) ? 1.0f : 0.0f;
        if (bin->op.type == TokenType::LESS_EQUAL) return (left <= right) ? 1.0f : 0.0f;
        if (bin->op.type == TokenType::GREATER) return (left > right) ? 1.0f : 0.0f;
        if (bin->op.type == TokenType::GREATER_EQUAL) return (left >= right) ? 1.0f : 0.0f;
        if (bin->op.type == TokenType::AND) return (left != 0.0f && right != 0.0f) ? 1.0f : 0.0f;
        if (bin->op.type == TokenType::OR) return (left != 0.0f || right != 0.0f) ? 1.0f : 0.0f;
    } else if (auto unary = dynamic_cast<UnaryOpNode*>(node)) {
        float val = evaluate(unary->operand.get());
        if (unary->op.type == TokenType::MINUS) return -val;
        if (unary->op.type == TokenType::NOT) return (val == 0.0f) ? 1.0f : 0.0f;
    } else if (auto func = dynamic_cast<FunctionCallNode*>(node)) {
        auto& name = func->function_name;
        std::vector<float> args;
        for (auto& a : func->arguments) args.push_back(evaluate(a.get()));
        
        if (name == "sin" && args.size() == 1) return std::sin(args[0]);
        if (name == "cos" && args.size() == 1) return std::cos(args[0]);
        if (name == "tan" && args.size() == 1) return std::tan(args[0]);
        if (name == "clamp" && args.size() == 3) return std::clamp(args[0], args[1], args[2]);
        if (name == "spring" && args.size() == 4) return (args[1] - args[0]) * args[2];
        if (name == "abs" && args.size() == 1) return std::abs(args[0]);
        if (name == "sqrt" && args.size() == 1) return std::sqrt(args[0]);
        if (name == "pow" && args.size() == 2) return std::pow(args[0], args[1]);
        if (name == "min" && args.size() == 2) return std::min(args[0], args[1]);
        if (name == "max" && args.size() == 2) return std::max(args[0], args[1]);
        if (name == "clamp" && args.size() == 3) return std::clamp(args[0], args[1], args[2]);
        if (name == "lerp" && args.size() == 3) return args[0] + (args[1] - args[0]) * args[2];
        if (name == "smoothstep" && args.size() == 3) {
            float t = std::clamp((args[2] - args[0]) / (args[1] - args[0]), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
        if (name == "distance" && args.size() == 6) {
            float dx = args[0] - args[3], dy = args[1] - args[4], dz = args[2] - args[5];
            return std::sqrt(dx*dx + dy*dy + dz*dz);
        }
        if (name == "dot" && args.size() == 6) return args[0]*args[3] + args[1]*args[4] + args[2]*args[5];
        if (name == "noise3d" && args.size() == 3) {
            float v = std::sin(args[0]*12.9898f + args[1]*78.233f + args[2]*37.719f) * 43758.5453f;
            return v - std::floor(v);
        }
        if (name == "rand" && args.size() == 0) return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        if (name == "get_time" && args.size() == 0) return variables["time"];
        if (name == "get_dt" && args.size() == 0) return variables["dt"];
        if (name == "is_key_pressed" && args.size() == 1) {
            const Uint8* state = SDL_GetKeyboardState(NULL);
            int scancode = static_cast<int>(args[0]);
            if (scancode >= 0 && scancode < SDL_NUM_SCANCODES) {
                return state[scancode] ? 1.0f : 0.0f;
            }
            return 0.0f;
        }

        if (actor_owner) {
            if (name == "get_x" && args.size() == 0) return actor_owner->get_actor_transform().position.x;
            if (name == "get_y" && args.size() == 0) return actor_owner->get_actor_transform().position.y;
            if (name == "get_z" && args.size() == 0) return actor_owner->get_actor_transform().position.z;
            if (name == "get_rot_x" && args.size() == 0) return actor_owner->get_actor_transform().rotation.x;
            if (name == "get_rot_y" && args.size() == 0) return actor_owner->get_actor_transform().rotation.y;
            if (name == "get_rot_z" && args.size() == 0) return actor_owner->get_actor_transform().rotation.z;
            
            if (name == "orbit" && args.size() == 5) {
                float t = variables["time"] * args[4];
                actor_owner->get_actor_transform().position = DVector3{args[0] + std::cos(t)*args[3], static_cast<double>(args[1]), args[2] + std::sin(t)*args[3]};
            } else if (name == "oscillate" && args.size() == 2) {
                float t = variables["time"] * args[0];
                actor_owner->get_actor_transform().position.y += std::sin(t) * args[1];
            } else if (name == "spring" && args.size() == 5) {
                return -args[3] * (args[0] - args[1]) - args[4] * args[2];
            } else if (name == "look_at" && args.size() == 3) {
                DVector3 pos = actor_owner->get_actor_transform().position;
                double dx = args[0] - pos.x, dy = args[1] - pos.y, dz = args[2] - pos.z;
                actor_owner->get_actor_transform().rotation.y = std::atan2(dx, dz);
                actor_owner->get_actor_transform().rotation.x = std::atan2(dy, std::sqrt(dx*dx + dz*dz));
            } else if (name == "apply_force" && args.size() == 3) {
                // Accelerates the actor rather than teleporting it. This previously
                // added straight to position, which is a displacement, not a force -
                // it ignored frame time entirely, so its effect changed with frame rate.
                actor_owner->velocity.x += args[0];
                actor_owner->velocity.y += args[1];
                actor_owner->velocity.z += args[2];
            } else if (name == "set_velocity" && args.size() == 3) {
                actor_owner->velocity = Vector3(args[0], args[1], args[2]);
            } else if (name == "add_velocity" && args.size() == 3) {
                actor_owner->velocity.x += args[0];
                actor_owner->velocity.y += args[1];
                actor_owner->velocity.z += args[2];
            } else if (name == "get_vel_x" && args.size() == 0) {
                return actor_owner->velocity.x;
            } else if (name == "get_vel_y" && args.size() == 0) {
                return actor_owner->velocity.y;
            } else if (name == "get_vel_z" && args.size() == 0) {
                return actor_owner->velocity.z;
            } else if (name == "set_angular_velocity" && args.size() == 3) {
                actor_owner->angular_velocity = Vector3(args[0], args[1], args[2]);
            } else if (name == "stop_motion" && args.size() == 0) {
                actor_owner->velocity = Vector3(0.0f, 0.0f, 0.0f);
                actor_owner->angular_velocity = Vector3(0.0f, 0.0f, 0.0f);
            } else if (name == "set_emissive" && args.size() == 1) {
                actor_owner->emissive = args[0];
            } else if (name == "translate" && args.size() == 3) {
                // Explicit displacement, for when you really do want to move now.
                actor_owner->get_actor_transform().position.x += args[0];
                actor_owner->get_actor_transform().position.y += args[1];
                actor_owner->get_actor_transform().position.z += args[2];
            } else if (name == "set_position" && args.size() == 3) {
                actor_owner->get_actor_transform().position = DVector3{static_cast<double>(args[0]), static_cast<double>(args[1]), static_cast<double>(args[2])};
            } else if (name == "set_rotation" && args.size() == 3) {
                actor_owner->get_actor_transform().rotation = Vector3(args[0], args[1], args[2]);
            } else if (name == "set_scale" && args.size() == 3) {
                actor_owner->get_actor_transform().scale = Vector3(args[0], args[1], args[2]);
            } else if (name == "set_color" && args.size() == 3) {
                actor_owner->actor_color = Vector3(args[0], args[1], args[2]);
            } else if (name == "set_emission" && args.size() == 1) {
                if (actor_owner->assigned_material) actor_owner->assigned_material->emission = args[0];
            } else if (name == "set_metallic" && args.size() == 1) {
                if (actor_owner->assigned_material) actor_owner->assigned_material->metallic = args[0];
            } else if (name == "set_roughness" && args.size() == 1) {
                if (actor_owner->assigned_material) actor_owner->assigned_material->roughness = args[0];
            } else if (name == "set_wireframe" && args.size() == 1) {
                if (g_engine) g_engine->get_renderer().wireframe_mode = args[0] > 0.5f;
            } else if (name == "set_msaa" && args.size() == 1) {
                if (g_engine) g_engine->get_renderer().enable_msaa = args[0] > 0.5f;
            } else if (name == "set_lighting" && args.size() == 1) {
                if (g_engine) g_engine->get_renderer().enable_ue4_lighting = args[0] > 0.5f;
            } else if (name == "set_time_of_day" && args.size() == 1) {
                if (g_engine) g_engine->get_renderer().sky_time_override = args[0];
            } else if (name == "set_cam_pos" && args.size() == 3) {
                if (g_engine) g_engine->camera_pos = DVector3{static_cast<double>(args[0]), static_cast<double>(args[1]), static_cast<double>(args[2])};
            } else if (name == "set_cam_rot" && args.size() == 2) {
                if (g_engine) g_engine->camera_rot = Vector3(args[0], args[1], 0.0f);
            } else if (name == "spawn_actor" && args.size() == 1) {
                if (g_engine) g_engine->spawn_actor_by_id(static_cast<int>(args[0]));
            } else if (name == "destroy_self" && args.size() == 0) {
                if (g_engine) g_engine->queue_destroy_actor(actor_owner);
            } else if (name == "get_nearby_actors" && args.size() == 1) {
                nearby_cache.clear();
                if (g_engine) {
                    float radius_sq = args[0] * args[0];
                    DVector3 my_pos = actor_owner->get_actor_transform().position;
                    for (auto& other_actor : g_engine->get_actors()) {
                        if (other_actor.get() == actor_owner) continue;
                        DVector3 other_pos = other_actor->get_actor_transform().position;
                        float dist_sq = std::pow(my_pos.x - other_pos.x, 2) + std::pow(my_pos.y - other_pos.y, 2) + std::pow(my_pos.z - other_pos.z, 2);
                        if (dist_sq <= radius_sq) {
                            nearby_cache.push_back(other_actor.get());
                        }
                    }
                }
                return static_cast<float>(nearby_cache.size());
            } else if (name == "find_actors" && args.size() == 4) {
                // Same cache as get_nearby_actors, but centred on an arbitrary point
                // rather than on the script's own actor. A script that drives the
                // camera has a player position that belongs to no actor at all, so
                // an owner-relative query cannot reach what the player is standing
                // next to.
                nearby_cache.clear();
                if (g_engine) {
                    float radius_sq = args[3] * args[3];
                    for (auto& other_actor : g_engine->get_actors()) {
                        if (other_actor.get() == actor_owner) continue;
                        DVector3 p = other_actor->get_actor_transform().position;
                        float dx = static_cast<float>(p.x) - args[0];
                        float dy = static_cast<float>(p.y) - args[1];
                        float dz = static_cast<float>(p.z) - args[2];
                        if (dx * dx + dy * dy + dz * dz <= radius_sq) {
                            nearby_cache.push_back(other_actor.get());
                        }
                    }
                }
                return static_cast<float>(nearby_cache.size());
            } else if (name == "set_nearby_color" && args.size() == 4) {
                // The only way for a script to change an actor other than its own.
                // Without it a script can see the whole scene and alter none of it.
                int idx = static_cast<int>(args[0]);
                if (idx >= 0 && idx < static_cast<int>(nearby_cache.size())) {
                    nearby_cache[idx]->actor_color = Vector3(args[1], args[2], args[3]);
                }
            } else if (name == "set_nearby_emissive" && args.size() == 2) {
                int idx = static_cast<int>(args[0]);
                if (idx >= 0 && idx < static_cast<int>(nearby_cache.size())) {
                    nearby_cache[idx]->emissive = args[1];
                }
            } else if (name == "get_nearby_x" && args.size() == 1) {
                int idx = static_cast<int>(args[0]);
                if (idx >= 0 && idx < nearby_cache.size()) return nearby_cache[idx]->get_actor_transform().position.x;
                return 0.0f;
            } else if (name == "get_nearby_y" && args.size() == 1) {
                int idx = static_cast<int>(args[0]);
                if (idx >= 0 && idx < nearby_cache.size()) return nearby_cache[idx]->get_actor_transform().position.y;
                return 0.0f;
            } else if (name == "get_nearby_z" && args.size() == 1) {
                int idx = static_cast<int>(args[0]);
                if (idx >= 0 && idx < nearby_cache.size()) return nearby_cache[idx]->get_actor_transform().position.z;
                return 0.0f;
            }
        }
        
        // Input actions. C-minus call arguments are floats, so actions are addressed
        // by index rather than by name; the editor's Input Bindings window lists the
        // index beside each action for exactly this reason.
        if (name == "action_held" && args.size() == 1) {
            return InputMap::get().held(static_cast<int>(args[0])) ? 1.0f : 0.0f;
        }
        if (name == "action_pressed" && args.size() == 1) {
            return InputMap::get().pressed(static_cast<int>(args[0])) ? 1.0f : 0.0f;
        }
        if (name == "action_released" && args.size() == 1) {
            return InputMap::get().released(static_cast<int>(args[0])) ? 1.0f : 0.0f;
        }
        if (name == "action_axis" && args.size() == 1) {
            return InputMap::get().axis(static_cast<int>(args[0]));
        }
        if (name == "lock_mouse" && args.size() == 1) {
            SDL_SetRelativeMouseMode(args[0] > 0.5f ? SDL_TRUE : SDL_FALSE);
            return 0.0f;
        }
        if (name == "get_mouse_dx" && args.size() == 0) {
            return InputMap::get().mouse_dx();
        }
        if (name == "get_mouse_dy" && args.size() == 0) {
            return InputMap::get().mouse_dy();
        }

        // ---- HUD ----
        if (name == "hud_pip" && args.size() == 2) {
            if (g_engine) {
                int idx = static_cast<int>(args[0]);
                if (idx >= 0 && idx < 8) {
                    g_engine->script_hud.pip[idx] = args[1];
                    if (idx + 1 > g_engine->script_hud.pip_count) {
                        g_engine->script_hud.pip_count = idx + 1;
                    }
                }
            }
            return 0.0f;
        }
        if (name == "hud_vignette" && args.size() == 4) {
            if (g_engine) {
                g_engine->script_hud.vignette = std::clamp(args[0], 0.0f, 1.0f);
                g_engine->script_hud.vignette_color = Vector3(args[1], args[2], args[3]);
            }
            return 0.0f;
        }

        if (name == "print" && args.size() == 1) std::cout << "[CMinus print] " << args[0] << std::endl;
        if (name == "raycast" && args.size() == 7) {
            float dist = 0.0f;
            if (PhysicsEngine::get_instance().raycast(args[0], args[1], args[2], args[3], args[4], args[5], args[6], dist)) {
                return dist;
            }
            return -1.0f;
        }
        return 0.0f;
    }
    return 0.0f;
}

} // namespace CMinus
