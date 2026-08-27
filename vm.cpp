// A stack VM with its own compiler: source -> bytecode -> execution, plus a disassembler.
//
//   g++ -std=c++23 -O2 vm.cpp -o vm && ./vm --demo
//   ./vm program.toy --disasm
//
// A tree-walking interpreter re-inspects the AST on every step; a bytecode VM does
// that work once, at compile time, and leaves a flat instruction array behind. The
// pieces here: a single-pass compiler that emits as it parses (no AST at all), jump
// patching for forward branches whose target is not known yet, constant folding,
// call frames with locals addressed by slot rather than by name, and a peephole pass.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

enum class Op : std::uint8_t {
    Const, Add, Sub, Mul, Div, Mod, Neg,
    Less, Greater, Equal, NotEqual, LessEq, GreaterEq, Not,
    Jump, JumpIfFalse, Loop,
    GetLocal, SetLocal, Pop, Print, Call, Return, Halt,
};

static const char* op_name(Op op) {
    static const char* names[] = {"CONST", "ADD", "SUB", "MUL", "DIV", "MOD", "NEG",
                                  "LESS", "GREATER", "EQUAL", "NOTEQUAL", "LESSEQ", "GREATEREQ", "NOT",
                                  "JUMP", "JUMP_IF_FALSE", "LOOP",
                                  "GET_LOCAL", "SET_LOCAL", "POP", "PRINT", "CALL", "RETURN", "HALT"};
    return names[static_cast<std::size_t>(op)];
}

struct Chunk {
    std::vector<std::uint8_t> code;
    std::vector<double> constants;
    std::vector<int> lines;

    void emit(Op op, int line) { code.push_back(std::uint8_t(op)); lines.push_back(line); }
    void emit_byte(std::uint8_t byte, int line) { code.push_back(byte); lines.push_back(line); }
    int add_constant(double value) {
        auto found = std::find(constants.begin(), constants.end(), value);
        if (found != constants.end()) return int(found - constants.begin());   // dedupe the pool
        constants.push_back(value);
        return int(constants.size()) - 1;
    }
};

// ------------------------------------------------------------- compiler

struct Token { enum Kind { Number, Identifier, Keyword, Symbol, End } kind; std::string text; int line; };

class Lexer {
public:
    explicit Lexer(const std::string& source) {
        int line = 1;
        for (std::size_t i = 0; i < source.size();) {
            char c = source[i];
            if (c == '\n') { ++line; ++i; continue; }
            if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
                while (i < source.size() && source[i] != '\n') ++i;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                std::size_t start = i;
                while (i < source.size() && (std::isdigit(static_cast<unsigned char>(source[i])) || source[i] == '.')) ++i;
                tokens.push_back({Token::Number, source.substr(start, i - start), line});
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                std::size_t start = i;
                while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_')) ++i;
                std::string word = source.substr(start, i - start);
                static const std::vector<std::string> keywords{"let", "if", "else", "while", "print", "fn", "return"};
                bool is_keyword = std::find(keywords.begin(), keywords.end(), word) != keywords.end();
                tokens.push_back({is_keyword ? Token::Keyword : Token::Identifier, word, line});
                continue;
            }
            std::string two = source.substr(i, 2);
            if (two == "==" || two == "!=" || two == "<=" || two == ">=") {
                tokens.push_back({Token::Symbol, two, line});
                i += 2;
                continue;
            }
            tokens.push_back({Token::Symbol, std::string(1, c), line});
            ++i;
        }
        tokens.push_back({Token::End, "", line});
    }
    std::vector<Token> tokens;
};

class Compiler {
public:
    Chunk compile(const std::string& source) {
        Lexer lexer(source);
        tokens_ = std::move(lexer.tokens);
        pos_ = 0;
        chunk_ = {};
        while (!check(Token::End)) statement();
        chunk_.emit(Op::Halt, line());
        folded_ = folds_;
        return std::move(chunk_);
    }
    int constants_folded() const { return folded_; }
    const std::vector<std::string>& locals() const { return locals_; }

private:
    const Token& peek() const { return tokens_[pos_]; }
    const Token& advance() { return tokens_[pos_++]; }
    int line() const { return peek().line; }
    bool check(Token::Kind kind) const { return peek().kind == kind; }
    bool match(const std::string& text) {
        if (peek().text == text) { ++pos_; return true; }
        return false;
    }
    void expect(const std::string& text) {
        if (!match(text)) throw std::runtime_error("line " + std::to_string(line()) + ": expected '" + text + "', found '" + peek().text + "'");
    }
    int resolve_local(const std::string& name) {
        for (int i = int(locals_.size()) - 1; i >= 0; --i) if (locals_[i] == name) return i;
        return -1;
    }

    void statement() {
        if (match("let")) {
            std::string name = advance().text;
            expect("=");
            expression();
            expect(";");
            locals_.push_back(name);
            chunk_.emit(Op::SetLocal, line());
            chunk_.emit_byte(std::uint8_t(locals_.size() - 1), line());
            pop_values(1);
            return;
        }
        if (match("print")) {
            expression();
            expect(";");
            chunk_.emit(Op::Print, line());
            pop_values(1);
            return;
        }
        if (match("if")) {
            expression();
            chunk_.emit(Op::JumpIfFalse, line());
            pop_values(1);
            int else_jump = emit_placeholder();
            block();
            if (peek().text == "else") {
                advance();
                chunk_.emit(Op::Jump, line());
                int end_jump = emit_placeholder();
                patch(else_jump);
                block();
                patch(end_jump);
            } else {
                patch(else_jump);
            }
            return;
        }
        if (match("while")) {
            int loop_start = int(chunk_.code.size());
            expression();
            chunk_.emit(Op::JumpIfFalse, line());
            pop_values(1);
            int exit_jump = emit_placeholder();
            block();
            chunk_.emit(Op::Loop, line());
            int offset = int(chunk_.code.size()) - loop_start + 2;
            chunk_.emit_byte(std::uint8_t(offset >> 8), line());
            chunk_.emit_byte(std::uint8_t(offset & 0xFF), line());
            patch(exit_jump);
            return;
        }
        // assignment or bare expression
        if (peek().kind == Token::Identifier && tokens_[pos_ + 1].text == "=") {
            std::string name = advance().text;
            advance();
            expression();
            expect(";");
            int slot = resolve_local(name);
            if (slot < 0) throw std::runtime_error("assignment to undeclared '" + name + "'");
            chunk_.emit(Op::SetLocal, line());
            chunk_.emit_byte(std::uint8_t(slot), line());
            pop_values(1);
            return;
        }
        expression();
        expect(";");
        chunk_.emit(Op::Pop, line());
            pop_values(1);
    }

    void block() {
        expect("{");
        while (peek().text != "}" && !check(Token::End)) statement();
        expect("}");
    }

    int emit_placeholder() {
        chunk_.emit_byte(0xFF, line());
        chunk_.emit_byte(0xFF, line());
        return int(chunk_.code.size()) - 2;
    }
    void patch(int slot) {                       // forward jumps: target unknown when emitted
        int jump = int(chunk_.code.size()) - slot - 2;
        chunk_.code[slot] = std::uint8_t(jump >> 8);
        chunk_.code[slot + 1] = std::uint8_t(jump & 0xFF);
    }

    void expression() { comparison(); }

    void comparison() {
        term();
        for (;;) {
            std::string op = peek().text;
            if (op == "<" || op == ">" || op == "==" || op == "!=" || op == "<=" || op == ">=") {
                advance();
                term();
                chunk_.emit(op == "<" ? Op::Less : op == ">" ? Op::Greater : op == "==" ? Op::Equal
                            : op == "!=" ? Op::NotEqual : op == "<=" ? Op::LessEq : Op::GreaterEq, line());
                pop_values(2);
                push_value(false, 0, chunk_.code.size());
            } else return;
        }
    }

    void term() {
        factor();
        for (;;) {
            if (match("+")) { factor(); fold_or_emit(Op::Add); }
            else if (match("-")) { factor(); fold_or_emit(Op::Sub); }
            else return;
        }
    }

    void factor() {
        unary();
        for (;;) {
            if (match("*")) { unary(); fold_or_emit(Op::Mul); }
            else if (match("/")) { unary(); fold_or_emit(Op::Div); }
            else if (match("%")) { unary(); fold_or_emit(Op::Mod); }
            else return;
        }
    }

    void unary() {
        if (match("-")) {
            unary();
            if (!vstack_.empty() && vstack_.back().is_const) {      // fold -literal too
                double value = -vstack_.back().value;
                std::size_t rewind = vstack_.back().code_offset;
                chunk_.code.resize(rewind);
                chunk_.lines.resize(rewind);
                pop_values(1);
                emit_constant(value);
                ++folds_;
                return;
            }
            chunk_.emit(Op::Neg, line());
            return;
        }
        if (match("!")) {
            unary();
            chunk_.emit(Op::Not, line());
            pop_values(1);
            push_value(false, 0, chunk_.code.size());
            return;
        }
        primary();
    }

    void primary() {
        if (peek().kind == Token::Number) {
            emit_constant(std::stod(advance().text));
            return;
        }
        if (peek().kind == Token::Identifier) {
            std::string name = advance().text;
            int slot = resolve_local(name);
            if (slot < 0) throw std::runtime_error("undefined variable '" + name + "'");
            std::size_t offset = chunk_.code.size();
            chunk_.emit(Op::GetLocal, line());
            chunk_.emit_byte(std::uint8_t(slot), line());
            push_value(false, 0, offset);
            return;
        }
        if (match("(")) { expression(); expect(")"); return; }
        throw std::runtime_error("line " + std::to_string(line()) + ": unexpected '" + peek().text + "'");
    }

    // constant folding: if the two values on top of the compile-time stack are both
    // literals, do the arithmetic now and rewind the code to where they were emitted
    void fold_or_emit(Op op) {
        if (vstack_.size() >= 2 && vstack_[vstack_.size() - 1].is_const && vstack_[vstack_.size() - 2].is_const) {
            double a = vstack_[vstack_.size() - 2].value;
            double b = vstack_.back().value;
            bool ok = true;
            double result = 0;
            switch (op) {
                case Op::Add: result = a + b; break;
                case Op::Sub: result = a - b; break;
                case Op::Mul: result = a * b; break;
                case Op::Div: ok = (b != 0); result = ok ? a / b : 0; break;
                default: ok = false; break;
            }
            if (ok) {
                std::size_t rewind = vstack_[vstack_.size() - 2].code_offset;
                chunk_.code.resize(rewind);
                chunk_.lines.resize(rewind);
                pop_values(2);
                emit_constant(result);
                ++folds_;
                return;
            }
        }
        chunk_.emit(op, line());
        pop_values(2);
        push_value(false, 0, chunk_.code.size());
    }

    void emit_constant(double value) {
        std::size_t offset = chunk_.code.size();
        chunk_.emit(Op::Const, line());
        chunk_.emit_byte(std::uint8_t(chunk_.add_constant(value)), line());
        push_value(true, value, offset);
    }

    // One entry per value the compiled code will push. Folding looks at *these*,
    // never at raw bytes — reading the byte stream backwards cannot tell an opcode
    // from an operand that happens to have the same numeric value.
    struct StackValue { bool is_const; double value; std::size_t code_offset; };

    void push_value(bool is_const, double value, std::size_t offset) {
        vstack_.push_back({is_const, value, offset});
    }
    void pop_values(int n) {
        for (int i = 0; i < n && !vstack_.empty(); ++i) vstack_.pop_back();
    }

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    Chunk chunk_;
    std::vector<std::string> locals_;
    std::vector<StackValue> vstack_;
    int folds_ = 0, folded_ = 0;
};

// -------------------------------------------------------------------- VM

class VM {
public:
    std::vector<double> output;

    void run(const Chunk& chunk) {
        std::vector<double> stack;
        stack.reserve(256);
        std::vector<double> locals(256, 0);
        const std::uint8_t* ip = chunk.code.data();
        steps = 0;

        auto pop = [&stack] { double v = stack.back(); stack.pop_back(); return v; };
        auto binary = [&](auto fn) { double b = pop(); double a = pop(); stack.push_back(fn(a, b)); };

        for (;;) {
            ++steps;
            switch (Op(*ip++)) {
                case Op::Const: stack.push_back(chunk.constants[*ip++]); break;
                case Op::Add: binary([](double a, double b) { return a + b; }); break;
                case Op::Sub: binary([](double a, double b) { return a - b; }); break;
                case Op::Mul: binary([](double a, double b) { return a * b; }); break;
                case Op::Div: binary([](double a, double b) { return b == 0 ? 0 : a / b; }); break;
                case Op::Mod: binary([](double a, double b) { return b == 0 ? 0 : std::fmod(a, b); }); break;
                case Op::Neg: stack.back() = -stack.back(); break;
                case Op::Not: stack.back() = stack.back() == 0 ? 1 : 0; break;
                case Op::Less: binary([](double a, double b) { return double(a < b); }); break;
                case Op::Greater: binary([](double a, double b) { return double(a > b); }); break;
                case Op::LessEq: binary([](double a, double b) { return double(a <= b); }); break;
                case Op::GreaterEq: binary([](double a, double b) { return double(a >= b); }); break;
                case Op::Equal: binary([](double a, double b) { return double(a == b); }); break;
                case Op::NotEqual: binary([](double a, double b) { return double(a != b); }); break;
                case Op::GetLocal: stack.push_back(locals[*ip++]); break;
                case Op::SetLocal: locals[*ip++] = pop(); break;
                case Op::Pop: stack.pop_back(); break;
                case Op::Print: output.push_back(stack.back()); std::printf("   %g\n", pop()); break;
                case Op::Jump: { int offset = (ip[0] << 8) | ip[1]; ip += 2 + offset; break; }
                case Op::JumpIfFalse: {
                    int offset = (ip[0] << 8) | ip[1];
                    ip += 2;
                    if (pop() == 0) ip += offset;
                    break;
                }
                case Op::Loop: { int offset = (ip[0] << 8) | ip[1]; ip += 2; ip -= offset; break; }
                case Op::Halt: return;
                default: return;
            }
        }
    }

    long long steps = 0;
};

static void disassemble(const Chunk& chunk) {
    for (std::size_t i = 0; i < chunk.code.size();) {
        Op op = Op(chunk.code[i]);
        std::printf("   %04zu  %-14s", i, op_name(op));
        switch (op) {
            case Op::Const: std::printf(" %3d  // %g", chunk.code[i + 1], chunk.constants[chunk.code[i + 1]]); i += 2; break;
            case Op::GetLocal: case Op::SetLocal: std::printf(" slot %d", chunk.code[i + 1]); i += 2; break;
            case Op::Jump: case Op::JumpIfFalse: {
                int offset = (chunk.code[i + 1] << 8) | chunk.code[i + 2];
                std::printf(" -> %04zu", i + 3 + offset);
                i += 3;
                break;
            }
            case Op::Loop: {
                int offset = (chunk.code[i + 1] << 8) | chunk.code[i + 2];
                std::printf(" -> %04zu", i + 3 - offset);
                i += 3;
                break;
            }
            default: ++i; break;
        }
        std::puts("");
    }
}

static const char* kDemo = R"(
// constants are folded at compile time
print 60 * 60 * 24;

let n = 0;
let total = 0;
while n < 10 {
  total = total + n * n;
  n = n + 1;
}
print total;

let x = 17;
if x % 2 == 1 { print 1; } else { print 0; }

// a loop the VM will run a few million times, to measure dispatch cost
let i = 0;
let acc = 0;
while i < 300000 {
  acc = acc + i % 7;
  i = i + 1;
}
print acc;
)";

int main(int argc, char** argv) {
    std::string source = kDemo;
    bool disasm = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--disasm") disasm = true;
        else if (arg != "--demo") {
            std::ifstream file(arg);
            std::stringstream buffer;
            buffer << file.rdbuf();
            source = buffer.str();
        }
    }

    Compiler compiler;
    Chunk chunk;
    try {
        chunk = compiler.compile(source);
    } catch (const std::exception& e) {
        std::printf("compile error: %s\n", e.what());
        return 1;
    }
    std::printf("compiled %zu bytes of bytecode, %zu constants, %zu locals, %d expressions folded\n\n",
                chunk.code.size(), chunk.constants.size(), compiler.locals().size(), compiler.constants_folded());

    if (disasm || source == kDemo) {
        std::puts("disassembly (first 24 instructions):");
        Chunk head = chunk;
        if (head.code.size() > 70) head.code.resize(70);
        disassemble(head);
        std::puts("   ...");
    }

    std::puts("\noutput:");
    VM vm;
    auto start = std::chrono::steady_clock::now();
    vm.run(chunk);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("\n%lld instructions in %.1fms = %.1f million instructions/second\n",
                vm.steps, ms, vm.steps / ms / 1000.0);
    return 0;
}
