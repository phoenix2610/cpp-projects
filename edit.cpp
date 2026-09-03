// A modal text editor: raw mode, a gap buffer, vi-style keys, incremental search.
//
//   g++ -std=c++23 -O2 edit.cpp -o edit && ./edit file.txt
//   ./edit --script "ihello world\x1b0wdwA!\x1b" --show    # drive it without a terminal
//   ./edit --demo
//
// Two things make an editor an editor. Raw mode: turn off canonical input and echo
// so you receive keystrokes rather than lines, and put the terminal back exactly as
// you found it even if you crash. And an edit-friendly buffer: a gap buffer keeps a
// hole at the cursor so typing is O(1) instead of memmoving the file on every
// keystroke — the cost moves to cursor jumps, which are rare and cheap.

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// ------------------------------------------------------------- gap buffer

class GapBuffer {
public:
    explicit GapBuffer(const std::string& text = "", std::size_t gap = 128) {
        buffer_.assign(text.begin(), text.end());
        buffer_.insert(buffer_.begin(), gap, '\0');
        gap_start_ = 0;
        gap_end_ = gap;
    }

    std::size_t size() const { return buffer_.size() - (gap_end_ - gap_start_); }
    std::size_t cursor() const { return gap_start_; }

    char at(std::size_t index) const {
        return buffer_[index < gap_start_ ? index : index + (gap_end_ - gap_start_)];
    }

    std::string str() const {
        std::string out;
        out.reserve(size());
        out.append(buffer_.begin(), buffer_.begin() + gap_start_);
        out.append(buffer_.begin() + gap_end_, buffer_.end());
        return out;
    }

    void move_to(std::size_t position) {
        position = std::min(position, size());
        while (gap_start_ > position) buffer_[--gap_end_] = buffer_[--gap_start_];   // shuffle the gap left
        while (gap_start_ < position) buffer_[gap_start_++] = buffer_[gap_end_++];   // ...or right
        ++moves_;
    }

    void insert(char c) {
        if (gap_start_ == gap_end_) grow();
        buffer_[gap_start_++] = c;
        ++inserts_;
    }
    void insert(const std::string& text) { for (char c : text) insert(c); }

    bool erase_back() {
        if (gap_start_ == 0) return false;
        --gap_start_;
        ++deletes_;
        return true;
    }
    bool erase_forward() {
        if (gap_end_ >= buffer_.size()) return false;
        ++gap_end_;
        ++deletes_;
        return true;
    }

    std::size_t line_start(std::size_t position) const {
        while (position > 0 && at(position - 1) != '\n') --position;
        return position;
    }
    std::size_t line_end(std::size_t position) const {
        while (position < size() && at(position) != '\n') ++position;
        return position;
    }
    std::size_t line_number(std::size_t position) const {
        std::size_t line = 1;
        for (std::size_t i = 0; i < position && i < size(); ++i) if (at(i) == '\n') ++line;
        return line;
    }
    std::size_t lines() const {
        std::size_t count = 1;
        for (std::size_t i = 0; i < size(); ++i) if (at(i) == '\n') ++count;
        return count;
    }

    std::size_t find(const std::string& needle, std::size_t from) const {
        if (needle.empty() || needle.size() > size()) return std::string::npos;
        for (std::size_t start = from; start + needle.size() <= size(); ++start) {
            std::size_t i = 0;
            while (i < needle.size() && at(start + i) == needle[i]) ++i;
            if (i == needle.size()) return start;
        }
        return std::string::npos;
    }

    struct Stats { std::uint64_t inserts, deletes, moves; std::size_t capacity, gap; };
    Stats stats() const { return {inserts_, deletes_, moves_, buffer_.size(), gap_end_ - gap_start_}; }

private:
    void grow() {
        std::size_t extra = std::max<std::size_t>(128, buffer_.size() / 2);
        buffer_.insert(buffer_.begin() + std::ptrdiff_t(gap_start_), extra, '\0');
        gap_end_ += extra;
    }

    std::vector<char> buffer_;
    std::size_t gap_start_ = 0, gap_end_ = 0;
    std::uint64_t inserts_ = 0, deletes_ = 0, moves_ = 0;
};

// ------------------------------------------------------------------ editor

class Editor {
public:
    enum class Mode { Normal, Insert, Search };

    explicit Editor(const std::string& path = "") : path_(path) {
        std::string text;
        if (!path.empty()) {
            std::ifstream file(path);
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        buffer_ = GapBuffer(text);
    }

    void feed(char key) {
        if (pending_) { char op = key; pending_ = 0; feed_pending(op); return; }
        if (mode_ == Mode::Insert) {
            if (key == 27) { mode_ = Mode::Normal; if (buffer_.cursor()) buffer_.move_to(buffer_.cursor() - 1); return; }
            if (key == 127 || key == 8) { buffer_.erase_back(); return; }
            buffer_.insert(key);
            return;
        }
        if (mode_ == Mode::Search) {
            if (key == '\r' || key == '\n') {
                std::size_t hit = buffer_.find(query_, buffer_.cursor() + 1);
                if (hit == std::string::npos) hit = buffer_.find(query_, 0);      // wrap around
                if (hit != std::string::npos) buffer_.move_to(hit);
                else status_ = "not found: " + query_;
                mode_ = Mode::Normal;
                return;
            }
            if (key == 27) { mode_ = Mode::Normal; query_.clear(); return; }
            if (key == 127 && !query_.empty()) { query_.pop_back(); return; }
            query_ += key;
            return;
        }

        std::size_t position = buffer_.cursor();
        switch (key) {
            case 'i': mode_ = Mode::Insert; break;
            case 'a': if (position < buffer_.size()) buffer_.move_to(position + 1); mode_ = Mode::Insert; break;
            case 'A': buffer_.move_to(buffer_.line_end(position)); mode_ = Mode::Insert; break;
            case 'I': buffer_.move_to(buffer_.line_start(position)); mode_ = Mode::Insert; break;
            case 'o': buffer_.move_to(buffer_.line_end(position)); buffer_.insert('\n'); mode_ = Mode::Insert; break;
            case 'h': if (position) buffer_.move_to(position - 1); break;
            case 'l': if (position < buffer_.size()) buffer_.move_to(position + 1); break;
            case 'j': move_line(1); break;
            case 'k': move_line(-1); break;
            case '0': buffer_.move_to(buffer_.line_start(position)); break;
            case '$': buffer_.move_to(buffer_.line_end(position)); break;
            case 'g': buffer_.move_to(0); break;
            case 'G': buffer_.move_to(buffer_.size()); break;
            case 'w': move_word(1); break;
            case 'b': move_word(-1); break;
            case 'x': buffer_.erase_forward(); break;
            case 'D': erase_to(buffer_.line_end(buffer_.cursor())); break;
            case 'd': pending_ = 'd'; return;
            case '/': mode_ = Mode::Search; query_.clear(); break;
            case 'n': {
                std::size_t hit = buffer_.find(query_, buffer_.cursor() + 1);
                if (hit == std::string::npos) hit = buffer_.find(query_, 0);
                if (hit != std::string::npos) buffer_.move_to(hit);
                break;
            }
            case 'u': break;                       // (undo would need an edit journal; noted, not faked)
            default: break;
        }
    }

    void feed(const std::string& keys) { for (char key : keys) feed(key); }

    bool save() {
        if (path_.empty()) return false;
        std::ofstream file(path_);
        file << buffer_.str();
        dirty_ = false;
        return bool(file);
    }

    std::string render(int rows = 12, int columns = 78) const {
        std::string out;
        std::size_t cursor_line = buffer_.line_number(buffer_.cursor());
        std::size_t first = cursor_line > std::size_t(rows) / 2 ? cursor_line - rows / 2 : 1;
        std::size_t line = 1, index = 0;
        while (line < first && index < buffer_.size()) { if (buffer_.at(index) == '\n') ++line; ++index; }
        for (int printed = 0; printed < rows && index <= buffer_.size(); ++printed) {
            char number[12];
            std::snprintf(number, sizeof(number), "%4zu ", line);
            out += number;
            std::string text;
            while (index < buffer_.size() && buffer_.at(index) != '\n') {
                if (index == buffer_.cursor()) text += "[";
                text += buffer_.at(index);
                if (index == buffer_.cursor()) text += "]";
                ++index;
            }
            if (index == buffer_.cursor() && index <= buffer_.size()) text += "[ ]";
            out += text.substr(0, std::size_t(columns)) + "\n";
            if (index >= buffer_.size()) break;
            ++index;
            ++line;
        }
        return out;
    }

    std::string status() const {
        const char* mode = mode_ == Mode::Insert ? "INSERT" : mode_ == Mode::Search ? "SEARCH" : "NORMAL";
        char line[200];
        std::snprintf(line, sizeof(line), "-- %s --  %s%s  line %zu/%zu  col %zu  %zu bytes%s%s",
                      mode, path_.empty() ? "[no name]" : path_.c_str(), dirty_ ? " [+]" : "",
                      buffer_.line_number(buffer_.cursor()), buffer_.lines(),
                      buffer_.cursor() - buffer_.line_start(buffer_.cursor()) + 1, buffer_.size(),
                      mode_ == Mode::Search ? "  /" : "", mode_ == Mode::Search ? query_.c_str() : "");
        return line;
    }

    GapBuffer& buffer() { return buffer_; }
    Mode mode() const { return mode_; }

private:
    // erase_forward() consumes the character after the cursor without moving the cursor,
    // so a delete must be driven by a count, never by "while cursor < end".
    void erase_to(std::size_t end) {
        std::size_t position = buffer_.cursor();
        for (std::size_t i = position; i < end; ++i)
            if (!buffer_.erase_forward()) break;
    }

    void feed_pending(char op) {
        if (op == 'w') erase_to(word_end(buffer_.cursor()));
        else if (op == 'd') {
            buffer_.move_to(buffer_.line_start(buffer_.cursor()));
            erase_to(buffer_.line_end(buffer_.cursor()));
            buffer_.erase_forward();        // the newline too
        }
    }
    std::size_t word_end(std::size_t from) const {
        std::size_t i = from;
        while (i < buffer_.size() && !std::isspace(static_cast<unsigned char>(buffer_.at(i)))) ++i;
        while (i < buffer_.size() && std::isspace(static_cast<unsigned char>(buffer_.at(i)))) ++i;
        return i;
    }
    void move_word(int direction) {
        std::size_t position = buffer_.cursor();
        if (direction > 0) buffer_.move_to(word_end(position));
        else {
            while (position > 0 && std::isspace(static_cast<unsigned char>(buffer_.at(position - 1)))) --position;
            while (position > 0 && !std::isspace(static_cast<unsigned char>(buffer_.at(position - 1)))) --position;
            buffer_.move_to(position);
        }
    }
    void move_line(int direction) {
        std::size_t position = buffer_.cursor();
        std::size_t column = position - buffer_.line_start(position);
        if (direction > 0) {
            std::size_t next = buffer_.line_end(position);
            if (next >= buffer_.size()) return;
            buffer_.move_to(std::min(next + 1 + column, buffer_.line_end(next + 1)));
        } else {
            std::size_t start = buffer_.line_start(position);
            if (start == 0) return;
            std::size_t previous = buffer_.line_start(start - 1);
            buffer_.move_to(std::min(previous + column, start - 1));
        }
    }

    GapBuffer buffer_;
    std::string path_, query_, status_;
    Mode mode_ = Mode::Normal;
    char pending_ = 0;
    bool dirty_ = false;
};

// ------------------------------------------------------------- raw mode

class RawMode {
public:
    RawMode() {
        if (!::isatty(STDIN_FILENO)) return;
        ::tcgetattr(STDIN_FILENO, &original_);
        active_ = true;
        termios raw = original_;
        raw.c_lflag &= ~(unsigned(ECHO) | unsigned(ICANON) | unsigned(ISIG) | unsigned(IEXTEN));
        raw.c_iflag &= ~(unsigned(IXON) | unsigned(ICRNL) | unsigned(BRKINT) | unsigned(INPCK) | unsigned(ISTRIP));
        raw.c_oflag &= ~unsigned(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    ~RawMode() { if (active_) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_); }   // always restore

private:
    termios original_{};
    bool active_ = false;
};

static std::string unescape(const std::string& input) {
    std::string out;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[++i];
            if (next == 'x' && i + 2 < input.size()) {
                out += char(std::stoi(input.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else if (next == 'n') out += '\n';
            else if (next == 'e') out += char(27);
            else out += next;
        } else out += input[i];
    }
    return out;
}

static int demo() {
    std::puts("1. gap buffer vs a flat string: 40k insertions at the cursor\n");
    {
        GapBuffer gap;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 40'000; ++i) gap.insert(char('a' + i % 26));
        double gap_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        std::string flat;
        start = std::chrono::steady_clock::now();
        for (int i = 0; i < 40'000; ++i) flat.insert(flat.begin() + std::ptrdiff_t(flat.size() / 2), char('a' + i % 26));
        double flat_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        auto stats = gap.stats();
        std::printf("   gap buffer           %6.2fms  (%llu inserts, gap now %zu bytes)\n",
                    gap_ms, (unsigned long long)stats.inserts, stats.gap);
        std::printf("   string::insert mid   %6.2fms  -> %.1fx slower (it memmoves the tail every time)\n",
                    flat_ms, flat_ms / gap_ms);
    }

    std::puts("\n2. a scripted editing session");
    Editor editor;
    struct Step { const char* keys; const char* what; };
    const Step steps[] = {
        {"ithe quick brown fox\\njumps over the lazy dog\\nand keeps going\\x1b", "insert three lines, then escape"},
        {"gg", "jump to the top"},
        {"wdw", "next word, then delete a word"},
        {"A -- edited\\x1b", "append at end of line"},
        {"j0", "down a line, to column 0"},
        {"dd", "delete the whole line"},
        {"/going\\n", "incremental search"},
        {"D", "delete to end of line"},
    };
    for (const Step& step : steps) {
        editor.feed(unescape(step.keys));
        std::printf("\n   after %-42s %s\n", step.what, editor.status().c_str());
        std::string rendered = editor.render(4, 70);
        std::string line;
        for (char c : rendered) {
            if (c == '\n') { std::printf("     %s\n", line.c_str()); line.clear(); }
            else line += c;
        }
    }

    auto stats = editor.buffer().stats();
    std::printf("\n   buffer: %zu bytes of text in %zu bytes of storage, gap %zu\n",
                editor.buffer().size(), stats.capacity, stats.gap);
    std::printf("   %llu inserts, %llu deletes, %llu gap moves\n",
                (unsigned long long)stats.inserts, (unsigned long long)stats.deletes,
                (unsigned long long)stats.moves);
    std::puts("\n   final text:");
    std::string text = editor.buffer().str();
    std::string line;
    for (char c : text) { if (c == '\n') { std::printf("     |%s\n", line.c_str()); line.clear(); } else line += c; }
    if (!line.empty()) std::printf("     |%s\n", line.c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::string path, script;
    bool show = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) script = argv[++i];
        else if (arg == "--show") show = true;
        else if (arg == "--demo") return demo();
        else path = arg;
    }
    if (argc == 1) return demo();

    Editor editor(path);
    if (!script.empty()) {
        editor.feed(unescape(script));
        if (show) { std::printf("%s\n%s\n", editor.render().c_str(), editor.status().c_str()); }
        else std::printf("%s", editor.buffer().str().c_str());
        return 0;
    }

    RawMode raw;
    std::printf("\033[2J\033[H");
    char key = 0;
    while (::read(STDIN_FILENO, &key, 1) == 1) {
        if (editor.mode() == Editor::Mode::Normal && key == 'q') break;
        if (editor.mode() == Editor::Mode::Normal && key == 's') { editor.save(); continue; }
        editor.feed(key);
        std::printf("\033[2J\033[H%s\n%s  (q to quit, s to save)\r\n",
                    editor.render(20).c_str(), editor.status().c_str());
        std::fflush(stdout);
    }
    std::printf("\033[2J\033[H");
    return 0;
}
