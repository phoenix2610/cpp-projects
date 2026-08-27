// A legal chess move generator on bitboards, checked against published perft counts.
//
//   g++ -std=c++23 -O2 chess.cpp -o chess && ./chess --perft 5
//   ./chess --fen "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -" --perft 4
//
// A board is twelve 64-bit integers, one per piece type: "all white pawns that can
// push" is a shift and an AND, not a loop over 64 squares. Perft — count the leaves
// of the move tree to depth N — is the only test that matters here, because every
// rule you got subtly wrong (en passant discovered check, castling through an
// attacked square, pinned-piece moves) changes the count at some depth.

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Bitboard = std::uint64_t;

enum Piece { Pawn, Knight, Bishop, Rook, Queen, King, PieceCount };
enum Color { White, Black };

constexpr Bitboard kFileA = 0x0101010101010101ULL;
constexpr Bitboard kFileH = 0x8080808080808080ULL;

static inline int pop_lsb(Bitboard& board) {
    int square = __builtin_ctzll(board);
    board &= board - 1;
    return square;
}
static inline Bitboard square_bit(int square) { return 1ULL << square; }

struct Move {
    std::uint8_t from, to;
    std::uint8_t promotion = 0;      // 0 = none, else Piece
    bool en_passant = false, castle = false;
};

struct Position {
    std::array<std::array<Bitboard, PieceCount>, 2> pieces{};
    Color side = White;
    std::uint8_t castling = 0;       // 1 K, 2 Q, 4 k, 8 q
    int en_passant_square = -1;

    Bitboard occupied(Color color) const {
        Bitboard all = 0;
        for (Bitboard board : pieces[color]) all |= board;
        return all;
    }
    Bitboard occupied() const { return occupied(White) | occupied(Black); }
};

// ---------------------------------------------------------- attack tables

static std::array<Bitboard, 64> knight_attacks, king_attacks;
static std::array<std::array<Bitboard, 64>, 2> pawn_attacks;

static void init_tables() {
    for (int square = 0; square < 64; ++square) {
        Bitboard bit = square_bit(square);
        Bitboard not_a = bit & ~kFileA, not_h = bit & ~kFileH;
        Bitboard not_ab = bit & ~(kFileA | (kFileA << 1));
        Bitboard not_gh = bit & ~(kFileH | (kFileH >> 1));
        knight_attacks[square] = (not_a << 15) | (not_h << 17) | (not_ab << 6) | (not_gh << 10)
                               | (not_h >> 15) | (not_a >> 17) | (not_gh >> 6) | (not_ab >> 10);
        king_attacks[square] = (bit << 8) | (bit >> 8) | (not_h << 1) | (not_a >> 1)
                             | (not_h << 9) | (not_a << 7) | (not_h >> 7) | (not_a >> 9);
        pawn_attacks[White][square] = (not_h << 9) | (not_a << 7);
        pawn_attacks[Black][square] = (not_a >> 9) | (not_h >> 7);
    }
}

// Sliding attacks by ray-walking. Magic bitboards are faster; this is the same answer.
static Bitboard ray_attacks(int square, Bitboard blockers, const int deltas[4][2]) {
    Bitboard attacks = 0;
    int rank = square / 8, file = square % 8;
    for (int d = 0; d < 4; ++d) {
        int r = rank + deltas[d][0], f = file + deltas[d][1];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int target = r * 8 + f;
            attacks |= square_bit(target);
            if (blockers & square_bit(target)) break;      // stop at the first blocker, inclusive
            r += deltas[d][0];
            f += deltas[d][1];
        }
    }
    return attacks;
}
static const int kBishopDeltas[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
static const int kRookDeltas[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

static Bitboard bishop_attacks(int square, Bitboard blockers) { return ray_attacks(square, blockers, kBishopDeltas); }
static Bitboard rook_attacks(int square, Bitboard blockers) { return ray_attacks(square, blockers, kRookDeltas); }

static bool is_attacked(const Position& position, int square, Color by) {
    Bitboard all = position.occupied();
    if (pawn_attacks[by == White ? Black : White][square] & position.pieces[by][Pawn]) return true;
    if (knight_attacks[square] & position.pieces[by][Knight]) return true;
    if (king_attacks[square] & position.pieces[by][King]) return true;
    Bitboard diagonal = position.pieces[by][Bishop] | position.pieces[by][Queen];
    if (bishop_attacks(square, all) & diagonal) return true;
    Bitboard straight = position.pieces[by][Rook] | position.pieces[by][Queen];
    if (rook_attacks(square, all) & straight) return true;
    return false;
}

static void make_move(Position& position, const Move& move) {
    Color us = position.side, them = Color(1 - us);
    Bitboard from = square_bit(move.from), to = square_bit(move.to);

    int moved = -1;
    for (int piece = 0; piece < PieceCount; ++piece)
        if (position.pieces[us][piece] & from) { moved = piece; break; }

    for (int piece = 0; piece < PieceCount; ++piece) position.pieces[them][piece] &= ~to;   // capture
    position.pieces[us][moved] &= ~from;
    position.pieces[us][moved] |= to;

    if (move.en_passant) {
        int captured_square = us == White ? move.to - 8 : move.to + 8;
        position.pieces[them][Pawn] &= ~square_bit(captured_square);
    }
    if (move.promotion) {
        position.pieces[us][Pawn] &= ~to;
        position.pieces[us][move.promotion] |= to;
    }
    if (move.castle) {                                        // move the rook too
        int rook_from = move.to > move.from ? move.from + 3 : move.from - 4;
        int rook_to = move.to > move.from ? move.to - 1 : move.to + 1;
        position.pieces[us][Rook] &= ~square_bit(rook_from);
        position.pieces[us][Rook] |= square_bit(rook_to);
    }

    // castling rights die when the king or a rook moves, or a rook is captured
    if (moved == King) position.castling &= us == White ? ~0x3 : ~0xC;
    if (move.from == 0 || move.to == 0) position.castling &= ~0x2;
    if (move.from == 7 || move.to == 7) position.castling &= ~0x1;
    if (move.from == 56 || move.to == 56) position.castling &= ~0x8;
    if (move.from == 63 || move.to == 63) position.castling &= ~0x4;

    position.en_passant_square = -1;
    if (moved == Pawn && std::abs(int(move.to) - int(move.from)) == 16)
        position.en_passant_square = (move.from + move.to) / 2;
    position.side = them;
}

static void generate(const Position& position, std::vector<Move>& moves) {
    Color us = position.side, them = Color(1 - us);
    Bitboard mine = position.occupied(us), theirs = position.occupied(them), all = mine | theirs;
    int forward = us == White ? 8 : -8;

    Bitboard pawns = position.pieces[us][Pawn];
    while (pawns) {
        int from = pop_lsb(pawns);
        int to = from + forward;
        if (to >= 0 && to < 64 && !(all & square_bit(to))) {
            bool promoting = (to / 8 == 7 || to / 8 == 0);
            if (promoting)
                for (int piece : {Queen, Rook, Bishop, Knight})
                    moves.push_back({std::uint8_t(from), std::uint8_t(to), std::uint8_t(piece)});
            else {
                moves.push_back({std::uint8_t(from), std::uint8_t(to)});
                int start_rank = us == White ? 1 : 6;
                int two = to + forward;
                if (from / 8 == start_rank && !(all & square_bit(two)))
                    moves.push_back({std::uint8_t(from), std::uint8_t(two)});
            }
        }
        Bitboard targets = pawn_attacks[us][from] & (theirs | (position.en_passant_square >= 0
                                                     ? square_bit(position.en_passant_square) : 0));
        while (targets) {
            int target = pop_lsb(targets);
            bool en_passant = target == position.en_passant_square;
            if (target / 8 == 7 || target / 8 == 0)
                for (int piece : {Queen, Rook, Bishop, Knight})
                    moves.push_back({std::uint8_t(from), std::uint8_t(target), std::uint8_t(piece)});
            else
                moves.push_back({std::uint8_t(from), std::uint8_t(target), 0, en_passant});
        }
    }

    auto add_from = [&](Bitboard sources, auto attacker) {
        while (sources) {
            int from = pop_lsb(sources);
            Bitboard targets = attacker(from) & ~mine;
            while (targets) moves.push_back({std::uint8_t(from), std::uint8_t(pop_lsb(targets))});
        }
    };
    add_from(position.pieces[us][Knight], [&](int square) { return knight_attacks[square]; });
    add_from(position.pieces[us][King], [&](int square) { return king_attacks[square]; });
    add_from(position.pieces[us][Bishop], [&](int square) { return bishop_attacks(square, all); });
    add_from(position.pieces[us][Rook], [&](int square) { return rook_attacks(square, all); });
    add_from(position.pieces[us][Queen], [&](int square) {
        return bishop_attacks(square, all) | rook_attacks(square, all);
    });

    // castling: rights, empty squares between, and no square the king crosses may be attacked
    if (position.pieces[us][King]) {
        int king_square = __builtin_ctzll(position.pieces[us][King]);
        std::uint8_t king_side = us == White ? 0x1 : 0x4;
        std::uint8_t queen_side = us == White ? 0x2 : 0x8;
        if ((position.castling & king_side) && !(all & (square_bit(king_square + 1) | square_bit(king_square + 2))))
            if (!is_attacked(position, king_square, them) && !is_attacked(position, king_square + 1, them))
                moves.push_back({std::uint8_t(king_square), std::uint8_t(king_square + 2), 0, false, true});
        if ((position.castling & queen_side) &&
            !(all & (square_bit(king_square - 1) | square_bit(king_square - 2) | square_bit(king_square - 3))))
            if (!is_attacked(position, king_square, them) && !is_attacked(position, king_square - 1, them))
                moves.push_back({std::uint8_t(king_square), std::uint8_t(king_square - 2), 0, false, true});
    }
}

static bool legal(const Position& before, const Move& move) {
    Position after = before;
    Color us = before.side;
    make_move(after, move);
    if (!after.pieces[us][King]) return false;
    int king_square = __builtin_ctzll(after.pieces[us][King]);
    return !is_attacked(after, king_square, Color(1 - us));   // you may not leave your king in check
}

static std::uint64_t perft(Position& position, int depth) {
    if (depth == 0) return 1;
    std::vector<Move> moves;
    moves.reserve(64);
    generate(position, moves);
    std::uint64_t nodes = 0;
    for (const Move& move : moves) {
        if (!legal(position, move)) continue;
        Position child = position;
        make_move(child, move);
        nodes += perft(child, depth - 1);
    }
    return nodes;
}

static Position from_fen(const std::string& fen) {
    Position position;
    int rank = 7, file = 0;
    std::size_t i = 0;
    for (; i < fen.size() && fen[i] != ' '; ++i) {
        char c = fen[i];
        if (c == '/') { --rank; file = 0; continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) { file += c - '0'; continue; }
        Color color = std::isupper(static_cast<unsigned char>(c)) ? White : Black;
        int piece = 0;
        switch (std::tolower(static_cast<unsigned char>(c))) {
            case 'p': piece = Pawn; break;
            case 'n': piece = Knight; break;
            case 'b': piece = Bishop; break;
            case 'r': piece = Rook; break;
            case 'q': piece = Queen; break;
            default: piece = King; break;
        }
        position.pieces[color][piece] |= square_bit(rank * 8 + file);
        ++file;
    }
    while (i < fen.size() && fen[i] == ' ') ++i;
    position.side = (i < fen.size() && fen[i] == 'b') ? Black : White;
    while (i < fen.size() && fen[i] != ' ') ++i;
    while (i < fen.size() && fen[i] == ' ') ++i;
    for (; i < fen.size() && fen[i] != ' '; ++i) {
        if (fen[i] == 'K') position.castling |= 0x1;
        if (fen[i] == 'Q') position.castling |= 0x2;
        if (fen[i] == 'k') position.castling |= 0x4;
        if (fen[i] == 'q') position.castling |= 0x8;
    }
    while (i < fen.size() && fen[i] == ' ') ++i;
    if (i + 1 < fen.size() && fen[i] >= 'a' && fen[i] <= 'h')
        position.en_passant_square = (fen[i + 1] - '1') * 8 + (fen[i] - 'a');
    return position;
}

static const char* kStart = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -";
static const char* kKiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -";
static const char* kPosition3 = "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -";
static const char* kPosition4 = "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq -";

int main(int argc, char** argv) {
    init_tables();
    std::string fen = kStart;
    int max_depth = 4;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--fen" && i + 1 < argc) fen = argv[++i];
        if (arg == "--perft" && i + 1 < argc) max_depth = std::atoi(argv[++i]);
    }

    struct Suite { const char* name; const char* fen; std::vector<std::uint64_t> expected; };
    const std::vector<Suite> suites = {
        {"initial position", kStart, {20, 400, 8902, 197281, 4865609}},
        {"kiwipete", kKiwipete, {48, 2039, 97862, 4085603}},
        {"position 3 (en passant)", kPosition3, {14, 191, 2812, 43238, 674624}},
        {"position 4 (promotions)", kPosition4, {6, 264, 9467, 422333}},
    };

    if (fen != kStart || argc == 1) {
        std::uint64_t total_nodes = 0;
        double total_ms = 0;
        int passed = 0, checked = 0;
        for (const Suite& suite : suites) {
            std::printf("%s\n", suite.name);
            for (std::size_t depth = 1; depth <= suite.expected.size() && int(depth) <= max_depth; ++depth) {
                Position position = from_fen(suite.fen);
                auto start = std::chrono::steady_clock::now();
                std::uint64_t nodes = perft(position, int(depth));
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                bool ok = nodes == suite.expected[depth - 1];
                ++checked;
                passed += ok;
                total_nodes += nodes;
                total_ms += ms;
                std::printf("   depth %zu  %12llu nodes  expected %12llu  %s  %7.0fms  %.1f Mnps\n",
                            depth, (unsigned long long)nodes, (unsigned long long)suite.expected[depth - 1],
                            ok ? "ok" : "MISMATCH", ms, nodes / ms / 1000.0);
            }
        }
        std::printf("\n%d/%d perft counts match published values (%llu nodes in %.1fs)\n",
                    passed, checked, (unsigned long long)total_nodes, total_ms / 1000);
        return passed == checked ? 0 : 1;
    }

    Position position = from_fen(fen);
    for (int depth = 1; depth <= max_depth; ++depth) {
        Position copy = position;
        auto start = std::chrono::steady_clock::now();
        std::uint64_t nodes = perft(copy, depth);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("  depth %d: %llu nodes in %.0fms\n", depth, (unsigned long long)nodes, ms);
    }
    return 0;
}
