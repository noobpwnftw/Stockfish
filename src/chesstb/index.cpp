#include "index.h"

#include "../attacks.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace Stockfish::ChessTB {

namespace {

constexpr std::array<std::array<uint32_t, 8>, 65> BINOMIAL = []() {
    std::array<std::array<uint32_t, 8>, 65> b{};
    for (size_t k = 0; k <= 64; ++k)
    {
        b[k][0] = 1;
        for (size_t n = 1; n <= 7; ++n)
            b[k][n] = (n > k) ? 0 : b[k - 1][n - 1] + b[k - 1][n];
    }
    return b;
}();

constexpr Square ANCHOR_FILE_MIRROR_SQUARES[] = {
  SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_A3, SQ_B3, SQ_C3,
  SQ_D3, SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_A6, SQ_B6,
  SQ_C6, SQ_D6, SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_A8, SQ_B8, SQ_C8, SQ_D8,
};

constexpr Square ANCHOR_TRIANGLE_SQUARES[] = {
  SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_B2, SQ_C2, SQ_D2, SQ_C3, SQ_D3, SQ_D4,
};

Square apply_transform(Square sq, Symmetry_Transform t) {
    int           f    = file_of(sq);
    int           r    = rank_of(sq);
    const uint8_t bits = static_cast<uint8_t>(t);
    if (bits & 1)
        f = 7 - f;
    if (bits & 2)
        r = 7 - r;
    if (bits & 4)
        std::swap(f, r);
    return make_square(static_cast<File>(f), static_cast<Rank>(r));
}

Square apply_diag_transform(Square sq) { return apply_transform(sq, Symmetry_Transform::DIAG); }

bool sq_on_main_diag(Square sq) {
    return static_cast<int>(file_of(sq)) == static_cast<int>(rank_of(sq));
}

bool kings_adjacent(Square a, Square b) { return bool(Attacks::attacks_bb(KING, a) & b); }

Const_Span<Square> anchor_legal_squares(Symmetry_Group sym) {
    switch (sym)
    {
    case Symmetry_Group::DIHEDRAL_8 :
        return Const_Span<Square>(ANCHOR_TRIANGLE_SQUARES, std::size(ANCHOR_TRIANGLE_SQUARES));
    case Symmetry_Group::FILE_MIRROR :
        return Const_Span<Square>(ANCHOR_FILE_MIRROR_SQUARES,
                                  std::size(ANCHOR_FILE_MIRROR_SQUARES));
    case Symmetry_Group::NONE :
        break;
    }
    assert(false);
    return Const_Span<Square>(ANCHOR_FILE_MIRROR_SQUARES, size_t(0));
}

bool is_anchor_legal(Symmetry_Group sym, Square wk) {
    const auto sqs = anchor_legal_squares(sym);
    for (size_t i = 0; i < sqs.size(); ++i)
        if (sqs[i] == wk)
            return true;
    return false;
}

int num_transforms(Symmetry_Group sym) {
    switch (sym)
    {
    case Symmetry_Group::DIHEDRAL_8 :
        return 8;
    case Symmetry_Group::FILE_MIRROR :
        return 2;
    case Symmetry_Group::NONE :
        break;
    }
    assert(false);
    return 1;
}

const King_Slice_Manager& slice_mgr_for(Symmetry_Group sym) {
    static const King_Slice_Manager file_mirror(Symmetry_Group::FILE_MIRROR);
    static const King_Slice_Manager dihedral_8(Symmetry_Group::DIHEDRAL_8);
    assert(sym != Symmetry_Group::NONE);
    return sym == Symmetry_Group::DIHEDRAL_8 ? dihedral_8 : file_mirror;
}

const King_Slice_Manager& castling_slice_mgr(size_t white_rights, size_t black_rights) {
    constexpr size_t  STRIDE = Castling_Group::MAX_RIGHTS + 1;
    static const auto table  = [] {
        std::array<std::unique_ptr<const King_Slice_Manager>, STRIDE * STRIDE> t;
        for (size_t w = 0; w < STRIDE; ++w)
            for (size_t b = 0; b < STRIDE; ++b)
                if (w + b > 0)
                    t[w * STRIDE + b] =
                      std::make_unique<King_Slice_Manager>(w, b);
        return t;
    }();
    const auto& mgr = table[white_rights * STRIDE + black_rights];
    assert(mgr != nullptr);
    return *mgr;
}

void apply_transform_to_placements(std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements,
                                   const Position_Index_Config&                        cfg,
                                   Symmetry_Transform                                  t) {
    if (t == Symmetry_Transform::IDENTITY)
        return;

    auto xform = [t](Square s) { return apply_transform(s, t); };

    placements[WHITE_KINGS] = placements[WHITE_KINGS].with_transformed_squares(xform);
    placements[BLACK_KINGS] = placements[BLACK_KINGS].with_transformed_squares(xform);

    for (Piece_Class c : {WHITE_PAWNS, BLACK_PAWNS})
        if (placements[c].size() > 0)
            placements[c] = placements[c].with_transformed_squares(xform);

    for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
    {
        const Piece_Class c = cfg.populated_classes()[i];
        placements[c]       = placements[c].with_transformed_squares(xform);
    }
}

void canonicalize_placements(std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements,
                             const Position_Index_Config&                        cfg) {
    assert(!cfg.king_slice_manager().has_castling);

    const Square wk     = placements[WHITE_KINGS][0];
    const Square bk     = placements[BLACK_KINGS][0];
    const auto&  lookup = cfg.king_slice_manager().lookup(wk, bk);

    if (lookup.slice_id == SLICE_NONE)
        return;

    apply_transform_to_placements(placements, cfg, lookup.transform);

    if (lookup.has_diag_stabilizer)
    {
        size_t within_cur = 0;
        size_t within_alt = 0;
        for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
        {
            const Piece_Class c   = cfg.populated_classes()[i];
            const auto&       cur = placements[c];
            const auto        alt = cur.with_transformed_squares(apply_diag_transform);
            within_cur += cfg.weight(c) * cfg.group(c).compound_index(cur);
            within_alt += cfg.weight(c) * cfg.group(c).compound_index(alt);
        }

        if (within_alt < within_cur)
        {
            for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
            {
                const Piece_Class c = cfg.populated_classes()[i];
                placements[c]       = placements[c].with_transformed_squares(apply_diag_transform);
            }
        }
    }
}

std::array<Piece_Group::Placement, PIECE_CLASS_NB>
placements_from_position(const Position_Index_Config& cfg, const View& pos) {
    std::array<Piece_Group::Placement, PIECE_CLASS_NB> out{};

    out[WHITE_KINGS].add(pos.king_square(WHITE));
    out[BLACK_KINGS].add(pos.king_square(BLACK));

    for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
    {
        const Piece_Class c  = cfg.populated_classes()[i];
        const Piece       pc = cfg.group(c).piece();
        Bitboard          b  = pos.piece_bb(pc);
        while (b)
            out[c].add(pop_lsb(b));
    }

    const bool has_pair = cfg.pair_group() != nullptr;
    for (Piece_Class c : {WHITE_PAWNS, BLACK_PAWNS})
    {
        if (!cfg.is_populated(c) && !has_pair)
            continue;
        const Piece pc = (c == WHITE_PAWNS) ? W_PAWN : B_PAWN;
        Bitboard    b  = pos.piece_bb(pc);
        while (b)
            out[c].add(pop_lsb(b));
    }
    return out;
}

}  // namespace

const Pawn_Slice_Manager& pawn_slice_mgr(bool has_pair, size_t white_pawns, size_t black_pawns) {
    constexpr size_t STRIDE = MAX_TOTAL_PAWNS + 1;
    static std::mutex mu;
    static std::array<std::unique_ptr<const Pawn_Slice_Manager>, 2 * STRIDE * STRIDE> table;
    assert(white_pawns < STRIDE && black_pawns < STRIDE);
    std::lock_guard<std::mutex> lk(mu);
    auto& slot = table[(size_t(has_pair) * STRIDE + white_pawns) * STRIDE + black_pawns];
    if (!slot)
        slot = std::make_unique<const Pawn_Slice_Manager>(has_pair, white_pawns, black_pawns);
    return *slot;
}

Piece_Group::Piece_Group(Piece pc, size_t count) :
    m_piece(pc),
    m_num_pieces(count) {
    std::vector<Square> ls;
    const size_t        n = possible_sq_nb(pc);
    ls.reserve(n);
    for (size_t i = 0; i < n; ++i)
        ls.push_back(possible_sq(pc, i));
    std::sort(ls.begin(), ls.end());
    ls.erase(std::unique(ls.begin(), ls.end()), ls.end());
    m_num_legal_squares = ls.size();

    std::fill(std::begin(m_sq_to_pos), std::end(m_sq_to_pos), int8_t(-1));
    for (size_t i = 0; i < m_num_legal_squares; ++i)
    {
        m_pos_to_sq[i]     = ls[i];
        m_sq_to_pos[ls[i]] = static_cast<int8_t>(i);
    }

    m_table_size = BINOMIAL[m_num_legal_squares][count];
}

Piece_Group::Placement_Index Piece_Group::compound_index(const Placement& list) const {
    assert(list.size() == m_num_pieces);
    uint32_t rank = 0;
    for (size_t i = 0; i < m_num_pieces; ++i)
    {
        const int p = m_sq_to_pos[list[i]];
        assert(p >= 0);
        rank += BINOMIAL[static_cast<size_t>(p)][i + 1];
    }
    assert(rank < m_table_size);
    return static_cast<Placement_Index>(rank);
}

Piece_Group::Placement Piece_Group::squares(Placement_Index idx) const {
    assert(idx < m_table_size);

    size_t   pos[MAX_PIECE_GROUP_SIZE]{};
    uint32_t rank = idx;
    size_t   hi   = m_num_legal_squares;
    for (size_t k = m_num_pieces; k > 0; --k)
    {
        size_t p = hi - 1;
        while (BINOMIAL[p][k] > rank)
        {
            assert(p > 0);
            --p;
        }
        pos[k - 1] = p;
        rank -= BINOMIAL[p][k];
        hi = p;
    }

    Placement pl;
    for (size_t i = 0; i < m_num_pieces; ++i)
        pl.add_unsorted(m_pos_to_sq[pos[i]]);
    return pl;
}

// Must enumerate exactly as ChessTB's generator does: the slice ids are stored.
King_Slice_Manager::King_Slice_Manager(size_t white_rights, size_t black_rights) :
    has_castling(true),
    castling_rights{white_rights, black_rights},
    m_castling_group(std::make_shared<Castling_Group>(white_rights, black_rights)) {
    for (auto& e : pair_lookup)
        e = {SLICE_NONE, Symmetry_Transform::IDENTITY, 0};

    m_free_king_stride = m_castling_group->both_kings_pinned() ? size_t(1) : size_t(SQUARE_NB);
    const size_t n     = m_castling_group->table_size();
    castling_slice_lookup.assign(n * m_free_king_stride, SLICE_NONE);

    for (size_t ci = 0; ci < n; ++ci)
    {
        const Castling_Group::Index cidx = static_cast<Castling_Group::Index>(ci);

        Square   pinned_king[COLOR_NB] = {SQ_NONE, SQ_NONE};
        Bitboard pinned                = Bitboard(0);
        for (const Color c : {WHITE, BLACK})
        {
            if (castling_rights[c] == 0)
                continue;
            pinned_king[c] = m_castling_group->king_square(cidx, c);
            pinned |= square_bb(pinned_king[c]);
            for (size_t i = 0; i < castling_rights[c]; ++i)
                pinned |= square_bb(m_castling_group->rook_square(cidx, c, i));
        }

        auto emit = [&](size_t free_slot) {
            castling_slice_lookup[ci * m_free_king_stride + free_slot] =
              static_cast<int32_t>(num_slices++);
        };

        if (m_castling_group->both_kings_pinned())
        {
            emit(0);
            continue;
        }

        const Color  pinned_color = (castling_rights[WHITE] > 0) ? WHITE : BLACK;
        const Square ksq          = pinned_king[pinned_color];
        for (Square fk = SQ_A1; fk < SQ_NONE; fk = static_cast<Square>(fk + 1))
        {
            if ((pinned & square_bb(fk)))
                continue;
            if (kings_adjacent(ksq, fk))
                continue;
            emit(static_cast<size_t>(fk));
        }
    }
}

King_Slice_Manager::King_Slice_Manager(Symmetry_Group sym) {
    for (auto& e : pair_lookup)
        e = {SLICE_NONE, Symmetry_Transform::IDENTITY, 0};

    for (Square wk = SQ_A1; wk < SQ_NONE; wk = static_cast<Square>(wk + 1))
    {
        if (!is_anchor_legal(sym, wk))
            continue;

        for (Square bk = SQ_A1; bk < SQ_NONE; bk = static_cast<Square>(bk + 1))
        {
            if (bk == wk)
                continue;
            if (kings_adjacent(wk, bk))
                continue;

            if (sym == Symmetry_Group::DIHEDRAL_8 && sq_on_main_diag(wk))
            {
                const Square bk_d = sq_diag_mirror(bk);
                if (bk_d != bk && static_cast<int>(bk) > static_cast<int>(bk_d))
                    continue;
            }

            const int32_t sid = static_cast<int32_t>(num_slices++);

            const bool stab =
              (sym == Symmetry_Group::DIHEDRAL_8) && sq_on_main_diag(wk) && sq_on_main_diag(bk);

            pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)] = {
              sid, Symmetry_Transform::IDENTITY, static_cast<uint8_t>(stab)};
        }
    }

    const int n_trans = num_transforms(sym);
    for (Square wk = SQ_A1; wk < SQ_NONE; wk = static_cast<Square>(wk + 1))
    {
        for (Square bk = SQ_A1; bk < SQ_NONE; bk = static_cast<Square>(bk + 1))
        {
            Pair_Lookup& e = pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
            if (e.slice_id != SLICE_NONE)
                continue;
            if (wk == bk || kings_adjacent(wk, bk))
                continue;

            for (int t_id = 0; t_id < n_trans; ++t_id)
            {
                const auto   t    = static_cast<Symmetry_Transform>(t_id);
                const Square wk_t = apply_transform(wk, t);
                const Square bk_t = apply_transform(bk, t);
                const auto&  look =
                  pair_lookup[static_cast<int>(wk_t) * SQUARE_NB + static_cast<int>(bk_t)];
                if (look.slice_id != SLICE_NONE && look.transform == Symmetry_Transform::IDENTITY)
                {
                    e.slice_id            = look.slice_id;
                    e.transform           = t;
                    e.has_diag_stabilizer = look.has_diag_stabilizer;
                    break;
                }
            }
        }
    }
}

Pawn_Slice_Manager::Pawn_Slice_Manager(bool has_pair, size_t white_pawns, size_t black_pawns) {
    if (has_pair)
        m_pair_group = std::make_unique<const Pair_Group>();
    if (white_pawns)
        m_white_group = std::make_unique<const Piece_Group>(W_PAWN, white_pawns);
    if (black_pawns)
        m_black_group = std::make_unique<const Piece_Group>(B_PAWN, black_pawns);
    const Pair_Group*  pair  = m_pair_group.get();
    const Piece_Group* white = m_white_group.get();
    const Piece_Group* black = m_black_group.get();

    m_has_pawns            = pair != nullptr || white != nullptr || black != nullptr;
    m_pair_table_size      = pair ? pair->table_size() : 1;
    m_white_table_size     = white ? white->table_size() : 1;
    m_black_table_size     = black ? black->table_size() : 1;
    m_num_cartesian_slices = m_pair_table_size * m_white_table_size * m_black_table_size;
    if (!pair)
    {
        // Without a pair only overlaps are cut, so the slice is direct
        // (lookup_from_squares): no bitmap.
        m_white_free_size = BINOMIAL[SQ_H7 - SQ_A2 + 1 - black_pawns][white_pawns];
        m_num_slices      = m_black_table_size * m_white_free_size;
        return;
    }

    auto placements_of = [](const Piece_Group* g, std::vector<Piece_Group::Placement>& out_pl,
                            std::vector<Bitboard>& out_occ) {
        if (g == nullptr)
        {
            out_pl.emplace_back();
            out_occ.push_back(Bitboard(0));
            return;
        }
        out_pl.reserve(g->table_size());
        out_occ.reserve(g->table_size());
        for (size_t i = 0; i < g->table_size(); ++i)
        {
            out_pl.push_back(g->squares(static_cast<Piece_Group::Placement_Index>(i)));
            out_occ.push_back(out_pl.back().occupancy());
        }
    };
    std::vector<Piece_Group::Placement> white_pl, black_pl;
    std::vector<Bitboard>               white_occ, black_occ;
    placements_of(white, white_pl, white_occ);
    placements_of(black, black_pl, black_occ);

    // The bitmap of surviving cells, built by bitsets; then each block's count
    // of survivors below it, for lookup_from_squares.
    m_rank_blocks.assign((m_num_cartesian_slices + 63) / 64, Rank_Block{});
    m_num_slices = 0;
    build_pawn_slice_bits(pair, white_pl, black_pl, white_occ, black_occ,
                          [&](size_t cart, uint64_t bits) {
                              if (!bits)
                                  return;
                              m_num_slices += popcount(bits);
                              const size_t word = cart >> 6, shift = cart & 63;
                              m_rank_blocks[word].bits |= bits << shift;
                              if (shift && word + 1 < m_rank_blocks.size())
                                  m_rank_blocks[word + 1].bits |= bits >> (64 - shift);
                          });
    int32_t running = 0;
    for (Rank_Block& block : m_rank_blocks)
    {
        block.survivors_before = running;
        running += static_cast<int32_t>(popcount(block.bits));
    }
    assert(static_cast<size_t>(running) == m_num_slices);
}

int32_t Pawn_Slice_Manager::lookup_from_squares(Square             pair_white_sq,
                                                Square             pair_black_sq,
                                                Const_Span<Square> white_pawn_squares,
                                                Const_Span<Square> black_pawn_squares) const {
    if (!m_has_pawns)
        return 0;

    Piece_Group::Placement white, black;
    for (size_t i = 0; i < white_pawn_squares.size(); ++i)
        white.add(white_pawn_squares[i]);
    for (size_t i = 0; i < black_pawn_squares.size(); ++i)
        black.add(black_pawn_squares[i]);
    const Piece_Group::Placement_Index b_idx =
      m_black_group ? m_black_group->compound_index(black) : 0;

    // Without a pair: black's placement, then white's among the squares black
    // leaves free.
    if (!m_pair_group)
    {
        const Bitboard free     = ~(Rank1BB | Rank8BB) & ~black.occupancy();
        Bitboard       white_bb = white.occupancy();
        size_t         rank = 0;
    #ifdef USE_PEXT
        Bitboard bits = pext(white_bb, free);
        for (size_t k = 1; bits; ++k)
            rank += BINOMIAL[pop_lsb(bits)][k];
    #else
        for (size_t k = 1; white_bb; ++k)
            rank += BINOMIAL[popcount(free & (square_bb(pop_lsb(white_bb)) - 1))][k];
    #endif
        assert(rank < m_white_free_size);
        return static_cast<int32_t>(b_idx * m_white_free_size + rank);
    }

    // With a pair: the cell's rank among the bitmap's survivors.
    const Piece_Group::Placement_Index w_idx =
      m_white_group ? m_white_group->compound_index(white) : 0;
    const size_t cart = static_cast<size_t>(m_pair_group->index_of(pair_white_sq, pair_black_sq))
                      + static_cast<size_t>(w_idx) * m_pair_table_size
                      + static_cast<size_t>(b_idx) * m_pair_table_size * m_white_table_size;
    assert(cart < m_num_cartesian_slices);
    assert(cartesian_survives(cart));
    return storage_of_cartesian(cart);
}

// Tables exist only for materials that fit, so the size checks are asserts.
Position_Index_Config::Position_Index_Config(const Piece_Config& ps) :
    Piece_Config(ps) {
    const auto counts = piece_counts();

    [[maybe_unused]] const size_t total_pawns =
      counts[W_PAWN] + counts[B_PAWN] + (has_opposing_pair() ? 2 : 0);
    assert(total_pawns <= MAX_TOTAL_PAWNS);

    const Symmetry_Group symmetry = pick_symmetry(*this);
    assert(has_castling() == (symmetry == Symmetry_Group::NONE));
    m_king_slice_manager = has_castling()
                           ? &castling_slice_mgr(castling_rights(WHITE), castling_rights(BLACK))
                           : &slice_mgr_for(symmetry);

    auto make_group = [&](Piece_Class pclass, Piece pc, size_t count) {
        m_groups_owned[pclass] = std::make_unique<Piece_Group>(pc, count);
        m_groups[pclass]       = m_groups_owned[pclass].get();
    };

    for (Color c : {WHITE, BLACK})
    {
        for (PieceType pt : {QUEEN, ROOK, BISHOP, KNIGHT, PAWN})
        {
            const Piece  pc = make_piece(c, pt);
            const size_t n  = counts[pc];
            if (n == 0)
                continue;
            const Piece_Type_Class ptcl = pt == QUEEN  ? QUEENS
                                        : pt == ROOK   ? ROOKS
                                        : pt == BISHOP ? BISHOPS
                                        : pt == KNIGHT ? KNIGHTS
                                                       : PAWNS;
            const Piece_Class      pcl  = make_piece_class(c, ptcl);
            make_group(pcl, pc, n);
        }
    }

    if (has_opposing_pair())
        m_pair_group = std::make_unique<Pair_Group>();

    m_pawn_slice_manager = &pawn_slice_mgr(has_opposing_pair(), counts[W_PAWN], counts[B_PAWN]);
    m_num_pawn_slices    = m_pawn_slice_manager->num_slices();

    m_num_populated_classes = 0;
    size_t w                = 1;
    for (Piece_Class i = BLACK_KINGS; i < PIECE_CLASS_NB; ++i)
    {
        if (m_groups[i] == nullptr)
            continue;
        if (i == WHITE_PAWNS || i == BLACK_PAWNS)
            continue;
        m_populated_classes[m_num_populated_classes++] = i;
        m_weights[i]                                   = w;
        w *= m_groups[i]->table_size();
        assert(w <= MAX_NUM_POSITIONS);
    }
    m_within_slice_size = w;

    const size_t num_king_slices = m_king_slice_manager->num_slices;
    m_pawn_slice_stride          = num_king_slices * m_within_slice_size;
    assert(m_num_pawn_slices * m_pawn_slice_stride <= MAX_NUM_POSITIONS);
}

Board_Index board_index_of_position(const Position_Index_Config& cfg,
                                    const Index_Storage_Layout&  layout,
                                    const View&                  pos) {
    auto       placements   = placements_from_position(cfg, pos);
    const bool has_castling = cfg.has_castling();
    if (!has_castling)
        canonicalize_placements(placements, cfg);

    const Square wk = placements[WHITE_KINGS][0];
    const Square bk = placements[BLACK_KINGS][0];

    Square rooks[COLOR_NB][Castling_Group::MAX_RIGHTS];
    size_t num_rooks[COLOR_NB] = {0, 0};
    if (has_castling)
    {
        for (const Color c : {WHITE, BLACK})
        {
            for (const bool h_side : {false, true})
                if (pos.can_castle(c, h_side))
                {
                    if (num_rooks[c] == Castling_Group::MAX_RIGHTS)
                        return BOARD_INDEX_NONE;
                    rooks[c][num_rooks[c]++] = pos.castling_rook_square(c, h_side);
                }
            if (num_rooks[c] == 2 && rooks[c][1] < rooks[c][0])
                std::swap(rooks[c][0], rooks[c][1]);
            if (num_rooks[c] != cfg.castling_rights(c))
                return BOARD_INDEX_NONE;
            if (num_rooks[c] == 0)
                continue;
            const Piece_Class rook_class = make_piece_class(c, ROOKS);
            if (cfg.is_populated(rook_class))
                for (size_t i = 0; i < num_rooks[c]; ++i)
                    placements[rook_class].remove_square(rooks[c][i]);
            else
                placements[rook_class].clear();
        }
    }

    const int32_t king_slice_id = has_castling
                                  ? cfg.king_slice_manager().castling_slice_of(
                                      wk, bk, Const_Span<Square>(rooks[WHITE], num_rooks[WHITE]),
                                      Const_Span<Square>(rooks[BLACK], num_rooks[BLACK]))
                                  : cfg.king_slice_manager().lookup(wk, bk).slice_id;
    if (king_slice_id == SLICE_NONE)
        return BOARD_INDEX_NONE;

    int32_t pawn_slice_id = 0;
    if (cfg.pawn_slice_manager().has_pawns())
    {
        const auto& w_pl = placements[WHITE_PAWNS];
        const auto& b_pl = placements[BLACK_PAWNS];

        Square pair_w = SQ_NONE, pair_b = SQ_NONE;
        Square free_w[Piece_Group::MAX_PIECE_GROUP_SIZE];
        Square free_b[Piece_Group::MAX_PIECE_GROUP_SIZE];
        size_t nfw = 0, nfb = 0;

        if (cfg.pair_group())
        {
            Pair_Group::canonical_pair(Const_Span<Square>(w_pl.begin(), w_pl.size()),
                                       Const_Span<Square>(b_pl.begin(), b_pl.size()), pair_w,
                                       pair_b);
            for (size_t i = 0; i < w_pl.size(); ++i)
                if (w_pl[i] != pair_w)
                    free_w[nfw++] = w_pl[i];
            for (size_t i = 0; i < b_pl.size(); ++i)
                if (b_pl[i] != pair_b)
                    free_b[nfb++] = b_pl[i];
        }
        else
        {
            for (size_t i = 0; i < w_pl.size(); ++i)
                free_w[nfw++] = w_pl[i];
            for (size_t i = 0; i < b_pl.size(); ++i)
                free_b[nfb++] = b_pl[i];
        }

        pawn_slice_id = cfg.pawn_slice_manager().lookup_from_squares(
          pair_w, pair_b, Const_Span<Square>(free_w, nfw), Const_Span<Square>(free_b, nfb));
    }

    // Within the slice, the classes in storage order, the first varying fastest.
    size_t within = 0;
    size_t w      = 1;
    for (size_t i = 0; i < layout.n; ++i)
    {
        const Piece_Class c = layout.order[i];
        within += w * cfg.group(c).compound_index(placements[c]);
        w *= layout.radix[i];
    }
    return static_cast<Board_Index>(static_cast<size_t>(pawn_slice_id) * cfg.pawn_slice_stride()
                                    + static_cast<size_t>(king_slice_id) * cfg.within_slice_size()
                                    + within);
}

bool Piece_Config::sort_pieces(Span<Piece>                         pieces,
                               const std::array<size_t, COLOR_NB>& castling_rights) {
    size_t score[COLOR_NB] = {0, 0};
    for (const Piece p : pieces)
        score[color_of(p)] += PIECE_STRENGTH_FOR_SIDE_ORDER[p];
    for (const Color c : {WHITE, BLACK})
        score[c] += castling_rights[c] * PIECE_STRENGTH_FOR_SIDE_ORDER[make_piece(c, ROOK)];

    bool do_swap = score[BLACK] > score[WHITE];
    if (score[BLACK] == score[WHITE])
    {
        // Counts by type, compared king first (the tables' order, not
        // Stockfish's): 7 - type runs KING..PAWN as 1..6.
        std::array<int8_t, PIECE_TYPE_NB> white_key{}, black_key{};
        for (const Piece p : pieces)
            (color_of(p) == WHITE ? white_key : black_key)[7 - type_of(p)]++;
        white_key[7 - ROOK] += static_cast<int8_t>(castling_rights[WHITE]);
        black_key[7 - ROOK] += static_cast<int8_t>(castling_rights[BLACK]);
        if (white_key != black_key)
            do_swap = black_key > white_key;
        else
            // Same men on both sides, so the only thing left to order by is which
            // side's rook kept its right. Fewer white rights is the canonical
            // orientation, matching Material_Key's castle code -- KRKr, not KrKR.
            do_swap = castling_rights[WHITE] > castling_rights[BLACK];
    }

    if (do_swap)
        for (Piece& p : pieces)
            p = ~p;

    std::sort(pieces.begin(), pieces.end(),
              [](Piece a, Piece b) { return PIECE_ORDER[a] < PIECE_ORDER[b]; });

    return do_swap;
}

}  // namespace Stockfish::ChessTB
