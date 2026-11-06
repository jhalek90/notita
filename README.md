# notita

A 2D pixel sandbox where every pixel is simulated. "notita" is the repo name;
the game itself still needs its own name (the window says "working title").

Phase 1 (this) is the engine core: a chunked, multithreaded falling-sand
simulation, data-driven materials, a GPU-presented 640x360 view and sandbox
brushes. The full plan lives in Corvid note N249.

## Build (Windows, MSVC)

Needs Visual Studio 2022 or newer with the C++ workload, and CMake 3.24+.
SDL3 3.4.20 and Dear ImGui 1.93.0 are downloaded by CMake FetchContent on the
first configure (hash-pinned).

```bat
build.bat
```

`build.bat` sets up the MSVC environment itself, uses Ninja if it is on PATH
and NMake otherwise, and builds Release into `build\`. Set
`NOTITA_BUILD_DIR` to build elsewhere. The first build compiles SDL3 and
takes a few minutes with NMake.

By hand, from an "x64 Native Tools" prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

(The Visual Studio generator works too, but the "Visual Studio 18 2026"
generator needs CMake 4.2+.)

## Run

```bat
build\notita.exe                  :: demo scene
build\notita.exe --scene churn    :: whole world full of moving material
build\notita.exe --scene empty
build\notita_tests.exe            :: unit tests (optional name filter as arg)
build\notita_bench.exe [ticks] [threads...]
```

Other flags: `--threads N`, `--seed N`, `--frames N --screenshot out.bmp`
(run N frames, save the sim view, quit: handy for smoke tests).

Controls:

| Input | Action |
| --- | --- |
| Left mouse | paint the selected material (fills empty/gas cells) |
| Shift + left mouse | paint, replacing anything |
| Right mouse | erase |
| Wheel, `[` `]` | brush size |
| `1`-`9` | pick material |
| Middle mouse drag, WASD / arrows (Shift = fast) | pan |
| Space | pause |
| `.` or `N` | step one tick (pauses) |
| `R` / `B` / `C` | demo scene / full churn scene / clear |
| F3 / F4 | chunk grid / dirty rects overlay |
| Tab or F1 | hide the panels |

## Layout

```
src/sim/      simulation core, no SDL dependency (tests and bench link only this)
  Cell.h        16-byte per-pixel data (material, stamp, temperature, velocity, shade, flags, ...)
  Chunk.h       64x64 cells + dirty rects (atomic "next" rect)
  World.*       sparse chunk map (unloaded space acts as a wall)
  Simulator.*   4-pass checkerboard tick, CA rules
  Material.*    material registry + data file parser
  ThreadPool.*  persistent workers, blocking parallel-for
  Brush.*, Scenes.*
src/app/      SDL3 window, GPU presentation, ImGui panels
src/bench/    headless benchmark
tests/        unit tests (tiny built-in harness)
data/materials.txt   material definitions
```

## How the sim works

- **Chunks.** The world is a sparse map of 64x64 chunks. Each chunk keeps a
  dirty rect: only cells inside it are visited next tick, and a chunk whose
  rect is empty sleeps and costs nothing.
- **Threads.** A tick runs 4 passes, one per chunk parity (even/odd x, even/odd
  y). Chunks in one pass are two chunks apart, and an update may reach at most
  32 px (`kMaxReach`) outside its chunk, so a pass runs in parallel with no
  locks on cells. Dirty-rect growth into neighbours uses atomics (a union, so
  order doesn't matter).
- **Determinism.** Fixed 60 Hz tick. Every chunk update gets its own RNG stream
  seeded from (world seed, tick, pass, chunk), so the same seed and input give
  bit-identical worlds for any thread count (tested).
- **Rules.** Powders fall with gravity, slide off piles (`slide` sets the
  angle) and sink through lighter liquids. Liquids fall, run sideways up to
  `spread` px per tick (`viscosity` is the chance to hold still), sort by
  density and push under lighter liquids; heavy liquids percolate through
  light powders. Gases rise or sink against air (density 1.2) and spread under
  ceilings. Velocity is 8.8 fixed point in the cell.

## Benchmark

`notita_bench` runs a 640x384 world (10x6 chunks) where ~70% of all pixels are
a random mix of powders, liquids and gas. "churn" times the first ticks after
the fill; "flip" turns the world upside down every 90 ticks so the whole
screen keeps falling. Phase 1 numbers (Ryzen 7 3800X, 16 threads): about 1.3 ms per tick sustained with ~168k moving cells, 10 ms single-threaded; full table in Corvid note N250.
