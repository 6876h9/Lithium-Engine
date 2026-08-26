#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "world/actor.hpp"

namespace CMinus {

enum class TokenType {
    IDENTIFIER, NUMBER,
    EQUALS, PLUS, MINUS, MULTIPLY, DIVIDE,
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET, SEMICOLON, COMMA,
    WHILE, IF, ELSE,
    EQUAL_EQUAL, NOT_EQUAL, LESS, LESS_EQUAL, GREATER, GREATER_EQUAL,
    AND, OR, NOT, MODULO,
    END_OF_FILE, UNKNOWN
};

struct Token {
    TokenType type;
    std::string text;
};

class Lexer {
public:
    Lexer(const std::string& source);
    std::vector<Token> tokenize();
private:
    std::string source;
    size_t pos;
    char current_char();
    void advance();
    void skip_whitespace();
    void skip_comment();
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
};

class ExpressionNode : public ASTNode {};

class NumberNode : public ExpressionNode {
public:
    float value;
    NumberNode(float val) : value(val) {}
};

class IdentifierNode : public ExpressionNode {
public:
    std::string name;
    IdentifierNode(const std::string& n) : name(n) {}
};

class ArrayAccessNode : public ExpressionNode {
public:
    std::string array_name;
    std::unique_ptr<ExpressionNode> index;
    ArrayAccessNode(const std::string& name, std::unique_ptr<ExpressionNode> idx)
        : array_name(name), index(std::move(idx)) {}
};

class ArrayAssignmentNode : public ASTNode {
public:
    std::string array_name;
    std::unique_ptr<ExpressionNode> index;
    std::unique_ptr<ExpressionNode> expression;
    ArrayAssignmentNode(const std::string& name, std::unique_ptr<ExpressionNode> idx, std::unique_ptr<ExpressionNode> expr)
        : array_name(name), index(std::move(idx)), expression(std::move(expr)) {}
};

class BinaryOpNode : public ExpressionNode {
public:
    std::unique_ptr<ExpressionNode> left;
    Token op;
    std::unique_ptr<ExpressionNode> right;
    BinaryOpNode(std::unique_ptr<ExpressionNode> l, Token o, std::unique_ptr<ExpressionNode> r) 
        : left(std::move(l)), op(o), right(std::move(r)) {}
};

class UnaryOpNode : public ExpressionNode {
public:
    Token op;
    std::unique_ptr<ExpressionNode> operand;
    UnaryOpNode(Token o, std::unique_ptr<ExpressionNode> expr)
        : op(o), operand(std::move(expr)) {}
};

class AssignmentNode : public ASTNode {
public:
    std::string identifier;
    std::unique_ptr<ExpressionNode> expression;
    AssignmentNode(const std::string& id, std::unique_ptr<ExpressionNode> expr)
        : identifier(id), expression(std::move(expr)) {}
};

class FunctionCallNode : public ExpressionNode {
public:
    std::string function_name;
    std::vector<std::unique_ptr<ExpressionNode>> arguments;
    FunctionCallNode(const std::string& name, std::vector<std::unique_ptr<ExpressionNode>> args)
        : function_name(name), arguments(std::move(args)) {}
};

class BlockNode : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> statements;
    BlockNode(std::vector<std::unique_ptr<ASTNode>> stmts) : statements(std::move(stmts)) {}
};

class IfNode : public ASTNode {
public:
    std::unique_ptr<ExpressionNode> condition;
    std::unique_ptr<ASTNode> then_branch;
    std::unique_ptr<ASTNode> else_branch;
    IfNode(std::unique_ptr<ExpressionNode> cond, std::unique_ptr<ASTNode> then_br, std::unique_ptr<ASTNode> else_br)
        : condition(std::move(cond)), then_branch(std::move(then_br)), else_branch(std::move(else_br)) {}
};

class WhileNode : public ASTNode {
public:
    std::unique_ptr<ExpressionNode> condition;
    std::unique_ptr<ASTNode> body;
    WhileNode(std::unique_ptr<ExpressionNode> cond, std::unique_ptr<ASTNode> body)
        : condition(std::move(cond)), body(std::move(body)) {}
};

class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::vector<std::unique_ptr<ASTNode>> parse();
private:
    std::vector<Token> tokens;
    size_t pos;
    Token current_token();
    void advance();
    Token expect(TokenType type);

    std::unique_ptr<ExpressionNode> parse_expression();
    std::unique_ptr<ExpressionNode> parse_logical_or();
    std::unique_ptr<ExpressionNode> parse_logical_and();
    std::unique_ptr<ExpressionNode> parse_equality();
    std::unique_ptr<ExpressionNode> parse_relational();
    std::unique_ptr<ExpressionNode> parse_additive();
    std::unique_ptr<ExpressionNode> parse_multiplicative();
    std::unique_ptr<ExpressionNode> parse_unary();
    std::unique_ptr<ExpressionNode> parse_factor();
    std::unique_ptr<ASTNode> parse_statement();
};

class Interpreter {
public:
    Interpreter(Actor* owner = nullptr);
    void execute(const std::vector<std::unique_ptr<ASTNode>>& program);
    void execute_statement(ASTNode* stmt);
    
    // Bindings
    std::unordered_map<std::string, float> variables;
    std::unordered_map<std::string, std::vector<float>> arrays;
    Actor* actor_owner;
    std::vector<Actor*> nearby_cache;

private:
    float evaluate(ExpressionNode* node);
};

} // namespace CMinus
