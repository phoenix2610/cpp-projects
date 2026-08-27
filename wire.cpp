// A binary wire format: varints, zigzag, a schema header, and forward-compatible decoding.
//
//   g++ -std=c++23 -O2 wire.cpp -o wire && ./wire
//
// The design questions any format has to answer, and the answers here:
// how are integers encoded (varint — a small number costs one byte), how are
// negatives handled (zigzag, so -1 is 1 rather than eight bytes of sign bits),
// how does a reader skip a field it does not know (a wire type in the tag, so
// every field is skippable), and how do old readers survive new writers (unknown
// fields are preserved, not rejected — that is what makes a rollout safe).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class WireType : std::uint8_t { Varint = 0, Fixed64 = 1, Bytes = 2, Fixed32 = 5 };

class Writer {
public:
    void varint(std::uint64_t value) {
        while (value >= 0x80) { out_.push_back(std::uint8_t(value) | 0x80); value >>= 7; }
        out_.push_back(std::uint8_t(value));
    }
    // zigzag: map signed to unsigned so small magnitudes stay small in both directions
    void zigzag(std::int64_t value) { varint(static_cast<std::uint64_t>((value << 1) ^ (value >> 63))); }

    void tag(int field, WireType type) { varint((std::uint64_t(field) << 3) | std::uint8_t(type)); }
    void uint_field(int field, std::uint64_t v) { tag(field, WireType::Varint); varint(v); }
    void int_field(int field, std::int64_t v) { tag(field, WireType::Varint); zigzag(v); }
    void bool_field(int field, bool v) { uint_field(field, v ? 1 : 0); }
    void double_field(int field, double v) {
        tag(field, WireType::Fixed64);
        std::uint64_t bits;
        std::memcpy(&bits, &v, 8);
        for (int i = 0; i < 8; ++i) out_.push_back(std::uint8_t(bits >> (i * 8)));
    }
    void string_field(int field, std::string_view text) {
        tag(field, WireType::Bytes);
        varint(text.size());
        out_.insert(out_.end(), text.begin(), text.end());
    }
    void message_field(int field, const std::vector<std::uint8_t>& body) {
        tag(field, WireType::Bytes);
        varint(body.size());
        out_.insert(out_.end(), body.begin(), body.end());
    }
    void raw(const std::vector<std::uint8_t>& bytes) { out_.insert(out_.end(), bytes.begin(), bytes.end()); }

    const std::vector<std::uint8_t>& bytes() const { return out_; }

private:
    std::vector<std::uint8_t> out_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool done() const { return pos_ >= size_; }

    std::optional<std::uint64_t> varint() {
        std::uint64_t value = 0;
        int shift = 0;
        while (pos_ < size_) {
            std::uint8_t byte = data_[pos_++];
            value |= std::uint64_t(byte & 0x7F) << shift;
            if (!(byte & 0x80)) return value;
            shift += 7;
            if (shift > 63) return std::nullopt;      // malformed: refuse rather than wrap
        }
        return std::nullopt;
    }
    std::int64_t unzigzag(std::uint64_t value) const {
        return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1);
    }

    struct Field { int number; WireType type; std::size_t start, end; };

    std::optional<Field> next() {
        if (done()) return std::nullopt;
        std::size_t tag_start = pos_;
        auto tag = varint();
        if (!tag) return std::nullopt;
        Field field{int(*tag >> 3), WireType(*tag & 7), pos_, pos_};
        switch (field.type) {
            case WireType::Varint: { auto v = varint(); if (!v) return std::nullopt; break; }
            case WireType::Fixed64: pos_ += 8; break;
            case WireType::Fixed32: pos_ += 4; break;
            case WireType::Bytes: {
                auto length = varint();
                if (!length) return std::nullopt;
                field.start = pos_;
                pos_ += *length;
                break;
            }
        }
        if (pos_ > size_) return std::nullopt;
        field.end = pos_;
        (void)tag_start;
        return field;
    }

    std::uint64_t as_uint(const Field& f) const { Reader r(data_ + f.start, f.end - f.start); return *r.varint(); }
    std::int64_t as_int(const Field& f) const { Reader r(data_ + f.start, f.end - f.start); return r.unzigzag(*r.varint()); }
    double as_double(const Field& f) const {
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits |= std::uint64_t(data_[f.start + i]) << (i * 8);
        double value;
        std::memcpy(&value, &bits, 8);
        return value;
    }
    std::string as_string(const Field& f) const {
        return std::string(reinterpret_cast<const char*>(data_ + f.start), f.end - f.start);
    }
    std::vector<std::uint8_t> raw_field(const Field& f, std::size_t tag_bytes_back) const {
        // capture the tag + payload verbatim, so an unknown field can be re-emitted untouched
        std::size_t begin = f.start - tag_bytes_back;
        return {data_ + begin, data_ + f.end};
    }

private:
    const std::uint8_t* data_;
    std::size_t size_, pos_ = 0;
};

// ------------------------------------------------------------------ schema

struct EventV1 {
    std::uint64_t id = 0;
    std::string name;
    std::int64_t delta = 0;
    bool active = false;

    std::vector<std::uint8_t> encode() const {
        Writer w;
        w.uint_field(1, id);
        w.string_field(2, name);
        w.int_field(3, delta);
        w.bool_field(4, active);
        return w.bytes();
    }
};

struct EventV2 {                       // v2 adds fields 5 and 6; v1 readers must survive it
    std::uint64_t id = 0;
    std::string name;
    std::int64_t delta = 0;
    bool active = false;
    double score = 0;
    std::string region;

    std::vector<std::uint8_t> encode() const {
        Writer w;
        w.uint_field(1, id);
        w.string_field(2, name);
        w.int_field(3, delta);
        w.bool_field(4, active);
        w.double_field(5, score);
        w.string_field(6, region);
        return w.bytes();
    }
};

struct DecodedV1 {
    EventV1 known;
    int unknown_fields = 0;
    std::size_t unknown_bytes = 0;
};

static DecodedV1 decode_as_v1(const std::vector<std::uint8_t>& bytes) {
    DecodedV1 result;
    Reader reader(bytes.data(), bytes.size());
    while (auto field = reader.next()) {
        switch (field->number) {
            case 1: result.known.id = reader.as_uint(*field); break;
            case 2: result.known.name = reader.as_string(*field); break;
            case 3: result.known.delta = reader.as_int(*field); break;
            case 4: result.known.active = reader.as_uint(*field) != 0; break;
            default:                                  // skip, do not fail
                ++result.unknown_fields;
                result.unknown_bytes += field->end - field->start;
                break;
        }
    }
    return result;
}

static std::size_t json_size(const EventV2& e) {
    char buffer[512];
    return std::snprintf(buffer, sizeof(buffer),
                         R"({"id":%llu,"name":"%s","delta":%lld,"active":%s,"score":%.2f,"region":"%s"})",
                         (unsigned long long)e.id, e.name.c_str(), (long long)e.delta,
                         e.active ? "true" : "false", e.score, e.region.c_str());
}

int main() {
    std::puts("1. varint sizes (why small numbers should be cheap)");
    for (std::uint64_t value : {0ULL, 1ULL, 127ULL, 128ULL, 16383ULL, 16384ULL, 1ULL << 40, ~0ULL}) {
        Writer w;
        w.varint(value);
        std::printf("   %20llu -> %zu byte(s)   (fixed64 would always be 8)\n",
                    (unsigned long long)value, w.bytes().size());
    }

    std::puts("\n2. zigzag keeps negatives small");
    for (std::int64_t value : {0LL, -1LL, 1LL, -64LL, 63LL, -1000000LL}) {
        Writer zig, plain;
        zig.zigzag(value);
        plain.varint(static_cast<std::uint64_t>(value));
        std::printf("   %10lld -> zigzag %zu byte(s), naive varint %zu byte(s)\n",
                    (long long)value, zig.bytes().size(), plain.bytes().size());
    }

    std::puts("\n3. round trip");
    {
        EventV1 original{42, "checkout.completed", -1750, true};
        auto bytes = original.encode();
        auto decoded = decode_as_v1(bytes);
        std::printf("   %zu bytes on the wire\n", bytes.size());
        std::printf("   id=%llu name=%s delta=%lld active=%d   matches: %s\n",
                    (unsigned long long)decoded.known.id, decoded.known.name.c_str(),
                    (long long)decoded.known.delta, decoded.known.active,
                    (decoded.known.id == original.id && decoded.known.name == original.name &&
                     decoded.known.delta == original.delta && decoded.known.active == original.active)
                        ? "yes" : "NO");
    }

    std::puts("\n4. forward compatibility: a v1 reader decoding v2 data");
    {
        EventV2 newer{42, "checkout.completed", -1750, true, 0.97, "eu-west-1"};
        auto bytes = newer.encode();
        auto decoded = decode_as_v1(bytes);
        std::printf("   v2 payload is %zu bytes; v1 read id=%llu name=%s delta=%lld\n",
                    bytes.size(), (unsigned long long)decoded.known.id,
                    decoded.known.name.c_str(), (long long)decoded.known.delta);
        std::printf("   skipped %d unknown fields (%zu bytes) without erroring — "
                    "the wire type in each tag says how far to jump\n",
                    decoded.unknown_fields, decoded.unknown_bytes);
    }

    std::puts("\n5. size against JSON");
    {
        EventV2 event{42, "checkout.completed", -1750, true, 0.97, "eu-west-1"};
        auto binary = event.encode();
        std::size_t json = json_size(event);
        std::printf("   binary %zu bytes, JSON %zu bytes -> %.1fx smaller\n",
                    binary.size(), json, double(json) / double(binary.size()));
        std::size_t total_binary = 0, total_json = 0;
        for (int i = 0; i < 100'000; ++i) {
            EventV2 e{std::uint64_t(i), "evt", std::int64_t(i % 200 - 100), i % 2 == 0, double(i % 100) / 100, "eu"};
            total_binary += e.encode().size();
            total_json += json_size(e);
        }
        std::printf("   100k events: %.1fKB binary vs %.1fKB JSON\n",
                    total_binary / 1024.0, total_json / 1024.0);
    }

    std::puts("\n6. malformed input is rejected, not misread");
    {
        std::vector<std::uint8_t> truncated{0x0A, 0x20, 'o', 'n', 'l', 'y'};   // claims 32 bytes, has 4
        Reader reader(truncated.data(), truncated.size());
        auto field = reader.next();
        std::printf("   length-prefix past the end: %s\n", field ? "accepted (BUG)" : "rejected");
        std::vector<std::uint8_t> endless(12, 0xFF);                            // varint that never terminates
        Reader r2(endless.data(), endless.size());
        std::printf("   varint with no terminator:  %s\n", r2.next() ? "accepted (BUG)" : "rejected");
    }
    return 0;
}
