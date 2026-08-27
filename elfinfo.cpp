// Read an ELF binary the way the loader does: headers, sections, symbols, dependencies.
//
//   g++ -std=c++23 -O2 elfinfo.cpp -o elfinfo && ./elfinfo ./elfinfo
//   ./elfinfo /bin/ls --symbols --sections
//
// An ELF file is two overlapping views of the same bytes: section headers describe
// it for the *linker* (.text, .rodata, .symtab), program headers describe it for the
// *loader* (which chunks to map, where, and with what permissions). Stripping a
// binary throws away the first view entirely — which is why a stripped binary still
// runs but shows nothing useful in a backtrace.

#include <elf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

struct Elf {
    std::vector<char> bytes;
    const Elf64_Ehdr* header = nullptr;

    bool load(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (bytes.size() < sizeof(Elf64_Ehdr)) return false;
        header = reinterpret_cast<const Elf64_Ehdr*>(bytes.data());
        return std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 && header->e_ident[EI_CLASS] == ELFCLASS64;
    }

    const Elf64_Shdr* sections() const {
        return reinterpret_cast<const Elf64_Shdr*>(bytes.data() + header->e_shoff);
    }
    const Elf64_Phdr* segments() const {
        return reinterpret_cast<const Elf64_Phdr*>(bytes.data() + header->e_phoff);
    }
    const char* section_names() const {
        return bytes.data() + sections()[header->e_shstrndx].sh_offset;
    }
    const Elf64_Shdr* find(const char* name) const {
        for (int i = 0; i < header->e_shnum; ++i)
            if (std::strcmp(section_names() + sections()[i].sh_name, name) == 0) return &sections()[i];
        return nullptr;
    }
};

static const char* type_name(std::uint16_t type) {
    switch (type) {
        case ET_REL: return "relocatable (.o)";
        case ET_EXEC: return "executable (fixed address)";
        case ET_DYN: return "shared object / PIE";
        case ET_CORE: return "core dump";
        default: return "unknown";
    }
}

static const char* segment_name(std::uint32_t type) {
    switch (type) {
        case PT_LOAD: return "LOAD";
        case PT_DYNAMIC: return "DYNAMIC";
        case PT_INTERP: return "INTERP";
        case PT_NOTE: return "NOTE";
        case PT_PHDR: return "PHDR";
        case PT_TLS: return "TLS";
        case PT_GNU_EH_FRAME: return "EH_FRAME";
        case PT_GNU_STACK: return "GNU_STACK";
        case PT_GNU_RELRO: return "GNU_RELRO";
        default: return "other";
    }
}

static std::string permissions(std::uint32_t flags) {
    return std::string(flags & PF_R ? "r" : "-") + (flags & PF_W ? "w" : "-") + (flags & PF_X ? "x" : "-");
}

static const char* symbol_type(unsigned char info) {
    switch (ELF64_ST_TYPE(info)) {
        case STT_FUNC: return "func";
        case STT_OBJECT: return "object";
        case STT_SECTION: return "section";
        case STT_FILE: return "file";
        case STT_NOTYPE: return "notype";
        default: return "other";
    }
}

static std::string human(std::uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1 << 20) std::snprintf(buffer, sizeof(buffer), "%.1fM", double(bytes) / (1 << 20));
    else if (bytes >= 1 << 10) std::snprintf(buffer, sizeof(buffer), "%.1fK", double(bytes) / (1 << 10));
    else std::snprintf(buffer, sizeof(buffer), "%lluB", (unsigned long long)bytes);
    return buffer;
}

int main(int argc, char** argv) {
    std::string path = argc > 1 && argv[1][0] != '-' ? argv[1] : "/proc/self/exe";
    bool want_symbols = false, want_sections = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--symbols") want_symbols = true;
        if (std::string(argv[i]) == "--sections") want_sections = true;
    }

    Elf elf;
    if (!elf.load(path)) {
        std::printf("not a 64-bit ELF file: %s\n", path.c_str());
        return 1;
    }
    const Elf64_Ehdr* h = elf.header;

    std::printf("%s  (%s on disk)\n", path.c_str(), human(elf.bytes.size()).c_str());
    std::printf("  type          %s\n", type_name(h->e_type));
    std::printf("  machine       %s\n", h->e_machine == EM_X86_64 ? "x86-64" : h->e_machine == EM_AARCH64 ? "aarch64" : "other");
    std::printf("  entry point   0x%llx\n", (unsigned long long)h->e_entry);
    std::printf("  sections      %d, segments %d\n", h->e_shnum, h->e_phnum);

    std::puts("\nloader's view — program headers (what actually gets mapped):");
    std::uint64_t mapped = 0;
    for (int i = 0; i < h->e_phnum; ++i) {
        const Elf64_Phdr& p = elf.segments()[i];
        if (p.p_type == PT_LOAD) mapped += p.p_memsz;
        if (p.p_type == PT_INTERP) {
            std::printf("  %-10s %s\n", "INTERP", elf.bytes.data() + p.p_offset);
            continue;
        }
        std::printf("  %-10s vaddr 0x%08llx  file %-8s mem %-8s %s\n", segment_name(p.p_type),
                    (unsigned long long)p.p_vaddr, human(p.p_filesz).c_str(), human(p.p_memsz).c_str(),
                    permissions(p.p_flags).c_str());
    }
    std::printf("  total mapped at load: %s\n", human(mapped).c_str());

    // W^X check: a segment that is both writable and executable is a red flag
    bool wx = false;
    for (int i = 0; i < h->e_phnum; ++i) {
        const Elf64_Phdr& p = elf.segments()[i];
        if (p.p_type == PT_LOAD && (p.p_flags & PF_W) && (p.p_flags & PF_X)) wx = true;
    }
    std::printf("  writable+executable segment: %s\n", wx ? "YES (unusual — check why)" : "no");

    std::puts("\nlinker's view — where the bytes went:");
    std::vector<std::pair<std::string, std::uint64_t>> sizes;
    for (int i = 0; i < h->e_shnum; ++i) {
        const Elf64_Shdr& s = elf.sections()[i];
        std::string name = elf.section_names() + s.sh_name;
        if (s.sh_size) sizes.emplace_back(name, s.sh_size);
        if (want_sections)
            std::printf("  %-20s %-8s at 0x%08llx  %s\n", name.c_str(), human(s.sh_size).c_str(),
                        (unsigned long long)s.sh_addr,
                        s.sh_flags & SHF_EXECINSTR ? "code" : s.sh_flags & SHF_WRITE ? "data" : "read-only");
    }
    std::sort(sizes.begin(), sizes.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (!want_sections)
        for (std::size_t i = 0; i < std::min<std::size_t>(8, sizes.size()); ++i)
            std::printf("  %-20s %s\n", sizes[i].first.c_str(), human(sizes[i].second).c_str());

    const Elf64_Shdr* dynamic = elf.find(".dynamic");
    const Elf64_Shdr* dynstr = elf.find(".dynstr");
    if (dynamic && dynstr) {
        std::puts("\nshared library dependencies:");
        const auto* entries = reinterpret_cast<const Elf64_Dyn*>(elf.bytes.data() + dynamic->sh_offset);
        const char* strings = elf.bytes.data() + dynstr->sh_offset;
        std::string runpath;
        for (std::size_t i = 0; entries[i].d_tag != DT_NULL; ++i) {
            if (entries[i].d_tag == DT_NEEDED) std::printf("  %s\n", strings + entries[i].d_un.d_val);
            if (entries[i].d_tag == DT_RUNPATH || entries[i].d_tag == DT_RPATH)
                runpath = strings + entries[i].d_un.d_val;
        }
        if (!runpath.empty()) std::printf("  (runpath: %s)\n", runpath.c_str());
    }

    const Elf64_Shdr* symtab = elf.find(".symtab");
    const Elf64_Shdr* strtab = elf.find(".strtab");
    std::puts("");
    if (!symtab) {
        std::puts("no .symtab — this binary is stripped (a backtrace here shows addresses, not names)");
    } else {
        const auto* symbols = reinterpret_cast<const Elf64_Sym*>(elf.bytes.data() + symtab->sh_offset);
        std::size_t count = symtab->sh_size / sizeof(Elf64_Sym);
        const char* strings = elf.bytes.data() + strtab->sh_offset;
        std::map<std::string, int> by_type;
        std::vector<std::pair<std::uint64_t, std::string>> functions;
        for (std::size_t i = 0; i < count; ++i) {
            by_type[symbol_type(symbols[i].st_info)]++;
            if (ELF64_ST_TYPE(symbols[i].st_info) == STT_FUNC && symbols[i].st_size)
                functions.emplace_back(symbols[i].st_size, strings + symbols[i].st_name);
        }
        std::printf("symbols: %zu total (", count);
        for (auto& [type, n] : by_type) std::printf("%s %d  ", type.c_str(), n);
        std::puts(")");
        std::sort(functions.rbegin(), functions.rend());
        std::puts("largest functions by code size:");
        for (std::size_t i = 0; i < std::min<std::size_t>(want_symbols ? 20 : 6, functions.size()); ++i)
            std::printf("  %-8s %s\n", human(functions[i].first).c_str(), functions[i].second.c_str());
    }
    return 0;
}
