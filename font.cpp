// Text rendering from first principles: a glyph atlas, kerning, and blitting into a framebuffer.
//
//   g++ -std=c++23 -O2 font.cpp -o font && ./font --text "Hello, world" --out text.ppm
//   ./font --demo
//
// Before shaping engines and subpixel hinting, text rendering is: pack glyph
// bitmaps into one texture so the GPU binds it once, look up each character's
// rectangle, advance the pen by that glyph's width plus a kerning adjustment for
// the specific pair, and blit. This does all of it on the CPU with a built-in 5x7
// font, then writes a PPM you can actually look at.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// A 5x7 bitmap font, one std::uint8_t row per scanline (top 5 bits used).
struct Glyph { char code; std::uint8_t rows[7]; std::uint8_t width; };

static const Glyph kFont[] = {
    {'A', {0x70,0x88,0x88,0xF8,0x88,0x88,0x88}, 5}, {'B', {0xF0,0x88,0xF0,0x88,0x88,0x88,0xF0}, 5},
    {'C', {0x78,0x80,0x80,0x80,0x80,0x80,0x78}, 5}, {'D', {0xF0,0x88,0x88,0x88,0x88,0x88,0xF0}, 5},
    {'E', {0xF8,0x80,0xF0,0x80,0x80,0x80,0xF8}, 5}, {'F', {0xF8,0x80,0xF0,0x80,0x80,0x80,0x80}, 5},
    {'G', {0x78,0x80,0x80,0xB8,0x88,0x88,0x78}, 5}, {'H', {0x88,0x88,0xF8,0x88,0x88,0x88,0x88}, 5},
    {'I', {0xF8,0x20,0x20,0x20,0x20,0x20,0xF8}, 5}, {'J', {0x38,0x10,0x10,0x10,0x10,0x90,0x60}, 5},
    {'K', {0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88}, 5}, {'L', {0x80,0x80,0x80,0x80,0x80,0x80,0xF8}, 5},
    {'M', {0x88,0xD8,0xA8,0x88,0x88,0x88,0x88}, 5}, {'N', {0x88,0xC8,0xA8,0x98,0x88,0x88,0x88}, 5},
    {'O', {0x70,0x88,0x88,0x88,0x88,0x88,0x70}, 5}, {'P', {0xF0,0x88,0x88,0xF0,0x80,0x80,0x80}, 5},
    {'Q', {0x70,0x88,0x88,0x88,0xA8,0x90,0x68}, 5}, {'R', {0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88}, 5},
    {'S', {0x78,0x80,0x80,0x70,0x08,0x08,0xF0}, 5}, {'T', {0xF8,0x20,0x20,0x20,0x20,0x20,0x20}, 5},
    {'U', {0x88,0x88,0x88,0x88,0x88,0x88,0x70}, 5}, {'V', {0x88,0x88,0x88,0x88,0x88,0x50,0x20}, 5},
    {'W', {0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88}, 5}, {'X', {0x88,0x88,0x50,0x20,0x50,0x88,0x88}, 5},
    {'Y', {0x88,0x88,0x50,0x20,0x20,0x20,0x20}, 5}, {'Z', {0xF8,0x08,0x10,0x20,0x40,0x80,0xF8}, 5},
    {'a', {0x00,0x00,0x70,0x08,0x78,0x88,0x78}, 5}, {'b', {0x80,0x80,0xF0,0x88,0x88,0x88,0xF0}, 5},
    {'c', {0x00,0x00,0x78,0x80,0x80,0x80,0x78}, 5}, {'d', {0x08,0x08,0x78,0x88,0x88,0x88,0x78}, 5},
    {'e', {0x00,0x00,0x70,0x88,0xF8,0x80,0x78}, 5}, {'f', {0x30,0x40,0xE0,0x40,0x40,0x40,0x40}, 4},
    {'g', {0x00,0x00,0x78,0x88,0x78,0x08,0x70}, 5}, {'h', {0x80,0x80,0xF0,0x88,0x88,0x88,0x88}, 5},
    {'i', {0x20,0x00,0x60,0x20,0x20,0x20,0x70}, 4}, {'j', {0x10,0x00,0x30,0x10,0x10,0x90,0x60}, 4},
    {'k', {0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90}, 5}, {'l', {0x60,0x20,0x20,0x20,0x20,0x20,0x70}, 4},
    {'m', {0x00,0x00,0xD0,0xA8,0xA8,0xA8,0xA8}, 5}, {'n', {0x00,0x00,0xF0,0x88,0x88,0x88,0x88}, 5},
    {'o', {0x00,0x00,0x70,0x88,0x88,0x88,0x70}, 5}, {'p', {0x00,0x00,0xF0,0x88,0xF0,0x80,0x80}, 5},
    {'q', {0x00,0x00,0x78,0x88,0x78,0x08,0x08}, 5}, {'r', {0x00,0x00,0xB0,0xC8,0x80,0x80,0x80}, 5},
    {'s', {0x00,0x00,0x78,0x80,0x70,0x08,0xF0}, 5}, {'t', {0x40,0x40,0xE0,0x40,0x40,0x48,0x30}, 5},
    {'u', {0x00,0x00,0x88,0x88,0x88,0x88,0x78}, 5}, {'v', {0x00,0x00,0x88,0x88,0x88,0x50,0x20}, 5},
    {'w', {0x00,0x00,0x88,0x88,0xA8,0xA8,0x50}, 5}, {'x', {0x00,0x00,0x88,0x50,0x20,0x50,0x88}, 5},
    {'y', {0x00,0x00,0x88,0x88,0x78,0x08,0x70}, 5}, {'z', {0x00,0x00,0xF8,0x10,0x20,0x40,0xF8}, 5},
    {'0', {0x70,0x88,0x98,0xA8,0xC8,0x88,0x70}, 5}, {'1', {0x20,0x60,0x20,0x20,0x20,0x20,0x70}, 5},
    {'2', {0x70,0x88,0x08,0x30,0x40,0x80,0xF8}, 5}, {'3', {0xF8,0x10,0x20,0x10,0x08,0x88,0x70}, 5},
    {'4', {0x10,0x30,0x50,0x90,0xF8,0x10,0x10}, 5}, {'5', {0xF8,0x80,0xF0,0x08,0x08,0x88,0x70}, 5},
    {'6', {0x30,0x40,0x80,0xF0,0x88,0x88,0x70}, 5}, {'7', {0xF8,0x08,0x10,0x20,0x40,0x40,0x40}, 5},
    {'8', {0x70,0x88,0x70,0x88,0x88,0x88,0x70}, 5}, {'9', {0x70,0x88,0x88,0x78,0x08,0x10,0x60}, 5},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x60,0x60}, 3}, {',', {0x00,0x00,0x00,0x00,0x60,0x20,0x40}, 3},
    {'!', {0x20,0x20,0x20,0x20,0x20,0x00,0x20}, 3}, {'?', {0x70,0x88,0x08,0x30,0x20,0x00,0x20}, 5},
    {':', {0x00,0x60,0x60,0x00,0x60,0x60,0x00}, 3}, {'-', {0x00,0x00,0x00,0xF8,0x00,0x00,0x00}, 5},
    {'/', {0x08,0x10,0x10,0x20,0x40,0x40,0x80}, 5}, {' ', {0,0,0,0,0,0,0}, 3},
};

// Kerning: per-pair adjustments, because "AV" set at the same spacing as "AB" looks wrong.
static const std::map<std::pair<char, char>, int> kKerning = {
    {{'A', 'V'}, -1}, {{'V', 'A'}, -1}, {{'A', 'W'}, -1}, {{'W', 'A'}, -1},
    {{'A', 'T'}, -1}, {{'T', 'A'}, -1}, {{'T', 'o'}, -1}, {{'T', 'e'}, -1},
    {{'Y', 'o'}, -1}, {{'P', 'a'}, -1}, {{'F', 'a'}, -1}, {{'r', '.'}, -1},
    {{'o', ','}, -1}, {{'L', 'T'}, -1}, {{'W', 'o'}, -1},
};

struct Atlas {
    std::vector<std::uint8_t> pixels;      // one byte per pixel, 0 or 255
    int width = 0, height = 0;
    struct Entry { int x, y, w, h, advance; };
    std::map<char, Entry> entries;

    // Pack every glyph into one texture, in rows — the same shelf-packing a real atlas uses.
    void build(int scale, int padding = 1) {
        const int glyph_h = 7 * scale;
        int columns = 16;
        int cell_w = 6 * scale + padding * 2;
        int rows = (int(sizeof(kFont) / sizeof(kFont[0])) + columns - 1) / columns;
        width = columns * cell_w;
        height = rows * (glyph_h + padding * 2);
        pixels.assign(std::size_t(width) * height, 0);

        int index = 0;
        for (const Glyph& glyph : kFont) {
            int cell_x = (index % columns) * cell_w + padding;
            int cell_y = (index / columns) * (glyph_h + padding * 2) + padding;
            for (int row = 0; row < 7; ++row)
                for (int bit = 0; bit < 5; ++bit)
                    if (glyph.rows[row] & (0x80 >> bit))
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx) {
                                int px = cell_x + bit * scale + sx, py = cell_y + row * scale + sy;
                                if (px < width && py < height) pixels[std::size_t(py) * width + px] = 255;
                            }
            entries[glyph.code] = {cell_x, cell_y, glyph.width * scale, glyph_h, (glyph.width + 1) * scale};
            ++index;
        }
    }
};

struct Canvas {
    std::vector<std::uint8_t> pixels;
    int width, height;

    Canvas(int w, int h) : pixels(std::size_t(w) * h * 3, 16), width(w), height(h) {}

    void blit(const Atlas& atlas, const Atlas::Entry& entry, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        for (int row = 0; row < entry.h; ++row)
            for (int col = 0; col < entry.w; ++col) {
                std::uint8_t coverage = atlas.pixels[std::size_t(entry.y + row) * atlas.width + entry.x + col];
                if (!coverage) continue;
                int px = x + col, py = y + row;
                if (px < 0 || py < 0 || px >= width || py >= height) continue;
                std::size_t offset = (std::size_t(py) * width + px) * 3;
                pixels[offset] = r; pixels[offset + 1] = g; pixels[offset + 2] = b;
            }
    }

    struct Layout { int width; int glyphs; int kerns; };

    Layout draw_text(const Atlas& atlas, const std::string& text, int x, int y, int letter_spacing,
                     std::uint8_t r = 235, std::uint8_t g = 235, std::uint8_t b = 235, bool kern = true) {
        int pen = x, glyphs = 0, kerns = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto found = atlas.entries.find(text[i]);
            if (found == atlas.entries.end()) { pen += 4; continue; }
            if (kern && i > 0) {
                auto adjustment = kKerning.find({text[i - 1], text[i]});
                if (adjustment != kKerning.end()) {
                    pen += adjustment->second * (found->second.h / 7);   // scale the kern with the font
                    ++kerns;
                }
            }
            blit(atlas, found->second, pen, y, r, g, b);
            pen += found->second.advance + letter_spacing;
            ++glyphs;
        }
        return {pen - x, glyphs, kerns};
    }

    void write_ppm(const std::string& path) const {
        std::ofstream file(path, std::ios::binary);
        file << "P6\n" << width << " " << height << "\n255\n";
        file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
    }

    void print_ascii(int max_rows = 40) const {
        const char* ramp = " .:-=+*#%@";
        for (int y = 0; y < std::min(height, max_rows); ++y) {
            std::string line;
            for (int x = 0; x < std::min(width, 110); ++x) {
                std::size_t offset = (std::size_t(y) * width + x) * 3;
                int luma = (pixels[offset] + pixels[offset + 1] + pixels[offset + 2]) / 3;
                line += ramp[std::min(9, luma * 10 / 256)];
            }
            std::puts(line.c_str());
        }
    }
};

int main(int argc, char** argv) {
    std::string text = "Hello, world!";
    std::string out_path = "/tmp/text.ppm";
    int scale = 2;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--text" && i + 1 < argc) text = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (arg == "--scale" && i + 1 < argc) scale = std::atoi(argv[++i]);
    }

    Atlas atlas;
    atlas.build(scale);
    std::printf("atlas: %dx%d, %zu glyphs packed into one texture (%zu KB)\n\n",
                atlas.width, atlas.height, atlas.entries.size(), atlas.pixels.size() / 1024);

    Canvas canvas(560, 130);
    auto title = canvas.draw_text(atlas, text, 12, 12, 1, 245, 245, 245);
    canvas.draw_text(atlas, "AVAST WAVE TAT: kerned", 12, 40, 1, 150, 200, 255, true);
    canvas.draw_text(atlas, "AVAST WAVE TAT: not kerned", 12, 64, 1, 255, 170, 140, false);
    canvas.draw_text(atlas, "0123456789 - the quick brown fox", 12, 92, 1, 180, 180, 180);
    canvas.write_ppm(out_path);

    std::printf("drew %d glyphs, %d kerning pairs applied, %dpx wide\n", title.glyphs, title.kerns, title.width);
    std::printf("wrote %s\n\n", out_path.c_str());
    canvas.print_ascii(120);

    Canvas measure(1, 1);
    Atlas big;
    big.build(4);
    auto kerned = measure.draw_text(big, "AVAST WAVE TAT", 0, 0, 1, 0, 0, 0, true);
    auto plain = measure.draw_text(big, "AVAST WAVE TAT", 0, 0, 1, 0, 0, 0, false);
    std::printf("\nkerning at 4x: %dpx kerned vs %dpx unkerned across %d pairs (%d px tighter)\n",
                kerned.width, plain.width, kerned.kerns, plain.width - kerned.width);
    return 0;
}
