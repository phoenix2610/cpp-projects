// A regex engine that cannot blow up: parse to a tree, compile to an NFA, simulate it.
//
//   g++ -std=c++23 -O2 regex.cpp -o regex && ./regex --demo
//   ./regex '^a(b|c)*d+$' abcbcddd
//
// Backtracking engines try one path at a time and unwind, which is why
// (a+)+$ against "aaaaaaaaaaaaaaaaaaaaX" takes exponential time in PCRE. A
// Thompson simulation walks *every* live state at once, one input character at a
// time: the state set is bounded by the pattern size, so matching is O(len(text) x
// len(pattern)) no matter what the pattern looks like. Supports: literals, ., \d
// \w \s, [a-z^], *, +, ?, |, grouping, {m,n}, ^ and $.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ------------------------------------------------------------------ parser

struct Ast {
    enum Kind { Char, Any, Class, Concat, Alternate, Star, Plus, Optional, Repeat, Begin, End } kind;
    char literal = 0;
    bool negated = false;
    std::vector<std::pair<char, char>> ranges;
    std::vector<std::unique_ptr<Ast>> children;
    int min_repeat = 0, max_repeat = 0;
};

class Parser {
public:
    explicit Parser(std::string_view pattern) : p_(pattern) {}

    std::unique_ptr<Ast> parse() {
        auto node = alternate();
        if (pos_ != p_.size()) throw std::runtime_error("unexpected ')' at " + std::to_string(pos_));
        return node;
    }

private:
    std::unique_ptr<Ast> alternate() {
        auto left = concat();
        if (peek() != '|') return left;
        auto node = std::make_unique<Ast>(Ast{Ast::Alternate});
        node->children.push_back(std::move(left));
        while (peek() == '|') { ++pos_; node->children.push_back(concat()); }
        return node;
    }

    std::unique_ptr<Ast> concat() {
        auto node = std::make_unique<Ast>(Ast{Ast::Concat});
        while (pos_ < p_.size() && peek() != '|' && peek() != ')') node->children.push_back(repeat());
        return node;
    }

    std::unique_ptr<Ast> repeat() {
        auto atom_node = atom();
        for (;;) {
            char c = peek();
            if (c == '*' || c == '+' || c == '?') {
                ++pos_;
                auto node = std::make_unique<Ast>(Ast{c == '*' ? Ast::Star : c == '+' ? Ast::Plus : Ast::Optional});
                node->children.push_back(std::move(atom_node));
                atom_node = std::move(node);
            } else if (c == '{') {
                std::size_t close = p_.find('}', pos_);
                if (close == std::string_view::npos) throw std::runtime_error("unterminated {");
                std::string spec(p_.substr(pos_ + 1, close - pos_ - 1));
                pos_ = close + 1;
                auto comma = spec.find(',');
                auto node = std::make_unique<Ast>(Ast{Ast::Repeat});
                node->min_repeat = std::stoi(comma == std::string::npos ? spec : spec.substr(0, comma));
                node->max_repeat = comma == std::string::npos ? node->min_repeat
                                  : (comma + 1 == spec.size() ? -1 : std::stoi(spec.substr(comma + 1)));
                node->children.push_back(std::move(atom_node));
                atom_node = std::move(node);
            } else break;
        }
        return atom_node;
    }

    std::unique_ptr<Ast> atom() {
        char c = next();
        if (c == '(') {
            auto node = alternate();
            if (next() != ')') throw std::runtime_error("missing ')'");
            return node;
        }
        if (c == '[') return char_class();
        if (c == '.') return std::make_unique<Ast>(Ast{Ast::Any});
        if (c == '^') return std::make_unique<Ast>(Ast{Ast::Begin});
        if (c == '$') return std::make_unique<Ast>(Ast{Ast::End});
        if (c == '\\') return escape(next());
        auto node = std::make_unique<Ast>(Ast{Ast::Char});
        node->literal = c;
        return node;
    }

    std::unique_ptr<Ast> escape(char c) {
        auto node = std::make_unique<Ast>(Ast{Ast::Class});
        switch (c) {
            case 'd': node->ranges = {{'0', '9'}}; return node;
            case 'D': node->ranges = {{'0', '9'}}; node->negated = true; return node;
            case 'w': node->ranges = {{'a', 'z'}, {'A', 'Z'}, {'0', '9'}, {'_', '_'}}; return node;
            case 'W': node->ranges = {{'a', 'z'}, {'A', 'Z'}, {'0', '9'}, {'_', '_'}}; node->negated = true; return node;
            case 's': node->ranges = {{' ', ' '}, {'\t', '\t'}, {'\n', '\n'}, {'\r', '\r'}}; return node;
            default: {
                auto literal = std::make_unique<Ast>(Ast{Ast::Char});
                literal->literal = c;
                return literal;
            }
        }
    }

    std::unique_ptr<Ast> char_class() {
        auto node = std::make_unique<Ast>(Ast{Ast::Class});
        if (peek() == '^') { node->negated = true; ++pos_; }
        while (pos_ < p_.size() && peek() != ']') {
            char lo = next();
            if (lo == '\\') { auto sub = escape(next()); for (auto r : sub->ranges) node->ranges.push_back(r); continue; }
            if (peek() == '-' && pos_ + 1 < p_.size() && p_[pos_ + 1] != ']') {
                ++pos_;
                node->ranges.push_back({lo, next()});
            } else node->ranges.push_back({lo, lo});
        }
        if (next() != ']') throw std::runtime_error("missing ']'");
        return node;
    }

    char peek() const { return pos_ < p_.size() ? p_[pos_] : '\0'; }
    char next() {
        if (pos_ >= p_.size()) throw std::runtime_error("unexpected end of pattern");
        return p_[pos_++];
    }

    std::string_view p_;
    std::size_t pos_ = 0;
};

// ------------------------------------------------------------------- NFA

struct State {
    enum Op { Char, Any, Class, Split, Match, AssertBegin, AssertEnd } op;
    char literal = 0;
    bool negated = false;
    std::vector<std::pair<char, char>> ranges;
    int next = -1, alt = -1;
};

class Program {
public:
    explicit Program(const Ast& root) {
        int start = emit(root, add({State::Match}));
        start_ = start;
    }

    // anchored_end=true: the Match state must be live once the whole input is consumed.
    // anchored_end=false: accept as soon as it is reachable (prefix match, used by search).
    bool matches(std::string_view text, int* steps = nullptr, bool anchored_end = true) const {
        std::vector<int> current, next_set;
        std::vector<int> seen(states_.size(), -1);
        int generation = 0, work = 0;

        add_state(current, seen, generation, start_, 0, text.size());
        for (std::size_t i = 0; i <= text.size(); ++i) {
            ++work;
            if (!anchored_end || i == text.size())
                for (int id : current)
                    if (states_[id].op == State::Match) { if (steps) *steps = work; return true; }
            if (i == text.size()) break;
            next_set.clear();
            ++generation;
            for (int id : current) {
                const State& s = states_[id];
                bool consume = false;
                switch (s.op) {
                    case State::Char: consume = (text[i] == s.literal); break;
                    case State::Any: consume = true; break;
                    case State::Class: consume = in_class(s, text[i]); break;
                    default: break;
                }
                if (consume) add_state(next_set, seen, generation, s.next, i + 1, text.size());
            }
            current.swap(next_set);
            if (current.empty()) break;
        }
        for (int id : current) if (states_[id].op == State::Match) { if (steps) *steps = work; return true; }
        if (steps) *steps = work;
        return false;
    }

    std::size_t size() const { return states_.size(); }

private:
    static bool in_class(const State& s, char c) {
        bool hit = false;
        for (auto [lo, hi] : s.ranges) if (c >= lo && c <= hi) { hit = true; break; }
        return s.negated ? !hit : hit;
    }

    void add_state(std::vector<int>& out, std::vector<int>& seen, int generation,
                   int id, std::size_t position, std::size_t length) const {
        if (id < 0 || seen[id] == generation) return;      // the set, not the path: no exponential blowup
        seen[id] = generation;
        const State& s = states_[id];
        if (s.op == State::Split) {
            add_state(out, seen, generation, s.next, position, length);
            add_state(out, seen, generation, s.alt, position, length);
            return;
        }
        if (s.op == State::AssertBegin) {
            if (position == 0) add_state(out, seen, generation, s.next, position, length);
            return;
        }
        if (s.op == State::AssertEnd) {
            if (position == length) add_state(out, seen, generation, s.next, position, length);
            return;
        }
        out.push_back(id);
    }

    int add(State state) { states_.push_back(state); return int(states_.size()) - 1; }

    // emit(node, next) returns the entry state id for `node` continuing to `next`
    int emit(const Ast& node, int next) {
        switch (node.kind) {
            case Ast::Char: { State s{State::Char}; s.literal = node.literal; s.next = next; return add(s); }
            case Ast::Any: { State s{State::Any}; s.next = next; return add(s); }
            case Ast::Class: { State s{State::Class}; s.ranges = node.ranges; s.negated = node.negated; s.next = next; return add(s); }
            case Ast::Begin: { State s{State::AssertBegin}; s.next = next; return add(s); }
            case Ast::End: { State s{State::AssertEnd}; s.next = next; return add(s); }
            case Ast::Concat: {
                int entry = next;
                for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
                    entry = emit(**it, entry);
                return entry;
            }
            case Ast::Alternate: {
                int entry = emit(*node.children.back(), next);
                for (int i = int(node.children.size()) - 2; i >= 0; --i) {
                    State split{State::Split};
                    split.next = emit(*node.children[i], next);
                    split.alt = entry;
                    entry = add(split);
                }
                return entry;
            }
            case Ast::Star: {
                int split_id = add({State::Split});
                int body = emit(*node.children[0], split_id);
                states_[split_id].next = body;
                states_[split_id].alt = next;
                return split_id;
            }
            case Ast::Plus: {
                int split_id = add({State::Split});
                int body = emit(*node.children[0], split_id);
                states_[split_id].next = body;
                states_[split_id].alt = next;
                return body;
            }
            case Ast::Optional: {
                State split{State::Split};
                split.next = emit(*node.children[0], next);
                split.alt = next;
                return add(split);
            }
            case Ast::Repeat: {
                int entry = next;
                if (node.max_repeat < 0) {                      // {m,} = m copies then a star
                    int split_id = add({State::Split});
                    int body = emit(*node.children[0], split_id);
                    states_[split_id].next = body;
                    states_[split_id].alt = next;
                    entry = split_id;
                } else {
                    for (int i = node.min_repeat; i < node.max_repeat; ++i) {
                        State split{State::Split};
                        split.next = emit(*node.children[0], entry);
                        split.alt = next;
                        entry = add(split);
                    }
                }
                for (int i = 0; i < node.min_repeat; ++i) entry = emit(*node.children[0], entry);
                return entry;
            }
        }
        return next;
    }

    std::vector<State> states_;
    int start_ = 0;
};

class Regex {
public:
    explicit Regex(std::string_view pattern)
        : ast_(Parser(pattern).parse()), program_(*ast_), source_(pattern) {}

    bool full_match(std::string_view text, int* steps = nullptr) const {
        return program_.matches(text, steps, true);
    }
    bool search(std::string_view text) const {
        for (std::size_t i = 0; i <= text.size(); ++i)
            if (program_.matches(text.substr(i), nullptr, false)) return true;
        return false;
    }
    std::size_t states() const { return program_.size(); }

private:
    std::unique_ptr<Ast> ast_;
    Program program_;
    std::string source_;
};

// What a backtracking engine does for /(a+)+X/: try every way of splitting the run
// of a's into one-or-more groups of one-or-more a's. That is 2^(n-1) partitions, and
// when the string ends in something that cannot match, it explores all of them.
static bool backtrack_plus_plus(const char* text, long& steps, long budget) {
    if (++steps > budget) return false;
    if (*text == '\0') return true;      // the whole string was consumed by groups of a's
    for (int taken = 1; text[taken - 1] == 'a'; ++taken)
        if (backtrack_plus_plus(text + taken, steps, budget)) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc >= 3) {
        Regex re(argv[1]);
        int steps = 0;
        bool ok = re.full_match(argv[2], &steps);
        std::printf("  pattern %s against %s: %s (%zu states, %d steps)\n",
                    argv[1], argv[2], ok ? "match" : "no match", re.states(), steps);
        return ok ? 0 : 1;
    }

    struct Case { const char* pattern; const char* text; bool expected; };
    const Case cases[] = {
        {"abc", "abc", true}, {"abc", "abd", false},
        {"a*", "", true}, {"a*", "aaaa", true}, {"a+", "", false},
        {"a?b", "b", true}, {"a?b", "ab", true}, {"a?b", "aab", false},
        {"(ab)+", "ababab", true}, {"(ab)+", "aba", false},
        {"a|b|c", "b", true}, {"a|b|c", "d", false},
        {"^hello$", "hello", true}, {"^hello$", "hello there", false},
        {"h.llo", "hello", true}, {"h.llo", "hllo", false},
        {"[a-z]+", "abc", true}, {"[a-z]+", "aBc", false},
        {"[^0-9]+", "abc", true}, {"[^0-9]+", "ab3", false},
        {R"(\d{3}-\d{4})", "555-1234", true}, {R"(\d{3}-\d{4})", "55-1234", false},
        {R"(\w+@\w+\.[a-z]{2,3})", "ana@example.com", true},
        {R"(\w+@\w+\.[a-z]{2,3})", "not-an-email", false},
        {"a{2,4}", "aaa", true}, {"a{2,4}", "a", false}, {"a{2,4}", "aaaaa", false},
        {"(a|b)*c", "ababbac", true}, {"(a|b)*c", "ababbad", false},
        {"colou?r", "color", true}, {"colou?r", "colour", true},
    };

    int passed = 0;
    for (const Case& c : cases) {
        Regex re(c.pattern);
        bool got = re.full_match(c.text);
        if (got == c.expected) ++passed;
        else std::printf("  FAIL  /%s/ vs %-16s expected %d got %d\n", c.pattern, c.text, c.expected, got);
    }
    std::printf("1. %d/%zu cases pass\n", passed, sizeof(cases) / sizeof(cases[0]));

    std::puts("\n2. the pathological pattern: (a+)+$ against a string that cannot match");
    std::puts("   length   NFA steps   NFA time    backtracking steps   backtracking time");
    for (int n : {10, 16, 20, 24, 26, 28, 30}) {
        std::string text(n, 'a');
        text += 'X';
        Regex re("(a+)+$");
        int steps = 0;
        auto start = std::chrono::steady_clock::now();
        re.full_match(text, &steps);
        double nfa_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        long bt_steps = 0;
        constexpr long kBudget = 200'000'000;
        start = std::chrono::steady_clock::now();
        backtrack_plus_plus(text.c_str(), bt_steps, kBudget);
        double bt_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("   %5d   %9d   %7.3fms   %18ld   %10.1fms%s\n", n, steps, nfa_ms, bt_steps, bt_ms,
                    bt_steps >= kBudget ? "  (gave up)" : "");
    }
    std::puts("   the NFA's step count is the input length — it has no path to explode along");

    std::puts("\n3. compiled program size");
    for (const char* pattern : {"abc", "(a|b)*c", R"(\d{3}-\d{4})", "a{2,10}", R"(\w+@\w+\.[a-z]{2,3})"}) {
        Regex re(pattern);
        std::printf("   %-24s %zu states\n", pattern, re.states());
    }

    std::puts("\n4. search (unanchored) over a log line");
    {
        Regex re(R"(\d{3}-\d{4})");
        std::string line = "call 555-1234 to confirm";
        std::printf("   /\\d{3}-\\d{4}/ found in \"%s\": %s\n", line.c_str(), re.search(line) ? "yes" : "no");
    }
    return 0;
}
