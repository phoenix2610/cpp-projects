// Huffman coding end to end: build the tree, pack the bits, verify the round trip.
//
//   g++ -std=c++23 -O2 huffman.cpp -o huffman
//   ./huffman compress input.txt out.huf && ./huffman decompress out.huf back.txt
//   ./huffman --demo
//
// The tree is the easy half. The half that actually bites is bit packing: codes
// are not byte-aligned, so you carry a bit buffer, and the file must record how
// many bits of the final byte are real — otherwise decoding walks into padding and
// emits phantom symbols. The canonical-code trick keeps the header small: store
// only each symbol's code *length*, and both sides rebuild identical codes from it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <fstream>
#include <memory>
#include <queue>
#include <string>
#include <vector>

struct Node {
    std::uint64_t weight;
    int symbol;                       // -1 for internal nodes
    Node* left = nullptr;
    Node* right = nullptr;
};

struct Compare {
    bool operator()(const Node* a, const Node* b) const {
        if (a->weight != b->weight) return a->weight > b->weight;
        return a->symbol > b->symbol;          // deterministic ties: same tree on both sides
    }
};

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void write(std::uint32_t code, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            buffer_ = static_cast<std::uint8_t>((buffer_ << 1) | ((code >> i) & 1));
            if (++count_ == 8) { out_.push_back(buffer_); buffer_ = 0; count_ = 0; }
        }
    }
    int flush() {                                  // returns how many bits of the last byte are real
        if (count_ == 0) return 8;
        int valid = count_;
        buffer_ <<= (8 - count_);
        out_.push_back(buffer_);
        buffer_ = 0; count_ = 0;
        return valid;
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint8_t buffer_ = 0;
    int count_ = 0;
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size, int valid_bits_in_last)
        : data_(data), size_(size), last_bits_(valid_bits_in_last) {}
    bool read(int& bit) {
        if (byte_ >= size_) return false;
        int available = (byte_ + 1 == size_) ? last_bits_ : 8;
        if (bit_ >= available) return false;
        bit = (data_[byte_] >> (7 - bit_)) & 1;
        if (++bit_ == 8) { bit_ = 0; ++byte_; }
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_, byte_ = 0;
    int bit_ = 0, last_bits_;
};

// Canonical codes: sort by (length, symbol), then hand out consecutive values.
static std::array<std::uint32_t, 256> canonical_codes(const std::array<std::uint8_t, 256>& lengths) {
    std::array<std::uint32_t, 256> codes{};
    std::vector<int> order;
    for (int s = 0; s < 256; ++s) if (lengths[s]) order.push_back(s);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return lengths[a] != lengths[b] ? lengths[a] < lengths[b] : a < b;
    });
    std::uint32_t code = 0;
    int previous = order.empty() ? 0 : lengths[order.front()];
    for (int symbol : order) {
        code <<= (lengths[symbol] - previous);
        previous = lengths[symbol];
        codes[symbol] = code++;
    }
    return codes;
}

static std::array<std::uint8_t, 256> code_lengths(const std::array<std::uint64_t, 256>& freq) {
    std::vector<std::unique_ptr<Node>> pool;
    std::priority_queue<Node*, std::vector<Node*>, Compare> heap;
    for (int s = 0; s < 256; ++s)
        if (freq[s]) { pool.push_back(std::make_unique<Node>(freq[s], s)); heap.push(pool.back().get()); }
    if (heap.empty()) return {};
    if (heap.size() == 1) {                        // a file of one repeated byte still needs one bit
        std::array<std::uint8_t, 256> single{};
        single[heap.top()->symbol] = 1;
        return single;
    }
    while (heap.size() > 1) {
        Node* a = heap.top(); heap.pop();
        Node* b = heap.top(); heap.pop();
        pool.push_back(std::make_unique<Node>(a->weight + b->weight, -1, a, b));
        heap.push(pool.back().get());
    }
    std::array<std::uint8_t, 256> lengths{};
    struct Walk {
        static void go(const Node* node, int depth, std::array<std::uint8_t, 256>& out) {
            if (node->symbol >= 0) { out[node->symbol] = static_cast<std::uint8_t>(std::max(depth, 1)); return; }
            go(node->left, depth + 1, out);
            go(node->right, depth + 1, out);
        }
    };
    Walk::go(heap.top(), 0, lengths);
    return lengths;
}

struct Encoded {
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 256> lengths{};
    std::uint64_t original_size = 0;
    int last_bits = 8;
};

static Encoded compress(const std::vector<std::uint8_t>& input) {
    std::array<std::uint64_t, 256> freq{};
    for (std::uint8_t byte : input) ++freq[byte];
    Encoded out;
    out.original_size = input.size();
    out.lengths = code_lengths(freq);
    auto codes = canonical_codes(out.lengths);
    BitWriter writer(out.bytes);
    for (std::uint8_t byte : input) writer.write(codes[byte], out.lengths[byte]);
    out.last_bits = writer.flush();
    return out;
}

static std::vector<std::uint8_t> decompress(const Encoded& in) {
    auto codes = canonical_codes(in.lengths);
    // decode table keyed by (length, code) — a real implementation would build a LUT
    std::vector<std::pair<std::pair<int, std::uint32_t>, std::uint8_t>> table;
    for (int s = 0; s < 256; ++s)
        if (in.lengths[s]) table.push_back({{in.lengths[s], codes[s]}, static_cast<std::uint8_t>(s)});
    std::sort(table.begin(), table.end());

    std::vector<std::uint8_t> out;
    out.reserve(in.original_size);
    BitReader reader(in.bytes.data(), in.bytes.size(), in.last_bits);
    std::uint32_t code = 0;
    int length = 0, bit = 0;
    while (out.size() < in.original_size && reader.read(bit)) {
        code = (code << 1) | static_cast<std::uint32_t>(bit);
        ++length;
        auto found = std::lower_bound(table.begin(), table.end(),
                                      std::pair{std::pair{length, code}, std::uint8_t{0}});
        if (found != table.end() && found->first == std::pair{length, code}) {
            out.push_back(found->second);
            code = 0; length = 0;
        }
    }
    return out;
}

static void write_file(const std::string& path, const Encoded& enc) {
    std::ofstream file(path, std::ios::binary);
    file.write("HUF1", 4);
    file.write(reinterpret_cast<const char*>(&enc.original_size), 8);
    std::uint8_t last = static_cast<std::uint8_t>(enc.last_bits);
    file.write(reinterpret_cast<const char*>(&last), 1);
    file.write(reinterpret_cast<const char*>(enc.lengths.data()), 256);   // the whole header
    file.write(reinterpret_cast<const char*>(enc.bytes.data()), static_cast<std::streamsize>(enc.bytes.size()));
}

static Encoded read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    char magic[4];
    Encoded enc;
    file.read(magic, 4);
    if (std::memcmp(magic, "HUF1", 4) != 0) throw std::runtime_error("not a HUF1 file");
    file.read(reinterpret_cast<char*>(&enc.original_size), 8);
    std::uint8_t last = 8;
    file.read(reinterpret_cast<char*>(&last), 1);
    enc.last_bits = last;
    file.read(reinterpret_cast<char*>(enc.lengths.data()), 256);
    enc.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return enc;
}

static std::vector<std::uint8_t> slurp(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static double entropy(const std::vector<std::uint8_t>& data) {
    std::array<std::uint64_t, 256> freq{};
    for (std::uint8_t b : data) ++freq[b];
    double bits = 0;
    for (std::uint64_t f : freq)
        if (f) { double p = double(f) / double(data.size()); bits -= p * std::log2(p); }
    return bits;
}

static int demo() {
    struct Case { const char* name; std::vector<std::uint8_t> data; };
    std::vector<Case> cases;
    {
        std::string english;
        while (english.size() < 60'000)
            english += "the quick brown fox jumps over the lazy dog while the compiler warns about narrowing. ";
        cases.push_back({"english text", {english.begin(), english.end()}});
    }
    {
        std::vector<std::uint8_t> skewed(60'000, 'A');
        for (std::size_t i = 0; i < skewed.size(); i += 37) skewed[i] = 'B';
        cases.push_back({"one byte, mostly", skewed});
    }
    {
        std::vector<std::uint8_t> random(60'000);
        std::uint64_t state = 88172645463325252ULL;
        for (auto& b : random) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; b = std::uint8_t(state); }
        cases.push_back({"random bytes", random});
    }
    cases.push_back({"empty file", {}});
    cases.push_back({"single byte", {'x'}});

    std::printf("%-18s %9s %9s %7s %8s %8s\n", "input", "original", "packed", "ratio", "entropy", "roundtrip");
    for (auto& c : cases) {
        Encoded enc = compress(c.data);
        std::vector<std::uint8_t> back = decompress(enc);
        bool ok = back == c.data;
        double packed = double(enc.bytes.size() + 269);
        double ratio = c.data.empty() ? 0 : packed / double(c.data.size());
        std::printf("%-18s %9zu %9zu %6.2fx %7.2f  %8s\n", c.name, c.data.size(),
                    enc.bytes.size() + 269, ratio, c.data.empty() ? 0.0 : entropy(c.data),
                    ok ? "ok" : "FAILED");
    }
    std::puts("\n  (packed size includes the 269-byte header: magic, length, and 256 code lengths)");
    std::puts("  entropy is the theoretical floor in bits/byte — Huffman gets within one bit of it");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--demo") return demo();
    std::string mode = argv[1];
    if (argc < 4) { std::puts("usage: huffman compress|decompress <in> <out>"); return 1; }
    if (mode == "compress") {
        auto data = slurp(argv[2]);
        Encoded enc = compress(data);
        write_file(argv[3], enc);
        std::printf("  %zu -> %zu bytes (%.1f%% of the original)\n", data.size(), enc.bytes.size() + 269,
                    data.empty() ? 0.0 : 100.0 * double(enc.bytes.size() + 269) / double(data.size()));
    } else {
        Encoded enc = read_file(argv[2]);
        auto data = decompress(enc);
        std::ofstream out(argv[3], std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        std::printf("  restored %zu bytes\n", data.size());
    }
    return 0;
}
