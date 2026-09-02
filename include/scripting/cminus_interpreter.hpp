#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include "core/math.hpp"

class Actor;

namespace CMinus {

// =============================================================================
//  C-Minus - the engine's own gameplay scripting language.
//
//  A hand-written lexer, recursive-descent parser and tree-walking interpreter.
//  It exists so a designer can write behaviour without a C++ toolchain and
//  without paying for a full Lua VM per actor.
//
//  Execution model, which everything else here follows from:
//  a script's TOP-LEVEL statements are re-executed from the beginning on every
//  frame. There is no persistent instruction pointer. Scripts that need
//  one-time setup gate it on a flag (`if (ready == 0) { ... ready = 1; }`), and
//  scripts that want the Blueprint shape define event functions instead
//  (on_begin_play / on_tick / on_collision_enter / ...), which CMinusComponent
//  calls in place of running the top level. Both styles work; the second is
//  what the visual script editor compiles to.
// =============================================================================

// -----------------------------------------------------------------------------
//  Errors
// -----------------------------------------------------------------------------

// Every lexer, parser and runtime failure carries the line it happened on and
// the name of the script it happened in. Before this existed a script that
// divided by zero produced a silent inf that propagated into a transform, and a
// script with a typo'd function name silently evaluated to 0 - both of which
// present as "the game is subtly wrong" rather than as an error.
class ScriptError : public std::runtime_error {
public:
    ScriptError(const std::string& message, int line, const std::string& script = {})
        : std::runtime_error(format(message, line, script)),
          line(line), script(script), detail(message) {}

    int line;
    std::string script;
    std::string detail;

private:
    static std::string format(const std::string& message, int line, const std::string& script);
};

// -----------------------------------------------------------------------------
//  Tokens
// -----------------------------------------------------------------------------

enum class TokenType {
    IDENTIFIER, NUMBER, STRING,
    EQUALS, PLUS, MINUS, MULTIPLY, DIVIDE,
    PLUS_EQUAL, MINUS_EQUAL, MULTIPLY_EQUAL, DIVIDE_EQUAL,
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET, SEMICOLON, COMMA, DOT,
    WHILE, IF, ELSE, FOR, FUNCTION, RETURN, BREAK, CONTINUE, ARRAY, GLOBAL,
    TRUE_LITERAL, FALSE_LITERAL,
    EQUAL_EQUAL, NOT_EQUAL, LESS, LESS_EQUAL, GREATER, GREATER_EQUAL,
    AND, OR, NOT, MODULO,
    END_OF_FILE, UNKNOWN
};

struct Token {
    TokenType type = TokenType::UNKNOWN;
    std::string text;
    int line = 1;
    int column = 1;
};

class Lexer {
public:
    explicit Lexer(const std::string& source, std::string script_name = {});
    std::vector<Token> tokenize();

private:
    std::string source;
    std::string script_name;
    size_t pos;
    int line;
    int column;

    char current_char() const;
    char peek(size_t offset = 1) const;
    void advance();
    void skip_whitespace();
    void skip_line_comment();
    void skip_block_comment();
    Token lex_number();
    Token lex_string();
};

// -----------------------------------------------------------------------------
//  Values
//
//  Everything used to be a float. That is why gameplay code in the sample game
//  carries positions as three separate scalars and every helper that touches one
//  takes six arguments: there was no way to hold a point. Vec3 is a first-class
//  value here, and String exists so a script can name an actor or a message.
//
//  Layout note: the string payload is a shared_ptr rather than a std::string by
//  value, because a Value is copied for every operand of every expression on
//  every frame. Inline storage for a member almost no Value uses would cost the
//  hot numeric path more than the indirection costs the rare string path.
// -----------------------------------------------------------------------------

enum class ValueType { Number, Vec3, String, Void };

struct Value {
    ValueType type = ValueType::Number;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::shared_ptr<const std::string> text;

    Value() = default;

    static Value number(float v) { Value r; r.type = ValueType::Number; r.x = v; return r; }
    static Value boolean(bool v) { return number(v ? 1.0f : 0.0f); }
    static Value vec(float a, float b, float c) {
        Value r; r.type = ValueType::Vec3; r.x = a; r.y = b; r.z = c; return r;
    }
    static Value vec(const Vector3& v) { return vec(v.x, v.y, v.z); }
    static Value string(std::string s) {
        Value r; r.type = ValueType::String; r.text = std::make_shared<const std::string>(std::move(s)); return r;
    }
    static Value nothing() { Value r; r.type = ValueType::Void; return r; }

    bool is_number() const { return type == ValueType::Number; }
    bool is_vec3() const { return type == ValueType::Vec3; }
    bool is_string() const { return type == ValueType::String; }

    Vector3 as_vector3() const { return Vector3{ x, y, z }; }
    const std::string& as_string() const;

    // A number is true when non-zero, a vector when any component is non-zero, a
    // string when non-empty. Void is always false.
    bool truthy() const;

    // How the debugger's watch panel and print() render this value.
    std::string to_display_string() const;

    static const char* type_name(ValueType t);
    const char* type_name() const { return type_name(type); }
};

// -----------------------------------------------------------------------------
//  AST
//
//  Nodes carry an explicit kind tag. Dispatch used to be a chain of
//  dynamic_cast, which is a string comparison against RTTI names per node per
//  frame; the tag turns the same dispatch into a switch.
// -----------------------------------------------------------------------------

enum class NodeKind {
    Number, StringLiteral, Identifier, ArrayAccess, MemberAccess,
    BinaryOp, UnaryOp, FunctionCall,
    Assignment, ArrayAssignment, MemberAssignment,
    Block, If, While, For, Return, Break, Continue,
    FunctionDecl, ArrayDecl
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
    NodeKind kind;
    int line = 0;

protected:
    ASTNode(NodeKind k, int l) : kind(k), line(l) {}
};

class ExpressionNode : public ASTNode {
protected:
    ExpressionNode(NodeKind k, int l) : ASTNode(k, l) {}
};

class NumberNode : public ExpressionNode {
public:
    float value;
    NumberNode(float val, int line = 0) : ExpressionNode(NodeKind::Number, line), value(val) {}
};

class StringNode : public ExpressionNode {
public:
    std::shared_ptr<const std::string> value;
    StringNode(std::string val, int line = 0)
        : ExpressionNode(NodeKind::StringLiteral, line),
          value(std::make_shared<const std::string>(std::move(val))) {}
};

class IdentifierNode : public ExpressionNode {
public:
    std::string name;
    IdentifierNode(const std::string& n, int line = 0)
        : ExpressionNode(NodeKind::Identifier, line), name(n) {}
};

class ArrayAccessNode : public ExpressionNode {
public:
    std::string array_name;
    std::unique_ptr<ExpressionNode> index;
    ArrayAccessNode(const std::string& name, std::unique_ptr<ExpressionNode> idx, int line = 0)
        : ExpressionNode(NodeKind::ArrayAccess, line), array_name(name), index(std::move(idx)) {}
};

// v.x / v.y / v.z on any expression producing a vec3.
class MemberAccessNode : public ExpressionNode {
public:
    std::unique_ptr<ExpressionNode> object;
    int component; // 0=x 1=y 2=z
    MemberAccessNode(std::unique_ptr<ExpressionNode> obj, int comp, int line = 0)
        : ExpressionNode(NodeKind::MemberAccess, line), object(std::move(obj)), component(comp) {}
};

class BinaryOpNode : public ExpressionNode {
public:
    std::unique_ptr<ExpressionNode> left;
    Token op;
    std::unique_ptr<ExpressionNode> right;
    BinaryOpNode(std::unique_ptr<ExpressionNode> l, Token o, std::unique_ptr<ExpressionNode> r, int line = 0)
        : ExpressionNode(NodeKind::BinaryOp, line), left(std::move(l)), op(std::move(o)), right(std::move(r)) {}
};

class UnaryOpNode : public ExpressionNode {
public:
    Token op;
    std::unique_ptr<ExpressionNode> operand;
    UnaryOpNode(Token o, std::unique_ptr<ExpressionNode> expr, int line = 0)
        : ExpressionNode(NodeKind::UnaryOp, line), op(std::move(o)), operand(std::move(expr)) {}
};

struct BuiltinInfo;
class FunctionDeclNode;

class FunctionCallNode : public ExpressionNode {
public:
    std::string function_name;
    std::vector<std::unique_ptr<ExpressionNode>> arguments;
    FunctionCallNode(const std::string& name, std::vector<std::unique_ptr<ExpressionNode>> args, int line = 0)
        : ExpressionNode(NodeKind::FunctionCall, line), function_name(name), arguments(std::move(args)) {}

    // Resolved on first execution and reused. A name lookup per call per frame is
    // pure overhead once the answer cannot change: the builtin table is static and
    // user functions are hoisted before the first statement runs.
    const BuiltinInfo* resolved_builtin = nullptr;
    const FunctionDeclNode* resolved_user = nullptr;
    bool resolution_attempted = false;
};

class AssignmentNode : public ASTNode {
public:
    std::string identifier;
    std::unique_ptr<ExpressionNode> expression;
    // Set by the `global` keyword. Inside a function, a plain assignment writes a
    // local so a helper cannot silently clobber game state that happens to share a
    // name; `global score = ...` is how you opt in to writing the outer variable.
    bool force_global = false;
    AssignmentNode(const std::string& id, std::unique_ptr<ExpressionNode> expr, int line = 0, bool global = false)
        : ASTNode(NodeKind::Assignment, line), identifier(id), expression(std::move(expr)), force_global(global) {}
};

class ArrayAssignmentNode : public ASTNode {
public:
    std::string array_name;
    std::unique_ptr<ExpressionNode> index;
    std::unique_ptr<ExpressionNode> expression;
    ArrayAssignmentNode(const std::string& name, std::unique_ptr<ExpressionNode> idx,
                        std::unique_ptr<ExpressionNode> expr, int line = 0)
        : ASTNode(NodeKind::ArrayAssignment, line), array_name(name),
          index(std::move(idx)), expression(std::move(expr)) {}
};

// pos.y = 3;  - the target is a plain variable holding a vec3.
class MemberAssignmentNode : public ASTNode {
public:
    std::string identifier;
    int component;
    std::unique_ptr<ExpressionNode> expression;
    bool force_global = false;
    MemberAssignmentNode(const std::string& id, int comp, std::unique_ptr<ExpressionNode> expr,
                         int line = 0, bool global = false)
        : ASTNode(NodeKind::MemberAssignment, line), identifier(id), component(comp),
          expression(std::move(expr)), force_global(global) {}
};

class BlockNode : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> statements;
    BlockNode(std::vector<std::unique_ptr<ASTNode>> stmts, int line = 0)
        : ASTNode(NodeKind::Block, line), statements(std::move(stmts)) {}
};

class IfNode : public ASTNode {
public:
    std::unique_ptr<ExpressionNode> condition;
    std::unique_ptr<ASTNode> then_branch;
    std::unique_ptr<ASTNode> else_branch;
    IfNode(std::unique_ptr<ExpressionNode> cond, std::unique_ptr<ASTNode> then_br,
           std::unique_ptr<ASTNode> else_br, int line = 0)
        : ASTNode(NodeKind::If, line), condition(std::move(cond)),
          then_branch(std::move(then_br)), else_branch(std::move(else_br)) {}
};

class WhileNode : public ASTNode {
public:
    std::unique_ptr<ExpressionNode> condition;
    std::unique_ptr<ASTNode> body;
    WhileNode(std::unique_ptr<ExpressionNode> cond, std::unique_ptr<ASTNode> b, int line = 0)
        : ASTNode(NodeKind::While, line), condition(std::move(cond)), body(std::move(b)) {}
};

class ForNode : public ASTNode {
public:
    std::unique_ptr<ASTNode> init;      // may be null
    std::unique_ptr<ExpressionNode> condition; // may be null (means "true")
    std::unique_ptr<ASTNode> step;      // may be null
    std::unique_ptr<ASTNode> body;
    ForNode(std::unique_ptr<ASTNode> i, std::unique_ptr<ExpressionNode> c,
            std::unique_ptr<ASTNode> s, std::unique_ptr<ASTNode> b, int line = 0)
        : ASTNode(NodeKind::For, line), init(std::move(i)), condition(std::move(c)),
          step(std::move(s)), body(std::move(b)) {}
};

class ReturnNode : public ASTNode {
public:
    std::unique_ptr<ExpressionNode> expression; // may be null
    ReturnNode(std::unique_ptr<ExpressionNode> expr, int line = 0)
        : ASTNode(NodeKind::Return, line), expression(std::move(expr)) {}
};

class BreakNode : public ASTNode {
public:
    explicit BreakNode(int line = 0) : ASTNode(NodeKind::Break, line) {}
};

class ContinueNode : public ASTNode {
public:
    explicit ContinueNode(int line = 0) : ASTNode(NodeKind::Continue, line) {}
};

class FunctionDeclNode : public ASTNode {
public:
    std::string name;
    std::vector<std::string> parameters;
    std::unique_ptr<ASTNode> body;
    FunctionDeclNode(std::string n, std::vector<std::string> params, std::unique_ptr<ASTNode> b, int line = 0)
        : ASTNode(NodeKind::FunctionDecl, line), name(std::move(n)),
          parameters(std::move(params)), body(std::move(b)) {}
};

// `array grid[64];` - fixes the size, which turns an out-of-range index into a
// reported error instead of a silent grow. Undeclared arrays still grow on
// demand, because that is what every existing script relies on.
class ArrayDeclNode : public ASTNode {
public:
    std::string name;
    std::unique_ptr<ExpressionNode> size;
    ArrayDeclNode(std::string n, std::unique_ptr<ExpressionNode> s, int line = 0)
        : ASTNode(NodeKind::ArrayDecl, line), name(std::move(n)), size(std::move(s)) {}
};

// -----------------------------------------------------------------------------
//  Parser
// -----------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens, std::string script_name = {});
    std::vector<std::unique_ptr<ASTNode>> parse();

private:
    std::vector<Token> tokens;
    std::string script_name;
    size_t pos;

    const Token& current_token() const;
    const Token& peek_token(size_t offset = 1) const;
    void advance();
    Token expect(TokenType type, const char* what);
    [[noreturn]] void fail(const std::string& message) const;

    std::unique_ptr<ExpressionNode> parse_expression();
    std::unique_ptr<ExpressionNode> parse_logical_or();
    std::unique_ptr<ExpressionNode> parse_logical_and();
    std::unique_ptr<ExpressionNode> parse_equality();
    std::unique_ptr<ExpressionNode> parse_relational();
    std::unique_ptr<ExpressionNode> parse_additive();
    std::unique_ptr<ExpressionNode> parse_multiplicative();
    std::unique_ptr<ExpressionNode> parse_unary();
    std::unique_ptr<ExpressionNode> parse_postfix();
    std::unique_ptr<ExpressionNode> parse_factor();
    std::unique_ptr<ASTNode> parse_statement();
    std::unique_ptr<ASTNode> parse_simple_statement(bool consume_semicolon);
    std::unique_ptr<ASTNode> parse_function_declaration();
};

// -----------------------------------------------------------------------------
//  Builtin registry
//
//  One table describes every function a script can call: its arity, argument
//  names, return type, category and implementation. The interpreter dispatches
//  through it and the visual script editor builds its node palette from it, so
//  the palette cannot drift from what the language actually provides - which is
//  exactly what a hand-maintained second list does.
//
//  Overloads are separate entries with the same name and different arity.
// -----------------------------------------------------------------------------

class Interpreter;

using BuiltinFn = Value (*)(Interpreter&, const std::vector<Value>&, int line);

struct BuiltinInfo {
    const char* name;
    int arity;               // -1 means variadic
    const char* arg_names;   // comma-separated, for the editor's pin labels
    ValueType return_type;   // Void for a pure statement
    const char* category;    // groups the editor's Add-node menu
    const char* doc;
    bool needs_actor;        // reports an error rather than silently doing nothing
    BuiltinFn fn;
};

const std::vector<BuiltinInfo>& builtin_table();
// Exact arity first, then a variadic entry. Null when the name exists but no
// overload takes argc arguments - the caller reports the arity, not "unknown".
const BuiltinInfo* find_builtin(const std::string& name, int argc);
bool builtin_name_exists(const std::string& name);

// -----------------------------------------------------------------------------
//  Debugging
//
//  Thrown to unwind out of the middle of a script when a breakpoint is hit.
//  Deliberately NOT derived from std::exception: every script host catches
//  std::exception to latch a runtime error, and a breakpoint is not an error.
// -----------------------------------------------------------------------------

struct PauseSignal {
    int line = 0;
    long long statement_index = 0;
    int depth = 0;
};

// Per-execution debug configuration. Filled in by ScriptDebugger before a tick
// and read without a lock during it, so the interpreter never touches shared
// debugger state from the logic thread while the UI is reading it.
struct DebugState {
    bool active = false;
    std::vector<int> breakpoint_lines;  // sorted; empty when only stepping
    long long step_target = -1;         // pause once statement_index reaches this
    int step_max_depth = 0x7fffffff;    // step-over: ignore deeper statements
    long long statement_index = 0;      // reset at the start of every execute()
    int depth = 0;
};

// -----------------------------------------------------------------------------
//  Interpreter
// -----------------------------------------------------------------------------

class Interpreter {
public:
    explicit Interpreter(Actor* owner = nullptr);

    // Registers any function declarations (hoisted, so order does not matter) and
    // runs every other top-level statement.
    void execute(const std::vector<std::unique_ptr<ASTNode>>& program);

    // Registers function declarations without running anything. Needed by hosts
    // that only ever call event functions.
    void bind_program(const std::vector<std::unique_ptr<ASTNode>>& program);

    // Calls a script-defined function. Returns false if it is not defined, which
    // is not an error - a script with only on_tick is perfectly normal.
    bool call_script_function(const std::string& name, const std::vector<Value>& args, Value* out_result = nullptr);
    bool has_script_function(const std::string& name) const;

    void execute_statement(ASTNode* stmt);
    Value evaluate(ExpressionNode* node);

    // Variable access that respects the current call frame.
    Value get_variable(const std::string& name) const;
    void set_variable(const std::string& name, const Value& value);
    void set_global(const std::string& name, const Value& value);

    [[noreturn]] void error(int line, const std::string& message) const;
    float require_number(const Value& v, int line, const char* what) const;
    Vector3 require_vec3(const Value& v, int line, const char* what) const;

    // Array storage. Undeclared arrays grow to fit any index inside the cap;
    // `array a[n];` fixes the size and makes an out-of-range index an error.
    struct ArrayData {
        std::vector<Value> data;
        int declared_size = -1;
    };
    // Growing on READ, not only on write, is load-bearing: existing scripts read
    // an array before ever assigning to it and expect zero (the sample game's
    // beacon table is exactly this). Erroring there would break every such script,
    // so "out of range" means negative, non-finite, past the cap, or past a
    // declared size - never merely "past what has been written".
    static constexpr int kMaxArrayElements = 65536;
    Value& array_slot(const std::string& name, const Value& index_value, int line, bool for_write);

    std::unordered_map<std::string, Value> variables;
    std::unordered_map<std::string, ArrayData> arrays;
    std::unordered_map<std::string, const FunctionDeclNode*> functions;
    Actor* actor_owner;
    std::vector<Actor*> nearby_cache;

    // Name of the script, for error messages. Set by the host.
    std::string script_name;

    DebugState debug;

    // A loop that never terminates would hang the logic thread and take the whole
    // engine with it, so it is capped and reported rather than left to spin.
    static constexpr int kMaxLoopIterations = 100000;
    static constexpr int kMaxCallDepth = 128;

private:
    friend struct BuiltinImpl;

    std::vector<std::unordered_map<std::string, Value>> call_frames;
    int call_depth = 0;

    // Control-flow signalling. Flags rather than exceptions: `return` inside a
    // per-frame loop is common enough that the cost of throwing would show up.
    bool returning = false;
    bool breaking = false;
    bool continuing = false;
    Value return_value;

    const void* bound_program = nullptr;

    void execute_block(const std::vector<std::unique_ptr<ASTNode>>& statements);
    Value call_user_function(const FunctionDeclNode& fn, const std::vector<Value>& args, int line);
    void debug_step(const ASTNode* stmt);
    bool control_flow_pending() const { return returning || breaking || continuing; }
};

} // namespace CMinus
