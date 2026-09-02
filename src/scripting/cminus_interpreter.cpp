#include "scripting/cminus_interpreter.hpp"
#include "core/input_map.hpp"
#include <iostream>
#include <sstream>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <SDL2/SDL.h>
#include "physics/physics_engine.hpp"
#include "core/engine.hpp"

namespace CMinus {

// =============================================================================
//  Errors
// =============================================================================

std::string ScriptError::format(const std::string& message, int line, const std::string& script) {
    std::string out;
    if (!script.empty()) out += script;
    else out += "<script>";
    if (line > 0) {
        out += ":";
        out += std::to_string(line);
    }
    out += ": ";
    out += message;
    return out;
}

// =============================================================================
//  Value
// =============================================================================

const std::string& Value::as_string() const {
    static const std::string empty;
    return text ? *text : empty;
}

bool Value::truthy() const {
    switch (type) {
        case ValueType::Number: return x != 0.0f;
        case ValueType::Vec3:   return x != 0.0f || y != 0.0f || z != 0.0f;
        case ValueType::String: return text && !text->empty();
        case ValueType::Void:   return false;
    }
    return false;
}

const char* Value::type_name(ValueType t) {
    switch (t) {
        case ValueType::Number: return "number";
        case ValueType::Vec3:   return "vec3";
        case ValueType::String: return "string";
        case ValueType::Void:   return "void";
    }
    return "?";
}

// Trailing zeros are trimmed so a whole number prints as "3" rather than
// "3.000000" - the watch panel is unreadable otherwise.
static std::string format_number(float v) {
    if (!std::isfinite(v)) return std::isnan(v) ? std::string("nan") : (v > 0 ? std::string("inf") : std::string("-inf"));
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(v));
    return buffer;
}

std::string Value::to_display_string() const {
    switch (type) {
        case ValueType::Number: return format_number(x);
        case ValueType::Vec3:   return "(" + format_number(x) + ", " + format_number(y) + ", " + format_number(z) + ")";
        case ValueType::String: return as_string();
        case ValueType::Void:   return "void";
    }
    return "?";
}

// =============================================================================
//  Lexer
// =============================================================================

Lexer::Lexer(const std::string& source, std::string script_name)
    : source(source), script_name(std::move(script_name)), pos(0), line(1), column(1) {}

char Lexer::current_char() const {
    if (pos >= source.length()) return '\0';
    return source[pos];
}

char Lexer::peek(size_t offset) const {
    if (pos + offset >= source.length()) return '\0';
    return source[pos + offset];
}

void Lexer::advance() {
    if (pos < source.length()) {
        if (source[pos] == '\n') { line++; column = 1; }
        else { column++; }
        pos++;
    }
}

void Lexer::skip_whitespace() {
    while (current_char() != '\0' && std::isspace(static_cast<unsigned char>(current_char()))) {
        advance();
    }
}

void Lexer::skip_line_comment() {
    while (current_char() != '\0' && current_char() != '\n') advance();
}

void Lexer::skip_block_comment() {
    const int start_line = line;
    advance(); advance(); // consume "/*"
    while (true) {
        if (current_char() == '\0') {
            throw ScriptError("unterminated /* comment", start_line, script_name);
        }
        if (current_char() == '*' && peek() == '/') { advance(); advance(); return; }
        advance();
    }
}

Token Lexer::lex_number() {
    const int start_line = line;
    const int start_col = column;
    std::string text;
    bool seen_dot = false;
    while (true) {
        const char c = current_char();
        if (std::isdigit(static_cast<unsigned char>(c))) { text += c; advance(); continue; }
        if (c == '.') {
            // "1.2.3" used to reach std::stof, which parses the prefix and discards
            // the rest without complaint. A malformed literal should not silently
            // become a different number.
            if (seen_dot) throw ScriptError("malformed number literal '" + text + "."  + "'", start_line, script_name);
            seen_dot = true;
            text += c;
            advance();
            continue;
        }
        if ((c == 'e' || c == 'E') &&
            (std::isdigit(static_cast<unsigned char>(peek())) ||
             ((peek() == '+' || peek() == '-') && std::isdigit(static_cast<unsigned char>(peek(2)))))) {
            text += c; advance();
            if (current_char() == '+' || current_char() == '-') { text += current_char(); advance(); }
            while (std::isdigit(static_cast<unsigned char>(current_char()))) { text += current_char(); advance(); }
            break;
        }
        break;
    }
    if (text.empty() || text == ".") {
        throw ScriptError("malformed number literal", start_line, script_name);
    }
    return Token{ TokenType::NUMBER, text, start_line, start_col };
}

Token Lexer::lex_string() {
    const int start_line = line;
    const int start_col = column;
    advance(); // consume opening quote
    std::string text;
    while (true) {
        const char c = current_char();
        if (c == '\0' || c == '\n') {
            throw ScriptError("unterminated string literal", start_line, script_name);
        }
        if (c == '"') { advance(); break; }
        if (c == '\\') {
            advance();
            switch (current_char()) {
                case 'n':  text += '\n'; break;
                case 't':  text += '\t'; break;
                case 'r':  text += '\r'; break;
                case '\\': text += '\\'; break;
                case '"':  text += '"';  break;
                case '0':  text += '\0'; break;
                default:
                    throw ScriptError(std::string("unknown escape sequence '\\") + current_char() + "'",
                                      line, script_name);
            }
            advance();
            continue;
        }
        text += c;
        advance();
    }
    return Token{ TokenType::STRING, text, start_line, start_col };
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (current_char() != '\0') {
        if (std::isspace(static_cast<unsigned char>(current_char()))) {
            skip_whitespace();
            continue;
        }

        if (current_char() == '/' && peek() == '/') { skip_line_comment(); continue; }
        if (current_char() == '/' && peek() == '*') { skip_block_comment(); continue; }

        const int tok_line = line;
        const int tok_col = column;

        if (std::isalpha(static_cast<unsigned char>(current_char())) || current_char() == '_') {
            std::string text;
            while (std::isalnum(static_cast<unsigned char>(current_char())) || current_char() == '_') {
                text += current_char();
                advance();
            }
            TokenType type = TokenType::IDENTIFIER;
            if (text == "while")         type = TokenType::WHILE;
            else if (text == "if")       type = TokenType::IF;
            else if (text == "else")     type = TokenType::ELSE;
            else if (text == "for")      type = TokenType::FOR;
            else if (text == "function") type = TokenType::FUNCTION;
            else if (text == "return")   type = TokenType::RETURN;
            else if (text == "break")    type = TokenType::BREAK;
            else if (text == "continue") type = TokenType::CONTINUE;
            else if (text == "array")    type = TokenType::ARRAY;
            else if (text == "global")   type = TokenType::GLOBAL;
            else if (text == "true")     type = TokenType::TRUE_LITERAL;
            else if (text == "false")    type = TokenType::FALSE_LITERAL;
            tokens.push_back({ type, text, tok_line, tok_col });
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(current_char())) ||
            (current_char() == '.' && std::isdigit(static_cast<unsigned char>(peek())))) {
            tokens.push_back(lex_number());
            continue;
        }

        if (current_char() == '"') {
            tokens.push_back(lex_string());
            continue;
        }

        auto push = [&](TokenType type, const char* text, int consume) {
            for (int i = 0; i < consume; ++i) advance();
            tokens.push_back({ type, text, tok_line, tok_col });
        };

        switch (current_char()) {
            case '=': peek() == '=' ? push(TokenType::EQUAL_EQUAL, "==", 2) : push(TokenType::EQUALS, "=", 1); break;
            case '!': peek() == '=' ? push(TokenType::NOT_EQUAL, "!=", 2) : push(TokenType::NOT, "!", 1); break;
            case '<': peek() == '=' ? push(TokenType::LESS_EQUAL, "<=", 2) : push(TokenType::LESS, "<", 1); break;
            case '>': peek() == '=' ? push(TokenType::GREATER_EQUAL, ">=", 2) : push(TokenType::GREATER, ">", 1); break;
            case '&':
                if (peek() == '&') push(TokenType::AND, "&&", 2);
                else throw ScriptError("stray '&' (did you mean '&&'?)", tok_line, script_name);
                break;
            case '|':
                if (peek() == '|') push(TokenType::OR, "||", 2);
                else throw ScriptError("stray '|' (did you mean '||'?)", tok_line, script_name);
                break;
            case '%': push(TokenType::MODULO, "%", 1); break;
            case '+': peek() == '=' ? push(TokenType::PLUS_EQUAL, "+=", 2) : push(TokenType::PLUS, "+", 1); break;
            case '-': peek() == '=' ? push(TokenType::MINUS_EQUAL, "-=", 2) : push(TokenType::MINUS, "-", 1); break;
            case '*': peek() == '=' ? push(TokenType::MULTIPLY_EQUAL, "*=", 2) : push(TokenType::MULTIPLY, "*", 1); break;
            case '/': peek() == '=' ? push(TokenType::DIVIDE_EQUAL, "/=", 2) : push(TokenType::DIVIDE, "/", 1); break;
            case '(': push(TokenType::LPAREN, "(", 1); break;
            case ')': push(TokenType::RPAREN, ")", 1); break;
            case '{': push(TokenType::LBRACE, "{", 1); break;
            case '}': push(TokenType::RBRACE, "}", 1); break;
            case '[': push(TokenType::LBRACKET, "[", 1); break;
            case ']': push(TokenType::RBRACKET, "]", 1); break;
            case ';': push(TokenType::SEMICOLON, ";", 1); break;
            case ',': push(TokenType::COMMA, ",", 1); break;
            case '.': push(TokenType::DOT, ".", 1); break;
            default:
                throw ScriptError(std::string("unexpected character '") + current_char() + "'", tok_line, script_name);
        }
    }
    tokens.push_back({ TokenType::END_OF_FILE, "", line, column });
    return tokens;
}

// =============================================================================
//  Parser
// =============================================================================

Parser::Parser(const std::vector<Token>& tokens, std::string script_name)
    : tokens(tokens), script_name(std::move(script_name)), pos(0) {}

const Token& Parser::current_token() const {
    if (pos >= tokens.size()) return tokens.back();
    return tokens[pos];
}

const Token& Parser::peek_token(size_t offset) const {
    if (pos + offset >= tokens.size()) return tokens.back();
    return tokens[pos + offset];
}

void Parser::advance() {
    if (pos < tokens.size()) pos++;
}

void Parser::fail(const std::string& message) const {
    throw ScriptError(message, current_token().line, script_name);
}

static const char* describe(TokenType type) {
    switch (type) {
        case TokenType::LPAREN: return "'('";
        case TokenType::RPAREN: return "')'";
        case TokenType::LBRACE: return "'{'";
        case TokenType::RBRACE: return "'}'";
        case TokenType::LBRACKET: return "'['";
        case TokenType::RBRACKET: return "']'";
        case TokenType::SEMICOLON: return "';'";
        case TokenType::COMMA: return "','";
        case TokenType::EQUALS: return "'='";
        case TokenType::IDENTIFIER: return "an identifier";
        default: return "token";
    }
}

Token Parser::expect(TokenType type, const char* what) {
    const Token& t = current_token();
    if (t.type == type) {
        Token copy = t;
        advance();
        return copy;
    }
    const std::string found = t.type == TokenType::END_OF_FILE ? "end of file" : ("'" + t.text + "'");
    throw ScriptError(std::string("expected ") + describe(type) + " " + what + ", found " + found,
                      t.line, script_name);
}

std::vector<std::unique_ptr<ASTNode>> Parser::parse() {
    std::vector<std::unique_ptr<ASTNode>> statements;
    while (current_token().type != TokenType::END_OF_FILE) {
        statements.push_back(parse_statement());
    }
    return statements;
}

std::unique_ptr<ASTNode> Parser::parse_function_declaration() {
    const int decl_line = current_token().line;
    advance(); // consume 'function'
    Token name = expect(TokenType::IDENTIFIER, "after 'function'");
    expect(TokenType::LPAREN, "after function name");

    std::vector<std::string> parameters;
    if (current_token().type != TokenType::RPAREN) {
        while (true) {
            Token param = expect(TokenType::IDENTIFIER, "in parameter list");
            if (std::find(parameters.begin(), parameters.end(), param.text) != parameters.end()) {
                throw ScriptError("duplicate parameter '" + param.text + "'", param.line, script_name);
            }
            parameters.push_back(param.text);
            if (current_token().type != TokenType::COMMA) break;
            advance();
        }
    }
    expect(TokenType::RPAREN, "after parameter list");
    if (current_token().type != TokenType::LBRACE) {
        fail("expected '{' to open the body of function '" + name.text + "'");
    }
    auto body = parse_statement();
    return std::make_unique<FunctionDeclNode>(name.text, std::move(parameters), std::move(body), decl_line);
}

// A "simple statement" is an assignment or a bare expression - the forms that are
// legal in a for-loop header, where a trailing semicolon is not always wanted.
std::unique_ptr<ASTNode> Parser::parse_simple_statement(bool consume_semicolon) {
    const int stmt_line = current_token().line;

    bool force_global = false;
    if (current_token().type == TokenType::GLOBAL) {
        advance();
        force_global = true;
        if (current_token().type != TokenType::IDENTIFIER) {
            fail("expected a variable name after 'global'");
        }
    }

    if (current_token().type == TokenType::IDENTIFIER) {
        const TokenType next = peek_token().type;

        auto compound_op = [](TokenType t) {
            switch (t) {
                case TokenType::PLUS_EQUAL:     return TokenType::PLUS;
                case TokenType::MINUS_EQUAL:    return TokenType::MINUS;
                case TokenType::MULTIPLY_EQUAL: return TokenType::MULTIPLY;
                case TokenType::DIVIDE_EQUAL:   return TokenType::DIVIDE;
                default:                        return TokenType::UNKNOWN;
            }
        };

        // name = expr;   /   name += expr;
        if (next == TokenType::EQUALS || compound_op(next) != TokenType::UNKNOWN) {
            Token id = current_token();
            advance();
            const TokenType op_token = current_token().type;
            advance();
            auto expr = parse_expression();
            if (op_token != TokenType::EQUALS) {
                // Desugared rather than given its own node: `a += b` and
                // `a = a + b` should never be able to diverge.
                Token op{ compound_op(op_token), "", stmt_line, id.column };
                expr = std::make_unique<BinaryOpNode>(
                    std::make_unique<IdentifierNode>(id.text, stmt_line), op, std::move(expr), stmt_line);
            }
            if (consume_semicolon) expect(TokenType::SEMICOLON, "after assignment");
            return std::make_unique<AssignmentNode>(id.text, std::move(expr), stmt_line, force_global);
        }

        // name.x = expr;   /   name.x += expr;
        if (next == TokenType::DOT && peek_token(2).type == TokenType::IDENTIFIER) {
            const TokenType after = peek_token(3).type;
            if (after == TokenType::EQUALS || compound_op(after) != TokenType::UNKNOWN) {
                Token id = current_token();
                advance(); advance();
                Token member = current_token();
                advance();
                int component = -1;
                if (member.text == "x") component = 0;
                else if (member.text == "y") component = 1;
                else if (member.text == "z") component = 2;
                else throw ScriptError("'" + member.text + "' is not a vec3 component (use .x, .y or .z)",
                                       member.line, script_name);
                const TokenType op_token = current_token().type;
                advance();
                auto expr = parse_expression();
                if (op_token != TokenType::EQUALS) {
                    Token op{ compound_op(op_token), "", stmt_line, id.column };
                    expr = std::make_unique<BinaryOpNode>(
                        std::make_unique<MemberAccessNode>(
                            std::make_unique<IdentifierNode>(id.text, stmt_line), component, stmt_line),
                        op, std::move(expr), stmt_line);
                }
                if (consume_semicolon) expect(TokenType::SEMICOLON, "after assignment");
                return std::make_unique<MemberAssignmentNode>(id.text, component, std::move(expr), stmt_line, force_global);
            }
        }

        // name[index] = expr;   /   name[index] += expr;
        if (next == TokenType::LBRACKET) {
            // Only an assignment when a '=' follows the closing bracket; otherwise
            // this is an expression statement such as a bare `arr[i];`.
            size_t scan = pos + 2;
            int depth = 1;
            while (scan < tokens.size() && depth > 0) {
                if (tokens[scan].type == TokenType::LBRACKET) depth++;
                else if (tokens[scan].type == TokenType::RBRACKET) depth--;
                else if (tokens[scan].type == TokenType::END_OF_FILE) break;
                scan++;
            }
            const TokenType after = scan < tokens.size() ? tokens[scan].type : TokenType::END_OF_FILE;
            if (after == TokenType::EQUALS || compound_op(after) != TokenType::UNKNOWN) {
                Token id = current_token();
                advance(); advance(); // name [
                auto index_expr = parse_expression();
                expect(TokenType::RBRACKET, "after array index");
                const TokenType op_token = current_token().type;
                advance();
                auto val_expr = parse_expression();
                if (op_token != TokenType::EQUALS) {
                    // The index expression is duplicated here, so it must be side
                    // effect free for `a[f()] += 1` to behave. Cloning an AST is not
                    // worth it for a language with no side-effecting index idioms;
                    // this is called out so a future reader knows it is deliberate.
                    fail("compound assignment to an array element is not supported; write a[i] = a[i] + x;");
                }
                if (consume_semicolon) expect(TokenType::SEMICOLON, "after array assignment");
                return std::make_unique<ArrayAssignmentNode>(id.text, std::move(index_expr),
                                                             std::move(val_expr), stmt_line);
            }
        }
    }

    if (force_global) fail("'global' must be followed by an assignment");

    auto expr = parse_expression();
    if (consume_semicolon) expect(TokenType::SEMICOLON, "after expression statement");
    return expr;
}

std::unique_ptr<ASTNode> Parser::parse_statement() {
    const int stmt_line = current_token().line;

    switch (current_token().type) {
        case TokenType::FUNCTION:
            return parse_function_declaration();

        case TokenType::WHILE: {
            advance();
            expect(TokenType::LPAREN, "after 'while'");
            auto cond = parse_expression();
            expect(TokenType::RPAREN, "after while condition");
            auto body = parse_statement();
            return std::make_unique<WhileNode>(std::move(cond), std::move(body), stmt_line);
        }

        case TokenType::FOR: {
            advance();
            expect(TokenType::LPAREN, "after 'for'");
            std::unique_ptr<ASTNode> init;
            if (current_token().type != TokenType::SEMICOLON) init = parse_simple_statement(false);
            expect(TokenType::SEMICOLON, "after for-loop initialiser");
            std::unique_ptr<ExpressionNode> cond;
            if (current_token().type != TokenType::SEMICOLON) cond = parse_expression();
            expect(TokenType::SEMICOLON, "after for-loop condition");
            std::unique_ptr<ASTNode> step;
            if (current_token().type != TokenType::RPAREN) step = parse_simple_statement(false);
            expect(TokenType::RPAREN, "after for-loop header");
            auto body = parse_statement();
            return std::make_unique<ForNode>(std::move(init), std::move(cond), std::move(step),
                                             std::move(body), stmt_line);
        }

        case TokenType::IF: {
            advance();
            expect(TokenType::LPAREN, "after 'if'");
            auto cond = parse_expression();
            expect(TokenType::RPAREN, "after if condition");
            auto then_branch = parse_statement();
            std::unique_ptr<ASTNode> else_branch;
            if (current_token().type == TokenType::ELSE) {
                advance();
                else_branch = parse_statement();
            }
            return std::make_unique<IfNode>(std::move(cond), std::move(then_branch),
                                            std::move(else_branch), stmt_line);
        }

        case TokenType::LBRACE: {
            advance();
            std::vector<std::unique_ptr<ASTNode>> stmts;
            while (current_token().type != TokenType::RBRACE) {
                if (current_token().type == TokenType::END_OF_FILE) {
                    throw ScriptError("unclosed '{' - expected '}'", stmt_line, script_name);
                }
                stmts.push_back(parse_statement());
            }
            advance(); // consume '}'
            return std::make_unique<BlockNode>(std::move(stmts), stmt_line);
        }

        case TokenType::RETURN: {
            advance();
            std::unique_ptr<ExpressionNode> expr;
            if (current_token().type != TokenType::SEMICOLON) expr = parse_expression();
            expect(TokenType::SEMICOLON, "after 'return'");
            return std::make_unique<ReturnNode>(std::move(expr), stmt_line);
        }

        case TokenType::BREAK:
            advance();
            expect(TokenType::SEMICOLON, "after 'break'");
            return std::make_unique<BreakNode>(stmt_line);

        case TokenType::CONTINUE:
            advance();
            expect(TokenType::SEMICOLON, "after 'continue'");
            return std::make_unique<ContinueNode>(stmt_line);

        case TokenType::ARRAY: {
            advance();
            Token name = expect(TokenType::IDENTIFIER, "after 'array'");
            expect(TokenType::LBRACKET, "after array name");
            auto size = parse_expression();
            expect(TokenType::RBRACKET, "after array size");
            expect(TokenType::SEMICOLON, "after array declaration");
            return std::make_unique<ArrayDeclNode>(name.text, std::move(size), stmt_line);
        }

        case TokenType::SEMICOLON:
            // An empty statement. Cheaper to accept than to make every generated
            // script careful about stray semicolons.
            advance();
            return std::make_unique<BlockNode>(std::vector<std::unique_ptr<ASTNode>>{}, stmt_line);

        default:
            break;
    }

    return parse_simple_statement(true);
}

std::unique_ptr<ExpressionNode> Parser::parse_expression() { return parse_logical_or(); }

std::unique_ptr<ExpressionNode> Parser::parse_logical_or() {
    auto left = parse_logical_and();
    while (current_token().type == TokenType::OR) {
        Token op = current_token();
        advance();
        auto right = parse_logical_and();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right), op.line);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_logical_and() {
    auto left = parse_equality();
    while (current_token().type == TokenType::AND) {
        Token op = current_token();
        advance();
        auto right = parse_equality();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right), op.line);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_equality() {
    auto left = parse_relational();
    while (current_token().type == TokenType::EQUAL_EQUAL || current_token().type == TokenType::NOT_EQUAL) {
        Token op = current_token();
        advance();
        auto right = parse_relational();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right), op.line);
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
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right), op.line);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_additive() {
    auto left = parse_multiplicative();
    while (current_token().type == TokenType::PLUS || current_token().type == TokenType::MINUS) {
        Token op = current_token();
        advance();
        auto right = parse_multiplicative();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right), op.line);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_multiplicative() {
    auto left = parse_unary();
    while (current_token().type == TokenType::MULTIPLY || current_token().type == TokenType::DIVIDE ||
           current_token().type == TokenType::MODULO) {
        Token op = current_token();
        advance();
        auto right = parse_unary();
        left = std::make_unique<BinaryOpNode>(std::move(left), op, std::move(right), op.line);
    }
    return left;
}

std::unique_ptr<ExpressionNode> Parser::parse_unary() {
    if (current_token().type == TokenType::NOT || current_token().type == TokenType::MINUS) {
        Token op = current_token();
        advance();
        return std::make_unique<UnaryOpNode>(op, parse_unary(), op.line);
    }
    return parse_postfix();
}

std::unique_ptr<ExpressionNode> Parser::parse_postfix() {
    auto expr = parse_factor();
    while (current_token().type == TokenType::DOT) {
        const int dot_line = current_token().line;
        advance();
        Token member = expect(TokenType::IDENTIFIER, "after '.'");
        int component = -1;
        if (member.text == "x") component = 0;
        else if (member.text == "y") component = 1;
        else if (member.text == "z") component = 2;
        else throw ScriptError("'" + member.text + "' is not a vec3 component (use .x, .y or .z)",
                               member.line, script_name);
        expr = std::make_unique<MemberAccessNode>(std::move(expr), component, dot_line);
    }
    return expr;
}

std::unique_ptr<ExpressionNode> Parser::parse_factor() {
    const Token t = current_token();
    switch (t.type) {
        case TokenType::NUMBER:
            advance();
            return std::make_unique<NumberNode>(std::stof(t.text), t.line);

        case TokenType::STRING:
            advance();
            return std::make_unique<StringNode>(t.text, t.line);

        case TokenType::TRUE_LITERAL:
            advance();
            return std::make_unique<NumberNode>(1.0f, t.line);

        case TokenType::FALSE_LITERAL:
            advance();
            return std::make_unique<NumberNode>(0.0f, t.line);

        case TokenType::IDENTIFIER: {
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
                expect(TokenType::RPAREN, "after call arguments");
                return std::make_unique<FunctionCallNode>(t.text, std::move(args), t.line);
            }
            if (current_token().type == TokenType::LBRACKET) {
                advance();
                auto index_expr = parse_expression();
                expect(TokenType::RBRACKET, "after array index");
                return std::make_unique<ArrayAccessNode>(t.text, std::move(index_expr), t.line);
            }
            return std::make_unique<IdentifierNode>(t.text, t.line);
        }

        case TokenType::LPAREN: {
            advance();
            auto expr = parse_expression();
            expect(TokenType::RPAREN, "to close a parenthesised expression");
            return expr;
        }

        default:
            break;
    }
    const std::string found = t.type == TokenType::END_OF_FILE ? "end of file" : ("'" + t.text + "'");
    throw ScriptError("expected a value, found " + found, t.line, script_name);
}


// =============================================================================
//  Interpreter
//
//  A tree-walking evaluator over the AST the parser above produces. The shape is
//  dictated by the execution model at the top of the header: top-level statements
//  re-run every frame, so nothing here may accumulate per-frame state. Function
//  declarations are hoisted, locals live in an explicit frame stack, and control
//  flow is signalled with flags rather than exceptions because `return` inside a
//  per-frame loop is common enough that throwing would show up in a profile.
// =============================================================================

Interpreter::Interpreter(Actor* owner) : actor_owner(owner) {
    // "time" and "dt" are read by scripts before anything writes them - the host
    // updates them each tick - so they exist from construction rather than
    // materialising as 0 on first read.
    variables["time"] = Value::number(0.0f);
    variables["dt"] = Value::number(0.0f);
}

void Interpreter::error(int line, const std::string& message) const {
    throw ScriptError(message, line, script_name);
}

float Interpreter::require_number(const Value& v, int line, const char* what) const {
    if (!v.is_number()) {
        error(line, std::string(what) + " expects a number, got " + v.type_name());
    }
    return v.x;
}

Vector3 Interpreter::require_vec3(const Value& v, int line, const char* what) const {
    if (!v.is_vec3()) {
        error(line, std::string(what) + " expects a vec3, got " + v.type_name());
    }
    return v.as_vector3();
}

// --- Variables ---------------------------------------------------------------

Value Interpreter::get_variable(const std::string& name) const {
    if (!call_frames.empty()) {
        const auto& frame = call_frames.back();
        auto local = frame.find(name);
        if (local != frame.end()) return local->second;
    }
    auto global = variables.find(name);
    if (global != variables.end()) return global->second;
    // Undefined reads are zero rather than an error. Existing scripts lean on this
    // heavily (a counter is incremented before it is ever initialised), and making
    // it an error would break every one of them.
    return Value::number(0.0f);
}

void Interpreter::set_variable(const std::string& name, const Value& value) {
    if (!call_frames.empty()) {
        // Inside a function a plain assignment is local, so a helper that happens to
        // use the name "score" cannot quietly overwrite the game's score. Writing the
        // outer variable is opt-in, through `global`.
        call_frames.back()[name] = value;
        return;
    }
    variables[name] = value;
}

void Interpreter::set_global(const std::string& name, const Value& value) {
    variables[name] = value;
}

// --- Arrays ------------------------------------------------------------------

Value& Interpreter::array_slot(const std::string& name, const Value& index_value,
                               int line, bool for_write) {
    const float raw = require_number(index_value, line, ("index of array '" + name + "'").c_str());
    if (!std::isfinite(raw)) {
        error(line, "array '" + name + "' indexed with a non-finite value");
    }
    const int index = static_cast<int>(raw);
    if (index < 0) {
        error(line, "array '" + name + "' indexed with negative index " + std::to_string(index));
    }
    if (index >= kMaxArrayElements) {
        error(line, "array '" + name + "' index " + std::to_string(index) +
                        " exceeds the maximum of " + std::to_string(kMaxArrayElements));
    }

    ArrayData& array = arrays[name];
    if (array.declared_size >= 0 && index >= array.declared_size) {
        error(line, "array '" + name + "' index " + std::to_string(index) +
                        " is outside its declared size of " + std::to_string(array.declared_size));
    }

    // Grows on read as well as on write. Scripts routinely read a table before ever
    // assigning to it and expect zero; erroring on that would break them, so "out of
    // range" above means negative, non-finite, past the cap or past a declared size -
    // never merely past what has been written so far.
    (void)for_write;
    if (static_cast<size_t>(index) >= array.data.size()) {
        array.data.resize(static_cast<size_t>(index) + 1, Value::number(0.0f));
    }
    return array.data[static_cast<size_t>(index)];
}

// --- Debugger ----------------------------------------------------------------

void Interpreter::debug_step(const ASTNode* stmt) {
    if (!debug.active || stmt == nullptr) return;
    ++debug.statement_index;

    bool pause = false;
    // Stepping: pause once the statement counter reaches the target, ignoring
    // statements deeper than the step started at so step-over does not descend
    // into every call the statement makes.
    if (debug.step_target >= 0 && debug.statement_index >= debug.step_target &&
        debug.depth <= debug.step_max_depth) {
        pause = true;
    }
    if (!pause && !debug.breakpoint_lines.empty() &&
        std::binary_search(debug.breakpoint_lines.begin(), debug.breakpoint_lines.end(),
                           stmt->line)) {
        pause = true;
    }
    if (pause) {
        throw PauseSignal{stmt->line, debug.statement_index, debug.depth};
    }
}

// --- Program entry -----------------------------------------------------------

void Interpreter::bind_program(const std::vector<std::unique_ptr<ASTNode>>& program) {
    // Hoisted: a script may call a helper declared further down the file, which is
    // how anyone reasonably expects to be able to write one.
    if (bound_program == &program) return;
    functions.clear();
    for (const auto& node : program) {
        if (node && node->kind == NodeKind::FunctionDecl) {
            auto* fn = static_cast<FunctionDeclNode*>(node.get());
            functions[fn->name] = fn;
        }
    }
    bound_program = &program;
}

void Interpreter::execute(const std::vector<std::unique_ptr<ASTNode>>& program) {
    bind_program(program);

    // Every execute() is a fresh frame of script time: the statement counter the
    // debugger steps against restarts, and any control flow left set by a previous
    // run (a `return` at the top level) is cleared so it cannot suppress this one.
    debug.statement_index = 0;
    debug.depth = 0;
    returning = breaking = continuing = false;
    return_value = Value::nothing();
    call_frames.clear();
    call_depth = 0;

    for (const auto& node : program) {
        if (!node || node->kind == NodeKind::FunctionDecl) continue;
        execute_statement(node.get());
        if (returning) break;
    }
}

bool Interpreter::has_script_function(const std::string& name) const {
    return functions.find(name) != functions.end();
}

bool Interpreter::call_script_function(const std::string& name, const std::vector<Value>& args,
                                       Value* out_result) {
    auto it = functions.find(name);
    // Not an error: a script defining only on_tick is entirely normal, and the host
    // asks for every event function it supports on every one of them.
    if (it == functions.end() || it->second == nullptr) return false;

    returning = breaking = continuing = false;
    const Value result = call_user_function(*it->second, args, it->second->line);
    if (out_result) *out_result = result;
    return true;
}

Value Interpreter::call_user_function(const FunctionDeclNode& fn, const std::vector<Value>& args,
                                      int line) {
    if (call_depth >= kMaxCallDepth) {
        error(line, "call depth limit of " + std::to_string(kMaxCallDepth) +
                        " exceeded calling '" + fn.name + "' (infinite recursion?)");
    }

    // Missing arguments are zero and extra ones are dropped, rather than being an
    // arity error. The visual script editor can emit a call whose pins the user has
    // not all connected yet, and refusing to run it would make the graph unusable
    // mid-edit.
    std::unordered_map<std::string, Value> frame;
    for (size_t i = 0; i < fn.parameters.size(); ++i) {
        frame[fn.parameters[i]] = (i < args.size()) ? args[i] : Value::number(0.0f);
    }

    call_frames.push_back(std::move(frame));
    ++call_depth;
    ++debug.depth;

    const bool outer_returning = returning;
    returning = false;
    return_value = Value::nothing();

    // The frame must come off the stack even if the body throws - a ScriptError
    // propagates to the host, which keeps using this interpreter afterwards, and a
    // leaked frame would make every later global write land in a dead local scope.
    try {
        if (fn.body) execute_statement(fn.body.get());
    } catch (...) {
        call_frames.pop_back();
        --call_depth;
        --debug.depth;
        returning = outer_returning;
        throw;
    }

    Value result = return_value;
    call_frames.pop_back();
    --call_depth;
    --debug.depth;
    returning = outer_returning;
    return_value = Value::nothing();
    // `break`/`continue` cannot escape a function into its caller's loop.
    breaking = continuing = false;
    return result;
}

// --- Statements --------------------------------------------------------------

void Interpreter::execute_block(const std::vector<std::unique_ptr<ASTNode>>& statements) {
    for (const auto& stmt : statements) {
        if (!stmt) continue;
        execute_statement(stmt.get());
        if (control_flow_pending()) return;
    }
}

void Interpreter::execute_statement(ASTNode* stmt) {
    if (!stmt) return;
    debug_step(stmt);

    switch (stmt->kind) {
    case NodeKind::Assignment: {
        auto* node = static_cast<AssignmentNode*>(stmt);
        const Value value = evaluate(node->expression.get());
        if (node->force_global) {
            set_global(node->identifier, value);
        } else {
            set_variable(node->identifier, value);
        }
        break;
    }

    case NodeKind::ArrayAssignment: {
        auto* node = static_cast<ArrayAssignmentNode*>(stmt);
        // The value is evaluated before the slot is resolved: evaluating it can
        // grow the same array (a[i] = a[i + 1]), which would dangle a reference
        // taken first.
        const Value value = evaluate(node->expression.get());
        const Value index = evaluate(node->index.get());
        array_slot(node->array_name, index, node->line, true) = value;
        break;
    }

    case NodeKind::MemberAssignment: {
        auto* node = static_cast<MemberAssignmentNode*>(stmt);
        const Value value = evaluate(node->expression.get());
        const float scalar = require_number(value, node->line, "assigning to a vector component");

        Value target = get_variable(node->identifier);
        if (!target.is_vec3()) {
            // Writing a component of something that was never a vector promotes it
            // from the default zero rather than erroring, so `pos.y = 3;` works as a
            // first statement. A genuine type confusion (a string) still reports.
            if (target.is_string()) {
                error(node->line, "cannot assign to a component of string '" + node->identifier + "'");
            }
            target = Value::vec(0.0f, 0.0f, 0.0f);
        }
        if (node->component == 0) target.x = scalar;
        else if (node->component == 1) target.y = scalar;
        else target.z = scalar;

        if (node->force_global) set_global(node->identifier, target);
        else set_variable(node->identifier, target);
        break;
    }

    case NodeKind::ArrayDecl: {
        auto* node = static_cast<ArrayDeclNode*>(stmt);
        const Value size_value = evaluate(node->size.get());
        const float raw = require_number(size_value, node->line, "array size");
        if (!std::isfinite(raw) || raw < 0.0f) {
            error(node->line, "array '" + node->name + "' declared with an invalid size");
        }
        const int size = static_cast<int>(raw);
        if (size > kMaxArrayElements) {
            error(node->line, "array '" + node->name + "' declared with size " +
                                  std::to_string(size) + ", above the maximum of " +
                                  std::to_string(kMaxArrayElements));
        }
        ArrayData& array = arrays[node->name];
        array.declared_size = size;
        array.data.assign(static_cast<size_t>(size), Value::number(0.0f));
        break;
    }

    case NodeKind::Block:
        execute_block(static_cast<BlockNode*>(stmt)->statements);
        break;

    case NodeKind::If: {
        auto* node = static_cast<IfNode*>(stmt);
        if (evaluate(node->condition.get()).truthy()) {
            execute_statement(node->then_branch.get());
        } else if (node->else_branch) {
            execute_statement(node->else_branch.get());
        }
        break;
    }

    case NodeKind::While: {
        auto* node = static_cast<WhileNode*>(stmt);
        int iterations = 0;
        while (evaluate(node->condition.get()).truthy()) {
            if (++iterations > kMaxLoopIterations) {
                error(node->line, "loop exceeded " + std::to_string(kMaxLoopIterations) +
                                      " iterations (infinite loop?)");
            }
            execute_statement(node->body.get());
            if (returning) return;
            if (breaking) { breaking = false; break; }
            continuing = false;
        }
        break;
    }

    case NodeKind::For: {
        auto* node = static_cast<ForNode*>(stmt);
        if (node->init) execute_statement(node->init.get());
        int iterations = 0;
        // A missing condition means "true"; `for (;;)` is still bounded by the
        // iteration cap below.
        while (!node->condition || evaluate(node->condition.get()).truthy()) {
            if (++iterations > kMaxLoopIterations) {
                error(node->line, "loop exceeded " + std::to_string(kMaxLoopIterations) +
                                      " iterations (infinite loop?)");
            }
            execute_statement(node->body.get());
            if (returning) return;
            if (breaking) { breaking = false; break; }
            continuing = false;
            // The step runs after a `continue`, which is what makes `continue` inside
            // a counting loop advance rather than spin forever.
            if (node->step) execute_statement(node->step.get());
        }
        break;
    }

    case NodeKind::Return: {
        auto* node = static_cast<ReturnNode*>(stmt);
        return_value = node->expression ? evaluate(node->expression.get()) : Value::nothing();
        returning = true;
        break;
    }

    case NodeKind::Break:
        breaking = true;
        break;

    case NodeKind::Continue:
        continuing = true;
        break;

    case NodeKind::FunctionDecl:
        // Hoisted by bind_program; reaching one in statement position is a no-op.
        break;

    default:
        // Anything left is an expression evaluated for its effect - overwhelmingly a
        // call to a builtin that returns Void.
        evaluate(static_cast<ExpressionNode*>(stmt));
        break;
    }
}

// --- Expressions -------------------------------------------------------------

namespace {

// Component-wise for two vectors, and broadcast when one side is a scalar, which is
// what makes `v * 2` and `v + offset` both read the way they look.
Value vector_arithmetic(const Value& a, const Value& b, TokenType op, bool* ok) {
    *ok = true;
    const bool a_vec = a.is_vec3();
    const bool b_vec = b.is_vec3();
    const float ax = a.x, ay = a_vec ? a.y : a.x, az = a_vec ? a.z : a.x;
    const float bx = b.x, by = b_vec ? b.y : b.x, bz = b_vec ? b.z : b.x;

    switch (op) {
    case TokenType::PLUS:     return Value::vec(ax + bx, ay + by, az + bz);
    case TokenType::MINUS:    return Value::vec(ax - bx, ay - by, az - bz);
    case TokenType::MULTIPLY: return Value::vec(ax * bx, ay * by, az * bz);
    case TokenType::DIVIDE:   return Value::vec(ax / bx, ay / by, az / bz);
    default: *ok = false; return Value::nothing();
    }
}

bool values_equal(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case ValueType::Number: return a.x == b.x;
    case ValueType::Vec3:   return a.x == b.x && a.y == b.y && a.z == b.z;
    case ValueType::String: return a.as_string() == b.as_string();
    case ValueType::Void:   return true;
    }
    return false;
}

} // namespace

Value Interpreter::evaluate(ExpressionNode* node) {
    if (!node) return Value::number(0.0f);

    switch (node->kind) {
    case NodeKind::Number:
        return Value::number(static_cast<NumberNode*>(node)->value);

    case NodeKind::StringLiteral: {
        Value v;
        v.type = ValueType::String;
        v.text = static_cast<StringNode*>(node)->value;
        return v;
    }

    case NodeKind::Identifier:
        return get_variable(static_cast<IdentifierNode*>(node)->name);

    case NodeKind::ArrayAccess: {
        auto* access = static_cast<ArrayAccessNode*>(node);
        const Value index = evaluate(access->index.get());
        return array_slot(access->array_name, index, access->line, false);
    }

    case NodeKind::MemberAccess: {
        auto* access = static_cast<MemberAccessNode*>(node);
        const Value object = evaluate(access->object.get());
        const Vector3 v = require_vec3(object, access->line, "member access");
        if (access->component == 0) return Value::number(v.x);
        if (access->component == 1) return Value::number(v.y);
        return Value::number(v.z);
    }

    case NodeKind::UnaryOp: {
        auto* unary = static_cast<UnaryOpNode*>(node);
        const Value operand = evaluate(unary->operand.get());
        switch (unary->op.type) {
        case TokenType::MINUS:
            if (operand.is_vec3()) return Value::vec(-operand.x, -operand.y, -operand.z);
            return Value::number(-require_number(operand, unary->line, "unary '-'"));
        case TokenType::NOT:
            return Value::boolean(!operand.truthy());
        case TokenType::PLUS:
            return operand;
        default:
            error(unary->line, "unsupported unary operator '" + unary->op.text + "'");
        }
    }

    case NodeKind::BinaryOp: {
        auto* bin = static_cast<BinaryOpNode*>(node);
        const TokenType op = bin->op.type;

        // Short-circuit before the right side is touched, so `if (n > 0 && a[n] ...)`
        // does not evaluate the guarded side when the guard fails.
        if (op == TokenType::AND) {
            if (!evaluate(bin->left.get()).truthy()) return Value::boolean(false);
            return Value::boolean(evaluate(bin->right.get()).truthy());
        }
        if (op == TokenType::OR) {
            if (evaluate(bin->left.get()).truthy()) return Value::boolean(true);
            return Value::boolean(evaluate(bin->right.get()).truthy());
        }

        const Value left = evaluate(bin->left.get());
        const Value right = evaluate(bin->right.get());

        if (op == TokenType::EQUAL_EQUAL) return Value::boolean(values_equal(left, right));
        if (op == TokenType::NOT_EQUAL)   return Value::boolean(!values_equal(left, right));

        // String concatenation. Only '+', and only when the left side is a string, so
        // a stray string in an arithmetic expression still reports a type error
        // instead of silently turning the whole sum into text.
        if (op == TokenType::PLUS && left.is_string()) {
            return Value::string(left.as_string() + right.to_display_string());
        }

        if (left.is_vec3() || right.is_vec3()) {
            if (left.is_string() || right.is_string()) {
                error(bin->line, "cannot apply '" + bin->op.text + "' to a vec3 and a string");
            }
            bool ok = false;
            const Value result = vector_arithmetic(left, right, op, &ok);
            if (!ok) {
                error(bin->line, "operator '" + bin->op.text + "' does not apply to vec3");
            }
            return result;
        }

        const float a = require_number(left, bin->line, ("left operand of '" + bin->op.text + "'").c_str());
        const float b = require_number(right, bin->line, ("right operand of '" + bin->op.text + "'").c_str());

        switch (op) {
        case TokenType::PLUS:     return Value::number(a + b);
        case TokenType::MINUS:    return Value::number(a - b);
        case TokenType::MULTIPLY: return Value::number(a * b);
        case TokenType::DIVIDE:
            // Reported rather than allowed to produce an inf that propagates into a
            // transform and presents as "the game is subtly wrong" three frames later.
            if (b == 0.0f) error(bin->line, "division by zero");
            return Value::number(a / b);
        case TokenType::MODULO:
            if (b == 0.0f) error(bin->line, "modulo by zero");
            return Value::number(std::fmod(a, b));
        case TokenType::LESS:          return Value::boolean(a < b);
        case TokenType::LESS_EQUAL:    return Value::boolean(a <= b);
        case TokenType::GREATER:       return Value::boolean(a > b);
        case TokenType::GREATER_EQUAL: return Value::boolean(a >= b);
        default:
            error(bin->line, "unsupported operator '" + bin->op.text + "'");
        }
    }

    case NodeKind::FunctionCall: {
        auto* call = static_cast<FunctionCallNode*>(node);

        std::vector<Value> args;
        args.reserve(call->arguments.size());
        for (const auto& argument : call->arguments) {
            args.push_back(evaluate(argument.get()));
        }
        const int argc = static_cast<int>(args.size());

        // Resolved once and cached on the node. A name lookup per call per frame is
        // pure overhead: the builtin table is static and user functions are hoisted
        // before the first statement runs, so the answer cannot change.
        if (!call->resolution_attempted) {
            auto user = functions.find(call->function_name);
            if (user != functions.end()) {
                call->resolved_user = user->second;
            } else {
                call->resolved_builtin = find_builtin(call->function_name, argc);
            }
            call->resolution_attempted = true;
        }

        if (call->resolved_user) {
            return call_user_function(*call->resolved_user, args, call->line);
        }
        if (call->resolved_builtin) {
            if (call->resolved_builtin->needs_actor && actor_owner == nullptr) {
                error(call->line, "'" + call->function_name +
                                      "' needs an actor, but this script is not attached to one");
            }
            return call->resolved_builtin->fn(*this, args, call->line);
        }

        // Distinguishing these two matters: "wrong number of arguments" is a typo in
        // the call, "unknown function" is a typo in the name, and they are fixed
        // differently. Before this, both silently evaluated to 0.
        if (builtin_name_exists(call->function_name)) {
            error(call->line, "'" + call->function_name + "' does not take " +
                                  std::to_string(argc) + " argument(s)");
        }
        error(call->line, "unknown function '" + call->function_name + "'");
    }

    default:
        error(node->line, "unsupported expression");
    }
}

} // namespace CMinus
