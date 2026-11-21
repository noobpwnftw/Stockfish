#pragma once

#include "common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Stockfish::ChessTB {

// An opposing pawn pair (lowercase 'p'): a white and a black pawn on the same
// file, white below black, neither able to pass or promote while the pair
// stands. This ranks the joint placement, collapsing two free-pawn dimensions
// into one. White ranks 2..6, black 3..7: C(6,2) x 8 files = 120 placements,
// all 8 enumerated because the slice manager folds the mirror on top.
struct Pair_Group {
    using Index = uint32_t;

    Pair_Group() {
        m_inverse.fill(-1);
        // Layout: file-major; within a file, ascending white rank then black rank.
        for (int f = 0; f < 8; ++f)
        {
            const File file = static_cast<File>(f);
            for (int wr = static_cast<int>(RANK_2); wr <= static_cast<int>(RANK_6); ++wr)
                for (int br = wr + 1; br <= static_cast<int>(RANK_7); ++br)
                {
                    const Square w           = make_square(file, static_cast<Rank>(wr));
                    const Square b           = make_square(file, static_cast<Rank>(br));
                    m_inverse[inv_key(w, b)] = static_cast<int32_t>(m_white.size());
                    m_white.push_back(w);
                    m_black.push_back(b);
                }
        }
        assert(m_white.size() == PLACEMENT_COUNT);
    }

    static constexpr size_t PLACEMENT_COUNT = 120;

    size_t table_size() const { return m_white.size(); }

    Square white_square(Index i) const { return m_white[i]; }
    Square black_square(Index i) const { return m_black[i]; }

    // Caller guarantees a legal pair.
    Index index_of(Square white_sq, Square black_sq) const {
        const int32_t idx = m_inverse[inv_key(white_sq, black_sq)];
        assert(idx >= 0);
        return static_cast<Index>(idx);
    }

    // Same file, white below black, both on ranks the enumeration covers.
    static bool is_opposing_pair(Square white_sq, Square black_sq) {
        if (file_of(white_sq) != file_of(black_sq))
            return false;
        const int wr = static_cast<int>(rank_of(white_sq));
        const int br = static_cast<int>(rank_of(black_sq));
        return wr >= static_cast<int>(RANK_2) && br <= static_cast<int>(RANK_7) && wr < br;
    }

    // The pair pawns are indistinguishable from free ones on a board, so the slot
    // is positional: the opposing pair minimal by (file, white_rank, black_rank).
    // False if there is none.
    static bool canonical_pair(Const_Span<Square> white_sqs,
                               Const_Span<Square> black_sqs,
                               Square&            out_white,
                               Square&            out_black) {
        bool found = false;
        for (size_t i = 0; i < white_sqs.size(); ++i)
            for (size_t j = 0; j < black_sqs.size(); ++j)
            {
                const Square w = white_sqs[i], b = black_sqs[j];
                if (!is_opposing_pair(w, b))
                    continue;
                if (!found || less_pair(w, b, out_white, out_black))
                {
                    out_white = w;
                    out_black = b;
                    found     = true;
                }
            }
        return found;
    }

   private:
    static int inv_key(Square w, Square b) {
        return static_cast<int>(w) * 64 + static_cast<int>(b);
    }

    // Lexicographic order on (file, white_rank, black_rank).
    static bool less_pair(Square w1, Square b1, Square w2, Square b2) {
        if (file_of(w1) != file_of(w2))
            return file_of(w1) < file_of(w2);
        if (rank_of(w1) != rank_of(w2))
            return rank_of(w1) < rank_of(w2);
        return rank_of(b1) < rank_of(b2);
    }

    std::vector<Square>          m_white;
    std::vector<Square>          m_black;
    std::array<int32_t, 64 * 64> m_inverse;
};

// Joint placement of the men standing rights pin: each unmoved king and the
// rooks it could still castle with. 56 per rights-holding side, the joint table
// their product. The enumeration order is baked into stored king-slice ids.
struct Castling_Group {
    using Index                        = int32_t;
    static constexpr Index  INDEX_NONE = -1;
    static constexpr size_t MAX_RIGHTS = 2;

    // One side's pinned men: its king's file, and its castling rooks' files
    // ascending. Only the first `rights(c)` rook entries are meaningful.
    struct Side_Placement {
        File                         king = FILE_NB;
        std::array<File, MAX_RIGHTS> rook{FILE_NB, FILE_NB};
    };

    Castling_Group(size_t white_rights, size_t black_rights) :
        m_rights{white_rights, black_rights} {
        assert(white_rights <= MAX_RIGHTS && black_rights <= MAX_RIGHTS);
        assert(white_rights + black_rights > 0);
        for (const Color c : {WHITE, BLACK})
            m_side[c].build(m_rights[c]);
    }

    size_t rights(Color c) const { return m_rights[c]; }
    bool   holds_rights(Color c) const { return m_rights[c] > 0; }
    bool   both_kings_pinned() const { return m_rights[WHITE] > 0 && m_rights[BLACK] > 0; }

    size_t table_size() const {
        return std::max<size_t>(m_side[WHITE].size(), 1)
             * std::max<size_t>(m_side[BLACK].size(), 1);
    }

    // Index order: white's placement outermost, black's within it. Nothing
    // depends on the order beyond it being stable -- but it must stay stable,
    // since stored king-slice ids are built from it.
    const Side_Placement& side(Index idx, Color c) const {
        assert(idx >= 0 && static_cast<size_t>(idx) < table_size());
        assert(holds_rights(c));
        const size_t b_span = std::max<size_t>(m_side[BLACK].size(), 1);
        const size_t within =
          (c == WHITE) ? static_cast<size_t>(idx) / b_span : static_cast<size_t>(idx) % b_span;
        return m_side[c].placements[within];
    }

    Index index_of(File             white_king,
                   Const_Span<File> white_rooks,
                   File             black_king,
                   Const_Span<File> black_rooks) const {
        const File              king[COLOR_NB]  = {white_king, black_king};
        const Const_Span<File>* rooks[COLOR_NB] = {&white_rooks, &black_rooks};

        size_t within[COLOR_NB] = {0, 0};
        for (const Color c : {WHITE, BLACK})
        {
            if (m_rights[c] == 0)
                continue;
            if (rooks[c]->size() != m_rights[c])
                return INDEX_NONE;
            const int32_t i = m_side[c].index_of(make_side_placement(king[c], *rooks[c]));
            if (i < 0)
                return INDEX_NONE;
            within[c] = static_cast<size_t>(i);
        }
        const size_t b_span = std::max<size_t>(m_side[BLACK].size(), 1);
        return static_cast<Index>(within[WHITE] * b_span + within[BLACK]);
    }

    static Side_Placement make_side_placement(File king_file, Const_Span<File> rook_files) {
        Side_Placement p;
        p.king = king_file;
        for (size_t i = 0; i < rook_files.size() && i < MAX_RIGHTS; ++i)
            p.rook[i] = rook_files[i];
        if (p.rook[1] != FILE_NB && p.rook[1] < p.rook[0])
            std::swap(p.rook[0], p.rook[1]);
        return p;
    }

    Square king_square(Index idx, Color c) const {
        return make_square(side(idx, c).king, relative_rank(c, RANK_1));
    }

    Square rook_square(Index idx, Color c, size_t which) const {
        assert(which < m_rights[c]);
        return make_square(side(idx, c).rook[which], relative_rank(c, RANK_1));
    }

    // Kingside in the FEN sense: this rook stands toward the h-file.
    bool h_side(Index idx, Color c, size_t which) const {
        const Side_Placement& p = side(idx, c);
        return p.rook[which] > p.king;
    }

   private:
    // One side's placements, enumerated king file first then rook files, all
    // ascending. Empty for a side holding no rights.
    struct Side_Table {
        std::vector<Side_Placement>    placements;
        std::array<int32_t, 9 * 9 * 9> inverse{};

        size_t size() const { return placements.size(); }

        void build(size_t num_rights) {
            inverse.fill(INDEX_NONE);
            if (num_rights == 0)
                return;

            for (File k = FILE_A; k < FILE_NB; ++k)
            {
                if (num_rights == 1)
                {
                    for (File r = FILE_A; r < FILE_NB; ++r)
                        if (r != k)
                            add({k, {r, FILE_NB}});
                }
                else
                {
                    for (File lo = FILE_A; lo < k; ++lo)
                        for (File hi = static_cast<File>(k + 1); hi < FILE_NB; ++hi)
                            add({k, {lo, hi}});
                }
            }
        }

        int32_t index_of(const Side_Placement& p) const { return inverse[key(p)]; }

       private:
        // (king, rook, rook) packed base-9 over FILE_A..FILE_NB. FILE_NB is the
        // absent second rook of a one-right side, and keys a slot build never
        // fills, so an unplaced man reads back as INDEX_NONE.
        static size_t key(const Side_Placement& p) {
            auto f = [](File x) {
                assert(x >= FILE_A && x <= FILE_NB);
                return static_cast<size_t>(x == FILE_NB ? 8 : x);
            };
            return (f(p.king) * 9 + f(p.rook[0])) * 9 + f(p.rook[1]);
        }

        void add(const Side_Placement& p) {
            inverse[key(p)] = static_cast<int32_t>(placements.size());
            placements.push_back(p);
        }
    };

    std::array<size_t, COLOR_NB> m_rights;
    Side_Table                   m_side[COLOR_NB];
};

// One class per piece type per color.
enum Piece_Type_Class : int8_t {
    KINGS,
    KNIGHTS,
    BISHOPS,
    ROOKS,
    QUEENS,
    PAWNS,
    PIECE_TYPE_CLASS_NB
};

enum Piece_Class : int8_t {
    BLACK_KINGS,
    BLACK_KNIGHTS,
    BLACK_BISHOPS,
    BLACK_ROOKS,
    BLACK_QUEENS,
    BLACK_PAWNS,
    WHITE_KINGS,
    WHITE_KNIGHTS,
    WHITE_BISHOPS,
    WHITE_ROOKS,
    WHITE_QUEENS,
    WHITE_PAWNS,
    PIECE_CLASS_NB
};

constexpr Piece_Class& operator++(Piece_Class& p_class) {
    p_class = static_cast<Piece_Class>(static_cast<int>(p_class) + 1);
    return p_class;
}

constexpr Piece_Class make_piece_class(Color color, Piece_Type_Class pt_class) {
    assert(pt_class < PIECE_TYPE_CLASS_NB);
    return static_cast<Piece_Class>((color == WHITE ? WHITE_KINGS : BLACK_KINGS) + pt_class);
}

struct Piece_Config {
    // Uppercase letters are ordinary piece types (color set by side ordering).
    // Lowercase 'p' is an opposing pawn pair: one white + one black pawn
    // locked head-to-head, indexed jointly. At most one 'p' per material.
    // Lowercase 'r' is a rook that still holds a castling right, together with
    // the king it castles with: KrK is physically KRK, with the right indexed
    // rather than lost. Two rights per side at most.

    static constexpr std::array<int16_t, PIECE_NB> PIECE_STRENGTH_FOR_SIDE_ORDER = []() {
        std::array<int16_t, PIECE_NB> arr{};
        // Kings present on both sides — value irrelevant for side ordering.
        arr[W_KING] = arr[B_KING] = 0;
        arr[W_QUEEN] = arr[B_QUEEN] = 900;
        arr[W_ROOK] = arr[B_ROOK] = 500;
        arr[W_BISHOP] = arr[B_BISHOP] = 330;
        arr[W_KNIGHT] = arr[B_KNIGHT] = 320;
        arr[W_PAWN] = arr[B_PAWN] = 100;
        return arr;
    }();

    // Sort order within a side: K, Q, R, B, N, P (descending strength, kings first).
    static constexpr std::array<int8_t, PIECE_NB> PIECE_ORDER = []() {
        std::array<int8_t, PIECE_NB> ret{};
        int8_t                       i           = 1;
        constexpr Color              AllColors[] = {WHITE, BLACK};
        for (const Color color : AllColors)
        {
            ret[make_piece(color, KING)]   = i++;
            ret[make_piece(color, QUEEN)]  = i++;
            ret[make_piece(color, ROOK)]   = i++;
            ret[make_piece(color, BISHOP)] = i++;
            ret[make_piece(color, KNIGHT)] = i++;
            ret[make_piece(color, PAWN)]   = i++;
        }
        return ret;
    }();

    static bool sort_pieces(Span<Piece>                         pieces,
                            const std::array<size_t, COLOR_NB>& castling_rights = {0, 0});

    static bool is_constructible_from(Const_Span<Piece> pieces) {
        if (pieces.size() < 2 || pieces.size() > MAX_MAN)
            return false;

        if (std::count(pieces.begin(), pieces.end(), W_KING) != 1)
            return false;

        if (std::count(pieces.begin(), pieces.end(), B_KING) != 1)
            return false;

        return true;
    }

    using Castling_Rights_Counts = std::array<size_t, COLOR_NB>;
    static constexpr Castling_Rights_Counts NO_CASTLE_RIGHTS{0, 0};

    Piece_Config(Const_Span<Piece>             pcs,
                 const Castling_Rights_Counts& castling_rights = NO_CASTLE_RIGHTS) :
        m_num_pieces(0) {
        assert(is_constructible_from(pcs));

        std::memcpy(m_pieces, pcs.data(), pcs.size() * sizeof(Piece));

        const bool swapped = sort_pieces(Span(m_pieces, pcs.size()), castling_rights);

        m_castling_rights = castling_rights;
        if (swapped)
            std::swap(m_castling_rights[WHITE], m_castling_rights[BLACK]);

        for (const Piece p : Const_Span(m_pieces, m_pieces + pcs.size()))
        {
            m_base_mat_key.add_piece(p);
            m_mirr_mat_key.add_piece(~p);
        }
        if (has_castling())
        {
            m_base_mat_key.add_castling(m_castling_rights[WHITE], m_castling_rights[BLACK]);
            m_mirr_mat_key.add_castling(m_castling_rights[BLACK], m_castling_rights[WHITE]);
        }
        m_num_pieces = pcs.size();
    }

    auto pieces() const { return Const_Span(m_pieces, m_pieces + m_num_pieces); }

    std::string name() const {
        std::string s;
        bool        seen_white_king = false;
        // Castling rooks are not in m_pieces, so they go in where they would have
        // sorted: before the first man of their color that outranks a rook.
        std::array<size_t, COLOR_NB> written{0, 0};
        auto                         write_castling_rooks_before = [&](int8_t order) {
            for (const Color c : {WHITE, BLACK})
            {
                if (written[c] == m_castling_rights[c])
                    continue;
                if (PIECE_ORDER[make_piece(c, ROOK)] >= order)
                    continue;
                s.append(m_castling_rights[c] - written[c], 'r');
                written[c] = m_castling_rights[c];
            }
        };
        for (size_t i = 0; i < m_num_pieces; ++i)
        {
            const Piece     p  = m_pieces[i];
            const PieceType pt = type_of(p);
            write_castling_rooks_before(PIECE_ORDER[p]);
            if (pt == KING)
            {
                if (seen_white_king && m_has_opposing_pair)
                    s += 'p';
                seen_white_king = true;
            }
            s += piece_type_char(pt);
        }
        write_castling_rooks_before(std::numeric_limits<int8_t>::max());
        if (m_has_opposing_pair)
            s += 'p';
        return s;
    }

    const std::array<size_t, PIECE_NB> piece_counts() const {
        std::array<size_t, PIECE_NB> counts;
        std::fill(counts.begin(), counts.end(), 0);
        for (const Piece piece : pieces())
            counts[piece] += 1;
        return counts;
    }

    bool has_opposing_pair() const { return m_has_opposing_pair; }

    bool   has_castling() const { return m_castling_rights[WHITE] + m_castling_rights[BLACK] > 0; }
    size_t castling_rights(Color c) const { return m_castling_rights[c]; }
    const Castling_Rights_Counts& castling_rights() const { return m_castling_rights; }

    bool has_pawns() const {
        if (m_has_opposing_pair)
            return true;
        for (const Piece p : pieces())
            if (type_of(p) == PAWN)
                return true;
        return false;
    }

    Material_Key base_material_key() const { return m_base_mat_key; }

    std::pair<Material_Key, Material_Key> material_keys() const {
        return {m_base_mat_key, m_mirr_mat_key};
    }

    Material_Key min_material_key() const { return std::min(m_base_mat_key, m_mirr_mat_key); }

    void mark_opposing_pair() {
        m_has_opposing_pair = true;
        m_base_mat_key.add_pair();
        m_mirr_mat_key.add_pair();
    }

   private:
    Piece  m_pieces[MAX_MAN];
    size_t m_num_pieces;
    bool   m_has_opposing_pair = false;
    // Castling rights per color, whose rooks are not in m_pieces.
    Castling_Rights_Counts m_castling_rights{0, 0};
    Material_Key           m_base_mat_key;
    Material_Key           m_mirr_mat_key;
};

// Permutation index over the populated non-pawn classes, stored as a bare uint32
// (perm 0 == native order). Validity is just the FACTORIAL range bound.
inline constexpr std::array<uint32_t, 9> FACTORIAL = {1u,   1u,   2u,    6u,    24u,
                                                      120u, 720u, 5040u, 40320u};

template<typename Config>
bool index_permutation_config_is_valid(const Config& cfg, uint32_t perm) {
    const size_t n = cfg.num_populated_classes();
    return n <= 8 && perm < FACTORIAL[n];
}

template<typename Config>
std::array<Piece_Class, PIECE_CLASS_NB> storage_within_class_order(const Config& cfg,
                                                                   uint32_t      perm) {
    assert(index_permutation_config_is_valid(cfg, perm));
    const size_t n = cfg.num_populated_classes();

    std::array<Piece_Class, PIECE_CLASS_NB> available{};
    for (size_t i = 0; i < n; ++i)
        available[i] = cfg.populated_classes()[i];

    std::array<Piece_Class, PIECE_CLASS_NB> order{};
    uint32_t                                idx = perm;
    for (size_t i = 0; i < n; ++i)
    {
        const uint32_t f    = FACTORIAL[n - 1 - i];
        const size_t   pick = idx / f;
        idx %= f;
        assert(pick < n - i);
        order[i] = available[pick];
        for (size_t j = pick; j + 1 < n - i; ++j)
            available[j] = available[j + 1];
    }
    return order;
}

// Emit survivor bits in the existing cartesian order: pair, white, black.
// Each call supplies up to 64 bits starting at cart; adjacent calls may
// share a destination word when a placement radix is not word-aligned.
// ChessTB's generator and prober share this with their own placement types.
template<typename Placement, typename Emit>
void build_pawn_slice_bits(const Pair_Group*             pair,
                           const std::vector<Placement>& white,
                           const std::vector<Placement>& black,
                           const std::vector<Bitboard>&  white_occ,
                           const std::vector<Bitboard>&  black_occ,
                           Emit&&                        emit) {
    if (pair)
    {
        using Mask = std::array<uint64_t, 2>;
        static_assert(Pair_Group::PLACEMENT_COUNT == 120);
        auto prefix = [](size_t n) -> Mask {
            return {n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1,
                    n <= 64 ? 0 : (uint64_t{1} << (n - 64)) - 1};
        };
        const Mask                  all = prefix(pair->table_size());
        std::array<Mask, SQUARE_NB> white_allowed{}, black_allowed{};
        for (size_t p = 0; p < pair->table_size(); ++p)
        {
            const auto   pi = static_cast<Pair_Group::Index>(p);
            const Square pw = pair->white_square(pi), pb = pair->black_square(pi);
            for (Square sq = SQ_A2; sq <= SQ_H7; sq = static_cast<Square>(sq + 1))
            {
                if (sq == pw || sq == pb)
                    continue;
                Square ws[] = {pw, sq}, bs[] = {pb, sq};
                Square cw = SQ_NONE, cb = SQ_NONE;
                Pair_Group::canonical_pair(Const_Span<Square>(ws, 2), Const_Span<Square>(bs, 1), cw,
                                           cb);
                if (cw == pw && cb == pb)
                    white_allowed[sq][p >> 6] |= uint64_t{1} << (p & 63);
                Pair_Group::canonical_pair(Const_Span<Square>(ws, 1), Const_Span<Square>(bs, 2), cw,
                                           cb);
                if (cw == pw && cb == pb)
                    black_allowed[sq][p >> 6] |= uint64_t{1} << (p & 63);
            }
        }
        auto candidates = [&](const std::vector<Placement>&      placements,
                              const std::array<Mask, SQUARE_NB>& allowed) {
            std::vector<Mask> result(placements.size(), all);
            for (size_t i = 0; i < placements.size(); ++i)
                for (size_t j = 0; j < placements[i].size(); ++j)
                    for (size_t word = 0; word < 2; ++word)
                        result[i][word] &= allowed[placements[i][j]][word];
            return result;
        };
        const auto white_candidates = candidates(white, white_allowed);
        const auto black_candidates = candidates(black, black_allowed);
        size_t     cart             = 0;
        for (size_t b = 0; b < black.size(); ++b)
        {
            // Pair indices follow canonical ordering. The designated pair
            // must precede every pair among the additional pawns as well.
            std::array<Mask, SQUARE_NB> before;
            before.fill(all);
            for (Square ws = SQ_A2; ws <= SQ_H7; ws = static_cast<Square>(ws + 1))
            {
                size_t first = pair->table_size();
                for (size_t j = 0; j < black[b].size(); ++j)
                    if (Pair_Group::is_opposing_pair(ws, black[b][j]))
                        first =
                          std::min(first, static_cast<size_t>(pair->index_of(ws, black[b][j])));
                before[ws] = prefix(first);
            }
            for (size_t w = 0; w < white.size(); ++w, cart += pair->table_size())
            {
                if (white_occ[w] & black_occ[b])
                    continue;
                Mask survivors;
                for (size_t word = 0; word < 2; ++word)
                {
                    survivors[word] = white_candidates[w][word] & black_candidates[b][word];
                    for (size_t j = 0; j < white[w].size(); ++j)
                        survivors[word] &= before[white[w][j]][word];
                }
                emit(cart, survivors[0]);
                emit(cart + 64, survivors[1]);
            }
        }
    }
    else
    {
        // Each black placement excludes every white placement touching
        // any of its squares. Compute a whole word of placements at once.
        const size_t                                 words = (white.size() + 63) / 64;
        std::array<std::vector<uint64_t>, SQUARE_NB> occupied;
        for (auto& bits : occupied)
            bits.resize(words);
        for (size_t w = 0; w < white.size(); ++w)
            for (size_t j = 0; j < white[w].size(); ++j)
                occupied[white[w][j]][w >> 6] |= uint64_t{1} << (w & 63);
        const size_t tail = white.size() & 63;
        for (size_t b = 0; b < black.size(); ++b)
            for (size_t word = 0; word < words; ++word)
            {
                uint64_t blocked = 0;
                for (size_t j = 0; j < black[b].size(); ++j)
                    blocked |= occupied[black[b][j]][word];
                uint64_t survivors = ~blocked;
                if (word + 1 == words && tail)
                    survivors &= (uint64_t{1} << tail) - 1;
                emit(b * white.size() + word * 64, survivors);
            }
    }
}

enum Board_Index : size_t {
    BOARD_INDEX_ZERO = 0,
    BOARD_INDEX_NONE = std::numeric_limits<size_t>::max()
};

enum struct Symmetry_Group : uint8_t {
    NONE,
    FILE_MIRROR,
    DIHEDRAL_8,
};

enum struct Symmetry_Transform : uint8_t {
    IDENTITY       = 0,
    FILE           = 1,
    RANK           = 2,
    FILE_RANK      = 3,
    DIAG           = 4,
    FILE_DIAG      = 5,
    RANK_DIAG      = 6,
    FILE_RANK_DIAG = 7
};

struct Piece_Group {
    static constexpr size_t MAX_PIECE_GROUP_SIZE = 7;

    using Placement_Index = uint32_t;

    struct alignas(8) Placement {
        Placement() :
            m_size(0) {}

        template<typename F>
        Placement with_transformed_squares(F&& f) const {
            Placement dst;
            dst.m_size = m_size;
            for (size_t i = 0; i < static_cast<size_t>(m_size); ++i)
                dst.m_squares[i] = f(m_squares[i]);
            dst.sort();
            return dst;
        }

        inline void add(Square s) {
            assert(static_cast<size_t>(m_size) < MAX_PIECE_GROUP_SIZE);
            size_t i = m_size;
            while (i > 0 && m_squares[i - 1] > s)
            {
                m_squares[i] = m_squares[i - 1];
                --i;
            }
            m_squares[i] = s;
            m_size += 1;
        }

        inline void add_unsorted(Square s) {
            assert(static_cast<size_t>(m_size) < MAX_PIECE_GROUP_SIZE);
            m_squares[m_size++] = s;
        }

        inline void clear() { m_size = 0; }

        // Lift one square out, keeping the rest in order: how a castling rook
        // leaves its color's rook class, which indexes only the free ones. A
        // square that is not here is left alone rather than driving m_size
        // negative -- as ChessTB's generator does.
        inline void remove_square(Square s) {
            const size_t n = static_cast<size_t>(m_size);
            size_t       i = 0;
            while (i < n && m_squares[i] != s)
                ++i;
            if (i == n)
                return;
            for (; i + 1 < n; ++i)
                m_squares[i] = m_squares[i + 1];
            m_size -= 1;
        }

        inline void sort() {
            for (size_t i = 1; i < static_cast<size_t>(m_size); ++i)
            {
                const Square v = m_squares[i];
                size_t       j = i;
                while (j > 0 && m_squares[j - 1] > v)
                {
                    m_squares[j] = m_squares[j - 1];
                    --j;
                }
                m_squares[j] = v;
            }
        }

        inline Square operator[](size_t i) const {
            assert(i < static_cast<size_t>(m_size));
            return m_squares[i];
        }

        inline size_t        size() const { return m_size; }
        inline Bitboard      occupancy() const {
            Bitboard bb = 0;
            for (int8_t i = 0; i < m_size; ++i)
                bb |= square_bb(m_squares[i]);
            return bb;
        }
        inline const Square* begin() const { return m_squares; }

       private:
        Square m_squares[MAX_PIECE_GROUP_SIZE];
        int8_t m_size;
    };

    Piece_Group(Piece pc, size_t count);

    Placement_Index compound_index(const Placement& list) const;
    Placement       squares(Placement_Index idx) const;

    size_t table_size() const { return m_table_size; }
    size_t size() const { return m_num_pieces; }
    Piece  piece() const { return m_piece; }

   private:
    Piece    m_piece;
    size_t   m_num_pieces;
    size_t   m_num_legal_squares;
    uint32_t m_table_size;

    int8_t m_sq_to_pos[SQUARE_NB];
    Square m_pos_to_sq[SQUARE_NB];
};

inline constexpr int32_t SLICE_NONE = -1;

struct King_Slice_Manager {
    size_t num_slices = 0;

    struct Pair_Lookup {
        int32_t            slice_id;
        Symmetry_Transform transform;
        uint8_t            has_diag_stabilizer;
    };

    std::array<Pair_Lookup, SQUARE_NB * SQUARE_NB> pair_lookup{};

    // Castling managers only. Must enumerate exactly as ChessTB's generator --
    // the slice ids are on disk. Both sides share Castling_Group for that reason,
    // so only this loop is duplicated, not the placement order.
    bool                         has_castling = false;
    std::array<size_t, COLOR_NB> castling_rights{0, 0};
    std::vector<int32_t>         castling_slice_lookup;

    explicit King_Slice_Manager(Symmetry_Group s);
    // Castling: no symmetry.
    King_Slice_Manager(size_t white_rights, size_t black_rights);

    bool both_kings_pinned() const { return m_castling_group->both_kings_pinned(); }

    const Pair_Lookup& lookup(Square wk, Square bk) const {
        assert(!has_castling);
        return pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
    }

    int32_t castling_slice_of(Square             wk,
                              Square             bk,
                              Const_Span<Square> white_rooks,
                              Const_Span<Square> black_rooks) const {
        assert(has_castling);
        const Square              king_sq[COLOR_NB] = {wk, bk};
        const Const_Span<Square>* rooks[COLOR_NB]   = {&white_rooks, &black_rooks};

        File rook_files[COLOR_NB][Castling_Group::MAX_RIGHTS];
        File king_file[COLOR_NB] = {FILE_NB, FILE_NB};
        for (const Color c : {WHITE, BLACK})
        {
            if (rooks[c]->size() != castling_rights[c])
                return SLICE_NONE;
            if (castling_rights[c] == 0)
                continue;
            const Rank home = relative_rank(c, RANK_1);
            if (rank_of(king_sq[c]) != home)
                return SLICE_NONE;
            king_file[c] = file_of(king_sq[c]);
            for (size_t i = 0; i < rooks[c]->size(); ++i)
            {
                if (rank_of((*rooks[c])[i]) != home)
                    return SLICE_NONE;
                rook_files[c][i] = file_of((*rooks[c])[i]);
            }
        }

        const Castling_Group::Index ci = m_castling_group->index_of(
          king_file[WHITE], Const_Span<File>(rook_files[WHITE], castling_rights[WHITE]),
          king_file[BLACK], Const_Span<File>(rook_files[BLACK], castling_rights[BLACK]));
        if (ci == Castling_Group::INDEX_NONE)
            return SLICE_NONE;

        const size_t free_slot =
          both_kings_pinned()
            ? size_t(0)
            : static_cast<size_t>(king_sq[castling_rights[WHITE] == 0 ? WHITE : BLACK]);
        return castling_slice_lookup[static_cast<size_t>(ci) * m_free_king_stride + free_slot];
    }

   private:
    std::shared_ptr<Castling_Group> m_castling_group;
    size_t                          m_free_king_stride = 1;
};

inline constexpr size_t MAX_TOTAL_PAWNS = 6;

// Depends on the pawns only: the opposing pair and each side's other pawns.
struct Pawn_Slice_Manager {
    Pawn_Slice_Manager(bool has_pair, size_t white_pawns, size_t black_pawns);

    Pawn_Slice_Manager(const Pawn_Slice_Manager&)            = delete;
    Pawn_Slice_Manager& operator=(const Pawn_Slice_Manager&) = delete;

    size_t num_slices() const { return m_num_slices; }
    bool   has_pawns() const { return m_has_pawns; }

    int32_t lookup_from_squares(Square             pair_white_sq,
                                Square             pair_black_sq,
                                Const_Span<Square> white_pawn_squares,
                                Const_Span<Square> black_pawn_squares) const;

   private:
    struct Rank_Block {
        int32_t  survivors_before = 0;
        uint64_t bits             = 0;
    };

    // Storage id of a cartesian cell: the number of surviving cells below it.
    int32_t storage_of_cartesian(size_t cart) const {
        const Rank_Block& blk   = m_rank_blocks[cart >> 6];
        const uint64_t    below = (uint64_t{1} << (cart & 63)) - 1;
        return blk.survivors_before + static_cast<int32_t>(popcount(blk.bits & below));
    }
    bool cartesian_survives(size_t cart) const {
        return (m_rank_blocks[cart >> 6].bits & (uint64_t{1} << (cart & 63))) != 0;
    }

    bool   m_has_pawns            = false;
    size_t m_num_slices           = 1;
    size_t m_num_cartesian_slices = 1;
    size_t m_pair_table_size      = 1;
    size_t m_white_table_size     = 1;
    size_t m_black_table_size     = 1;
    size_t m_white_free_size      = 1;  // without a pair: white's placements per black one

    std::vector<Rank_Block> m_rank_blocks;

    std::unique_ptr<const Pair_Group>  m_pair_group;
    std::unique_ptr<const Piece_Group> m_white_group;
    std::unique_ptr<const Piece_Group> m_black_group;
};

// The manager for these pawns, built once and shared by every material that
// has them.
const Pawn_Slice_Manager& pawn_slice_mgr(bool has_pair, size_t white_pawns, size_t black_pawns);

struct Index_Storage_Layout {
    size_t                                  n = 0;
    std::array<Piece_Class, PIECE_CLASS_NB> order{};
    std::array<size_t, PIECE_CLASS_NB>      radix{};
};

template<typename Config>
Index_Storage_Layout make_index_storage_layout(const Config& cfg, uint32_t perm) {
    assert(index_permutation_config_is_valid(cfg, perm));

    Index_Storage_Layout layout;
    layout.n         = cfg.num_populated_classes();
    const auto order = storage_within_class_order(cfg, perm);
    for (size_t i = 0; i < layout.n; ++i)
    {
        layout.order[i] = order[i];
        layout.radix[i] = cfg.group(order[i]).table_size();
    }
    return layout;
}

struct Position_Index_Config: public Piece_Config {
   private:
    static constexpr size_t MAX_NUM_POSITIONS = 0xffffffffffffull;

    static Symmetry_Group pick_symmetry(const Piece_Config& ps) {
        if (ps.has_castling())
            return Symmetry_Group::NONE;
        const auto counts    = ps.piece_counts();
        const bool has_pawns = counts[W_PAWN] > 0 || counts[B_PAWN] > 0 || ps.has_opposing_pair();
        return has_pawns ? Symmetry_Group::FILE_MIRROR : Symmetry_Group::DIHEDRAL_8;
    }

   public:
    explicit Position_Index_Config(const Piece_Config& ps);

    Position_Index_Config(const Position_Index_Config&)            = delete;
    Position_Index_Config& operator=(const Position_Index_Config&) = delete;
    Position_Index_Config(Position_Index_Config&&)                 = default;
    Position_Index_Config& operator=(Position_Index_Config&&)      = default;

    const King_Slice_Manager& king_slice_manager() const { return *m_king_slice_manager; }
    const Pawn_Slice_Manager& pawn_slice_manager() const { return *m_pawn_slice_manager; }
    const Pair_Group*         pair_group() const { return m_pair_group.get(); }

    const Piece_Group& group(Piece_Class c) const {
        assert(m_groups[c] != nullptr);
        return *m_groups[c];
    }

    bool               is_populated(Piece_Class c) const { return m_groups[c] != nullptr; }
    const Piece_Class* populated_classes() const { return m_populated_classes; }
    size_t             num_populated_classes() const { return m_num_populated_classes; }
    size_t             weight(Piece_Class c) const { return m_weights[c]; }

    size_t pawn_slice_stride() const { return m_pawn_slice_stride; }
    size_t within_slice_size() const { return m_within_slice_size; }

   private:
    const King_Slice_Manager*           m_king_slice_manager = nullptr;
    const Pawn_Slice_Manager*           m_pawn_slice_manager = nullptr;
    std::unique_ptr<Pair_Group>         m_pair_group;  // null unless this material has 'p'

    size_t m_num_pawn_slices   = 1;
    size_t m_pawn_slice_stride = 0;
    size_t m_within_slice_size = 0;

    size_t      m_num_populated_classes = 0;
    Piece_Class m_populated_classes[PIECE_CLASS_NB]{};

    std::unique_ptr<Piece_Group> m_groups_owned[PIECE_CLASS_NB];
    const Piece_Group*           m_groups[PIECE_CLASS_NB]{};
    size_t                       m_weights[PIECE_CLASS_NB]{};
};

Board_Index board_index_of_position(const Position_Index_Config& cfg,
                                    const Index_Storage_Layout&  layout,
                                    const View&                  pos);

}  // namespace Stockfish::ChessTB
