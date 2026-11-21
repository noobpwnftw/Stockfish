/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef TBPROBE_H
#define TBPROBE_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Stockfish {
class Position;
class OptionsMap;

using Depth = int;
using Value = int;

namespace Search {
struct RootMove;
struct LimitsType;
using RootMoves = std::vector<RootMove>;
}
}

namespace Stockfish::Tablebases {

struct Config {
    int   cardinality = 0;
    bool  rootInTB    = false;
    bool  useRule50   = false;
    Depth probeDepth  = 0;
#ifdef USE_CHESSTB
    // Least remaining depth for a distance read in search.
    Depth mateProbeDepth = 0;
#endif
};

enum WDLScore {
    WDLLoss        = -2,  // Loss
    WDLBlessedLoss = -1,  // Loss, but draw under 50-move rule
    WDLDraw        = 0,   // Draw
    WDLCursedWin   = 1,   // Win, but draw under 50-move rule
    WDLWin         = 2,   // Win
};

// Possible states after a probing operation
enum ProbeState {
    FAIL = 0,  // Probe failed (missing file table)
    OK   = 1,  // Probe successful
#ifdef USE_SYZYGY
    CHANGE_STM        = -1,  // DTZ should check the other side
    ZEROING_BEST_MOVE = 2    // Best move zeroes DTZ (capture or pawn move)
#endif
};

#if defined(USE_SYZYGY) || defined(USE_CHESSTB)
extern int MaxCardinality;

void     init(const std::string& paths);
WDLScore probe_wdl(Position& pos, ProbeState* result);
Config   rank_root_moves(
    const OptionsMap&            options,
    Position&                    pos,
    Search::RootMoves&           rootMoves,
    bool                         rankDistance = false,
    const std::function<bool()>& time_abort   = []() { return false; });

// Corrects and extends the PV of a root move with a TB score (v), within
// Move Overhead: where the PV is in the tables, their line toward mate. May
// make v exact (a proven mate) or, exceptionally, a draw.
void extend_pv(const OptionsMap&         options,
               const Search::LimitsType& limits,
               Position&                 pos,
               Search::RootMove&         rootMove,
               Value&                    v,
               std::size_t               multiPV);

    #ifdef USE_SYZYGY
int probe_dtz(Position& pos, ProbeState* result);
    #endif

    #ifdef USE_CHESSTB
// DTM, and under the 50-move rule DTM50 at the position's clock: plies to
// mate, positive when the side to move mates; 0 for a draw or no answer. A
// distance read: search gates it.
void probe_dtm(Position& pos, bool rule50, int& dtm, int& dtm50);

// DTZ, and DTC at the position's clock: plies, positive when the side to move
// wins; 0 for a draw or no answer. DTC with the pawn pushes owed before a
// conversion.
void probe_dtc(Position& pos, int& dtz, int& dtc, int& order);

// The decoded-block pool the tables share, in MB (ChessTBCache).
void set_cache_size(size_t mb);
    #endif

#endif

}  // namespace Stockfish::Tablebases

#endif
