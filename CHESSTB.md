# ChessTB in Stockfish (branch `chesstb`)

Stockfish probing ChessTB tables in place of Syzygy. ChessTB's prober and
reader are ported onto Stockfish's own types in `src/chesstb/`, behind the
same `Tablebases::` interface (`src/tbprobe.h`).

## What ChessTB adds

| | Syzygy | ChessTB |
|---|---|---|
| WDL | 5 classes | 5 classes |
| DTZ | rounded in places | exact (a cursed one may read a ply short) |
| Mate | searched for | read: DTM, and DTM50 (exact under any clock) |
| Castling | not probed | castling tables |

With only WDL+DTZ a ChessTB build does what a Syzygy build does, on exact DTZ
and with castling. DTM and DTM50 add the mates.

## How search uses it

- **Root in the tables:** moves rank by DTM50 (50-move rule on) or DTM
  (off), else DTZ, else WDL. A win or loss the clock draws stays just off a
  draw. Ranking by distance, certain wins rank by DTC, as ChessTB's ranker.
- **In the tree:** WDL probes as Syzygy's, castling included. Where WDL
  can't decide a node, the mate distance is read, at remaining depth >=
  `ChessTBMateProbeDepth`.
- **PV:** where it enters the tables, their line to mate, continued over
  outputs within Move Overhead. A proven line under 246 plies scores
  `mate N`; a longer one keeps the TB-win score.

## Build and options

`make chesstb=yes` builds `chesstb/*.cpp` instead of `syzygy/tbprobe.cpp`.
Both probers implement the interface in `src/tbprobe.h`, ChessTB's parts under
`USE_CHESSTB`; a build without a prober declares none of it.

| Option | Default | |
|---|---|---|
| `ChessTBPath` | | directories as `SyzygyPath`; each holds `wdl/` … `dtm50/` and `castling/` |
| `ChessTBProbeDepth`, `ChessTB50MoveRule`, `ChessTBProbeLimit` | as Syzygy's | the Syzygy options under their own names |
| `ChessTBMateProbeDepth` | 20 | least remaining depth for a distance read in search |
| `ChessTBCache` | 64 | MB of decoded blocks shared by all tables |

## Matetrack and speed

`matetrack.epd` (6554 positions), 1M nodes, found / best mates; Stockfish's
`speedtest` (all threads, WDL tables locked in RAM), nodes per second:

| Tables | 5-man mates | 5-man speed | 6-man mates | 6-man speed |
|---|---|---|---|---|
| none | 3873 / 2644 | 21.96M | 3873 / 2644 | 21.96M |
| WDL | 3724 / 2612 | 22.24M (+1.3%) | 3667 / 2608 | 20.70M (−5.7%) |
| WDL+DTZ | 3721 / 2621 | 22.18M (+1.0%) | 3689 / 2636 | 20.98M (−4.4%) |
| all, 50-move rule on (DTM50) | 4117 / 2849 | 22.32M (+1.7%) | 4193 / 3040 | 20.92M (−4.7%) |
| all, 50-move rule off (DTM) | 4110 / 2843 | | 4199 / 3046 | |

With the default options, LZMA-decoding the distance tables shows no
measurable speed loss in `speedtest`; other positions may differ.
