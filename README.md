# notita

A 2D pixel sandbox where every pixel is simulated. "notita" is the repo name;
the game itself still needs its own name (the window says "working title").

Phase 1 is the engine core: a chunked, multithreaded falling-sand
simulation, data-driven materials, a GPU-presented 640x360 view and sandbox
brushes. Phase 2 adds per-pixel heat, state changes, fire and a reaction table
written in Luau. Phase 3 adds the gas grid: smoke, steam and other gases
flow on a velocity/pressure grid (GPU compute, with a CPU reference), with
buoyancy, convection, burning gas and explosions. Phase 4 adds splash
particles (liquids hit hard fly as momentum particles and settle back),
miscible liquids that blend, and rigid bodies: unsupported clumps of solid
pixels become Box2D bodies that fall, tumble and shatter. Phase 5 adds the
player: a 20 px capsule that walks, jumps, swims and glides through the
pixel world, digs material into an inventory and places it back, with a
creative mode and movement tools that all act through the sim (a jet that
puts real gas into the grid, a grapple, blast jumps, rafts that float by
density, freezing water to walk on). The full plan lives in Corvid note N249.

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
build\notita.exe --scene gas      :: gas lab: boiling pot, chimney, hydrogen cave, heavy gas
build\notita.exe --scene quarry   :: phase 4: waterfall, blending liquids, blocks on a burning post, a boulder
build\notita.exe --scene play     :: phase 5: the player in a playground (hill, lake, rafts, cliff, updraft, quickmetal tub)
build\notita.exe --autoplay       :: the play scene with the player driven through a short demo
build\notita.exe --scene empty
build\notita.exe --gas-selftest   :: GPU gas solver vs the CPU reference, prints PASS/FAIL
build\notita_tests.exe            :: unit tests (optional name filter as arg)
build\notita_bench.exe [ticks] [threads...]
```

Other flags: `--threads N`, `--seed N`, `--frames N --screenshot out.bmp`
(run N frames, save the sim view, quit: handy for smoke tests),
`--gas-cpu` (gas grid on the CPU solver), `--gas-view gas|velocity|pressure`.

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
| F6 | gas view: gas / velocity (hue = direction) / pressure (red +, blue -) |
| `G` / `X` / `V` | gas grid on/off / blast at the cursor (also kicks bodies) / gas lab scene |
| `Q` / `K` | quarry scene / cut the solid clump under the cursor loose as a rigid body |
| Tab or F1 | hide the panels |
| `L` / F7 | play scene with the player / spawn (or remove) a player at the cursor in any scene |
| F2 / `T` | creative mode on/off / slow motion |

With a player in the world the keys change (the Player panel lists them too):

| Input | Action |
| --- | --- |
| `A` `D` | walk (steps up ledges of 4 px) |
| `W` / Space | jump; swim up in liquid; reel the rope in; hold while falling to glide |
| `S` | swim down; let the rope out |
| `1`-`6` | tool: dig, jet, grapple, blast, raft, freeze |
| Left mouse | use the tool towards the cursor (within reach) |
| Right mouse | place the material in hand with the brush |
| Wheel (Shift+wheel = size) | material in hand; `[` `]` brush size |
| `Z` | placed solids fixed (scene geometry) or loose (they can fall as bodies) |
| `P` | pause (Space jumps) |

## Layout

```
src/sim/      simulation core, no SDL dependency (tests and bench link only this)
  Cell.h        16-byte per-pixel data (material, stamp, temperature, velocity, shade, flags, ...)
  Chunk.h       64x64 cells + dirty rects (atomic "next" rect)
  World.*       sparse chunk map (unloaded space acts as a wall)
  Simulator.*   4-pass checkerboard tick, CA rules
  Material.*    material registry + data file parser (heat, state changes, fire, mass)
  Reactions.*   reaction table (plain data the tick looks up)
  GasExchange.h hook the gas grid plugs into (takes created gas mass and ambient heat)
  GasGrid.*     phase 3 gas grid: fields, pixel coupling, explosions
  GasSolverCpu.cpp  the grid math on the CPU (reference for the GPU kernels)
  Particle.h, Particles.*  phase 4 momentum particles (splashes, blast debris, shoved pixels)
  Bodies.*      phase 4 rigid bodies on Box2D v3: clump search, stamping, terrain, shatter, buoyancy
  Player.*      phase 5 player: capsule movement, damage, inventory, dig/place and the movement tools
  ThreadPool.*  persistent workers, blocking parallel-for
  Brush.*, Scenes.*
src/script/   sandboxed Luau, load time only: data/reactions.luau -> ReactionTable
src/app/      SDL3 window, GPU presentation, ImGui panels
  GpuGasSolver.*, shaders/gas.hlsl   the gas grid math as SDL3 GPU compute kernels
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

## Gas grid (phase 3)

- **The grid.** One cell per 4x4 pixels (256x128 cells for the app's
  1024x512 world). Velocity lives on cell faces (a MAC grid), in cells per
  tick. Each tick, after the pixel tick: buoyancy (hot gas and gas lighter
  than air rise, heavy gas sinks), vorticity confinement for swirls,
  semi-Lagrangian advection of the velocity, a Jacobi pressure solve (40
  iterations) and projection, then transport of gas mass and heat.
- **Two solvers, same kernels.** `CpuGasSolver` (src/sim, threaded, used by
  the tests and the benchmark) and `GpuGasSolver` (src/app, SDL3 GPU compute,
  src/app/shaders/gas.hlsl built with the Windows SDK's dxc into
  `build/shaders/*.dxil`). The app uses the GPU one when the device takes
  DXIL and the shaders are there, else the CPU one (`--gas-cpu` forces it).
  `notita --gas-selftest` runs both from the same state and compares.
- **Mass is exact.** Gas mass is fixed point (4096 per `mass` unit) and moves
  only as integer face fluxes: each face's flux is computed by both cells
  with the same expression, and a cell never sends out more than it holds.
  So pixels + grid keep the same total mass every tick, on the GPU too.
- **Pixels and grid.** Solid, liquid and powder pixels are obstacles (a block
  with no air, gas or flame pixel is closed). Every gas the sim makes
  (boiling, burning, reactions) goes into the grid through `GasExchange`;
  gas pixels painted with a brush are taken in on the next tick. Gas cooled
  below its `condense_point` falls back out as pixels into air in its block
  (steam rains back as water, smoke drops ash). Air pixels and their grid
  cell trade heat, so hot air rises and warms the ceiling (convection); with
  the grid on, the pixel sim's own air cooling is switched off and the grid
  loses a little heat to the ambient instead (counted, so tests can check
  the energy balance).
- **Pressure on pixels.** Fast gas moves the liquid and powder pixels it
  blows against. A pocket packed denser than about 1.2 units per pixel
  (gas trapped under or behind something) shoves the line of loose pixels
  next to it one pixel outwards, towards thinner gas.
- **Fire and explosions.** Flammable grid gas (hydrogen, murk) burns where it
  is hot enough or touches a flame, turns into its `burns_into` gas, heats
  and expands; a burning cell lights its neighbours next tick, so a cloud
  goes up as a front. `X` (or `GasGrid::explode`) sets off a blast: an
  expansion source, heat and a radial kick in the grid, and the loose pixels
  in the radius are thrown outwards.

## Splashes, blending liquids, rigid bodies (phase 4)

- **Splash particles.** A liquid pixel that fell this tick and is stopped
  hard (`splashSpeed`, 5 px/tick) splashes: the surface pixels of the pool
  around the impact (or, on hard ground, the drop itself) leave the grid as
  particles. Each particle carries its whole cell (material, temperature,
  shade), so `World::totalMass + ParticleSystem::totalMass` is constant; the
  cell left behind is air at ambient. The ejecta share at most `restitution`
  (0.6) of the impact momentum, drawn from a budget until it is spent.
  Particles fly with gravity, a little drag and the gas grid's wind
  (`gasDrag`), stop at the first non-empty cell and settle into it, handing
  their velocity back to the cell (the automaton carries on with the fall
  speed). A particle with no room waits, and climbs out if liquid flowed
  over it, so nothing is ever lost. Blasts (`X`) throw loose pixels as
  particles too, and rigid bodies shove liquids and powders aside as
  particles. Particles are created in chunk order and settled in index order,
  so results stay deterministic for any thread count.
- **Miscible liquids.** `miscible = water, tincture` in the material data
  (symmetric). Miscible pairs don't density-sort; instead a liquid swaps with
  a different miscible neighbour with chance `mixRate` per tick, a random walk
  that blends the pool. `Cell::aux` counts a pixel's swaps (it starts swaps
  under `mixSteps`, takes part under twice that), so a blended pool stops and
  sleeps while fresh liquid still mixes in. Immiscible liquids layer as before.
- **Rigid bodies (Box2D v3.2, FetchContent).** Every few ticks, chunks that
  moved are searched for 4-connected clumps of solid pixels. A clump is held
  up by an anchor material (bedrock, `anchor = true`) or the world edge
  anywhere in it, or, once physics has put it down (`aux == kAuxSettled`), by
  powder, another body or pinned scene geometry under one of its pixels.
  Scene builders pin their solids (`aux == kAuxPinned`); brushed solids and
  solids made by state changes are live. An unsupported clump becomes a body
  that owns its pixels (local coordinates, material, temperature). Each tick
  the bodies leave the grid, Box2D steps them against the pixel terrain (the
  solid and powder pixels around each body turned into static boxes by a
  row-run rectangle cover, used once), and they are stamped back with
  `kFlagBody`, so liquids flow around them, powders pile on them and heat
  conducts through them; a stamped pixel that melts, burns or is erased
  simply leaves the body. The gas grid pushes bodies (pressure gradient and
  wind at the centre) and blasts kick them. A velocity change above
  `shatterSpeed` (5 px/tick) in one step shatters a body into a few
  fragments (nearest random seed, then connected pieces); crumbs fly off as
  particles. A body at rest for `freezeAfter` ticks turns back into settled
  pixels. `World::totalMass` skips body stamps, so world + particles + bodies
  is the conserved sum (tested every tick in the phase 4 tests).

## The player, inventory and movement tools (phase 5)

- **Movement.** The player is a 7x20 px capsule that is not stamped into the
  grid: it moves through it on its own, colliding with solid and powder
  pixels (rigid-body stamps included) in pixel steps, and walks up ledges of
  up to 4 px. It floats by the density of the liquid around it (900 vs water
  1000: head out; it sinks in lamp oil), with drag in liquid. The gas grid's
  wind and pressure gradient over the cells it covers push it, so blasts,
  jets and drafts move it; holding jump while falling spreads the cloak (much
  more vertical drag), which lets a gentle updraft carry it. Standing on a
  rigid body carries it along and presses on the body with its weight.
  Damage: hot or cold pixels around it (past 70 C or below -30 C), flames and
  burning pixels, and landings faster than 6.5 px/tick; it regenerates after
  2 s without damage and respawns at its spawn point.
- **Dig and place.** Digging removes pixels under the brush (solids by
  hardness, from `hardness` in the material data or the density; anchors and
  body stamps can't be dug) and counts them in the inventory per material.
  Placing puts inventory material back into air cells (never inside the
  player). Play mode has a capacity and counts everything, so world +
  particles + bodies + gas grid + inventory mass stays exactly constant
  (tested every tick with every system on). Creative mode (F2) has every
  material without limit, like the sandbox brushes.
- **Tools, all through the sim.** Jet: a liquid from the inventory is boiled
  (water into steam) or a fuel burnt (lamp oil into smoke) and goes into the
  gas grid at the nozzle with a velocity kick (`GasGrid::jet`, the same
  intake as the sim's emissions, so mass is exact); the recoil pushes the
  player the other way, and the jet blows dust and smoke around. Grapple: a
  rope to the first solid pixel along the aim; it follows and pulls on a
  rigid body it hooks, and comes loose when its pixel is dug, melts or burns.
  Blast: a charge set off through the explosion path (`blastAt`: the gas
  grid's blast, the bodies' kick and the player's own kick; the `X` key uses
  the same function), so a blast at the feet is an explosive jump that also
  throws the ground as particles. Raft: a 48x6 block of the solid in hand,
  cut loose as a rigid body; bodies now float by density (Archimedes on the
  pixels under the pool surface beside them, at their centroid, with drag),
  so timber floats on water and even slate floats on quickmetal. Freeze: a
  cooling brush that stops at -12 C, so water freezes into frost you can walk
  on (the sheet floats away as a body if it isn't held).

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

Phase 3 rows, gas grid on (CPU solver, 160x96 cells, 40 pressure
iterations; the number includes the grid step):

| scene | 1 thread | 4 threads | 16 threads | of which grid (16 thr) |
| --- | --- | --- | --- | --- |
| gaslab | 10.70 | 3.84 | 4.26 | 3.72 (solve 2.64) |
| thermal+gas | 24.61 | 7.75 | 6.40 | 5.05 (solve 2.70) |

"thermal+gas" is the thermal worst case with every gas it makes (steam from
boiling, smoke, fumes, hydrogen) going into the grid; the coupling then also
pushes a lot of pixels. The CPU solver stops scaling past 4 threads (each of
its ~47 kernels is one parallel-for). In the app the grid runs on the GPU:
`notita --gas-selftest` measured about 1.4 ms per step on the 256x128 grid,
upload, 47 dispatches and download included (it waits for the result).

Phase 4 rows (particles and bodies on; gas rows on the CPU solver):

| scene | 1 thread | 4 threads | 16 threads | particles / bodies (16 thr) |
| --- | --- | --- | --- | --- |
| quarry | 9.6 | 3.6 | 4.3 | 0.01 ms (~70 alive) / 0.19 ms (1 body, 757 px) |
| thermal+p4 | 24.5 | 12.9 | 7.9 | 0.00 ms / 1.5 ms (14 bodies, 87 px) |
| flip+p4 | 11.2 | 3.4 | 1.8 | 0.03 ms (~100 alive) / 0.3 ms (0 bodies) |

"thermal+p4" is the thermal+gas worst case with the scattered frost, timber
and iron pixels in the random mix turning into (tiny) bodies: most of its
body time is the clump search over 60 active chunks. "flip+p4" is flip with
splashes on: every liquid landing hard splashes, and the particle step costs
almost nothing. Phase 4 details are in the #470 note.

Phase 5 row: the playground with everything on and a scripted player that
walks back and forth, digs, jets and sets off a charge every 2 s:

| scene | 1 thread | 4 threads | 16 threads | player step |
| --- | --- | --- | --- | --- |
| play | 8.1 | 4.1 | 4.9 | 0.015 ms |

Most of it is the CPU gas solver (6.8 / 3.1 / 3.9 ms); the player itself is
about 15 microseconds a tick. Phase 5 details are in the #471 note.
