# notita

A 2D pixel sandbox where every pixel is simulated. "notita" is the repo name;
the game itself still needs its own name (the window says "working title").

Phase 1 is the engine core: a chunked, multithreaded falling-sand
simulation, data-driven materials, a GPU-presented 640x360 view and sandbox
brushes. Phase 2 adds per-pixel heat, state changes, fire and a reaction table
written in Luau. The full plan lives in Corvid note N249.

## Build (Windows, MSVC)

Needs Visual Studio 2022 or newer with the C++ workload, and CMake 3.24+.
SDL3 3.4.20, Dear ImGui 1.93.0 and Luau 0.745 are downloaded by CMake
FetchContent on the first configure (hash-pinned).

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
build\notita.exe --scene forge    :: heat playground: magma, fire, etchant, frost
build\notita.exe --scene churn    :: whole world full of moving material
build\notita.exe --scene thermal  :: churn with hot and cold materials mixed in
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
| Ctrl + left / right mouse | heat / cool under the brush |
| `H` / `J` / `P` | brush mode: heat / cool / paint |
| `R` / `F` / `B` / `C` | demo scene / forge scene / full churn scene / clear |
| F3 / F4 | chunk grid / dirty rects overlay |
| F5 | temperature view (hot pixels also glow in the normal view) |
| Tab or F1 | hide the panels |

## Layout

```
src/sim/      simulation core, no SDL dependency (tests and bench link only this)
  Cell.h        16-byte per-pixel data (material, stamp, temperature, velocity, shade, flags, ...)
  Chunk.h       64x64 cells + dirty rects (atomic "next" rect)
  World.*       sparse chunk map (unloaded space acts as a wall)
  Simulator.*   4-pass checkerboard tick, CA rules
  Material.*    material registry + data file parser (heat, state changes, fire, mass)
  Reactions.*   reaction table (plain data the tick looks up)
  GasExchange.h hook for the phase 3 gas grid (takes created gas mass and ambient heat)
  ThreadPool.*  persistent workers, blocking parallel-for
  Brush.*, Scenes.*
src/script/   sandboxed Luau, load time only: data/reactions.luau -> ReactionTable
src/app/      SDL3 window, GPU presentation, ImGui panels
src/bench/    headless benchmark
tests/        unit tests (tiny built-in harness)
data/materials.txt   material definitions (keys documented at the top)
data/reactions.luau  reaction table (A + B -> products + heat)
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

## Heat, state changes, fire, reactions (phase 2)

- **Temperature** is a float per cell (degrees C). Each tick, after a chunk's
  motion, the chunk runs a heat pass when it moved or its heat is awake: every
  cell trades heat with its right and lower neighbour. The pair conductance is
  the harmonic mean of the two `conductivity` values, capped so an exchange
  moves the pair at most halfway to their common temperature; each pair moves
  the same energy out of one cell as into the other (`heat_capacity` x
  degrees), so conduction keeps the total energy. Heat writes 1 px into the
  next chunk, well inside the checkerboard margin, so it stays lock-free and
  deterministic. A chunk's heat sleeps once nothing in it changes by more
  than 0.01 C per tick; a warmer or colder edge wakes the neighbour.
- **Air** barely conducts and slowly relaxes towards the ambient 20 C (the
  stand-in for convection until the phase 3 gas grid takes that heat).
- **State changes** come from `melt_point`/`boil_point` and
  `freeze_point`/`condense_point` in data/materials.txt, with optional
  `latent_heat`. The heat above the change point is kept and the latent heat
  is taken (or given back on the way down).
- **Mass is conserved.** Every material has a whole-number `mass` per cell
  (air and flames 0). A state change, burn-out or reaction that would change
  the total is rejected when the data loads, so one water pixel boils into one
  steam pixel and condenses back into one water pixel.
- **Fire.** A material with `ignite_point` starts burning once it is that hot
  and touches something it can draw through (air, a flame, a gas, or a cell
  already burning). Burning cells release `burn_heat`, throw short-lived,
  massless flame cells into the air, and end as `burns_into` (or
  `burn_residue`). Water and frost put flames out (reaction table).
- **Reactions** live in data/reactions.luau: `{ a = "etchant", b = "iron",
  into = { "rust_salt", "hydrogen" }, heat = 45, chance = 0.04 }`. The script
  runs once at load in a sandboxed Luau VM (base, string, table and math only;
  read-only globals; an instruction and memory budget) and only its returned
  table is kept. The two products share the pair's heat plus `heat`.
- **Gas hook.** `Simulator::setGasExchange` lets the phase 3 grid take every
  gas the sim creates (mass + temperature) and the heat air loses to the
  ambient; emissions are gathered per chunk and delivered in chunk order.

## Benchmark

`notita_bench` runs a 640x384 world (10x6 chunks) where ~70% of all pixels are
a random mix of powders, liquids and gas. "churn" times the first ticks after
the fill; "flip" turns the world upside down every 90 ticks so the whole
screen keeps falling. Both run with heat off and on. "thermal" is flip with
magma, frost, timber, etchant and iron mixed in (boiling, freezing, burning and
reactions); "conduct" is solid slate at random temperatures (heat only).

Phase 2 numbers, Ryzen 7 3800X, Release, 900 ticks, mean ms per tick:

| scene | heat | 1 thread | 4 threads | 16 threads |
| --- | --- | --- | --- | --- |
| churn | off | 4.77 | 1.57 | 0.92 |
| churn | on | 7.10 | 2.29 | 1.27 |
| flip | off | 10.02 | 2.83 | 1.36 |
| flip | on | 14.56 | 3.81 | 1.84 |
| thermal | on | 12.23 | 3.34 | 1.43 |
| conduct | on | 1.43 | 0.44 | 0.34 |

With every thread, a full screen of moving, heat-conducting material takes
about 1.8 ms of the 16.7 ms budget at 60 Hz. Phase 1 details are in Corvid
note N250; phase 2 details in the #468 note.
