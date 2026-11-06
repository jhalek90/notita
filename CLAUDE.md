# notita: notes for agents

## Project notes
- Build on this PC: system CMake is 3.25 (no VS 2026 generator) and there is no Ninja; use `build.bat` (vcvars64 + NMake) or call vcvars64.bat then `cmake --build build`. Bash `cmd //c "..."` quoting silently skips builds: run builds from PowerShell.
- `src/sim` must stay SDL-free; tests (`notita_tests [filter]`) and `notita_bench` link only `notita_sim` + `notita_script` (Luau), never SDL. For quick sim-only builds configure a second dir with `-DNOTITA_BUILD_APP=OFF`.
- Threading contract: a chunk update may read/write at most `kMaxReach` (32 px) outside its chunk; keep max fall/spread per tick <= 16 or the checkerboard passes race.
- Determinism contract: all randomness inside a tick comes from the per-chunk `Rng` passed to the update; never use global/thread-local RNGs or iterate unordered containers in the tick. `determinism_independent_of_thread_count` guards this.
- Cell is 16 bytes on purpose (`static_assert`); temperature is used by heat, life by flame/burn timers, aux is still free (bodies in phase 4). Adding fields means revisiting the size.
- Unknown keys in data/materials.txt land in `Material::extra`, so later phases can add properties without touching the parser. Material names/art are original; don't copy another game's.
- Rule tests use their own material table (tests/test_sim.cpp), so retuning data/materials.txt doesn't break them.
- Mass rule: every state change, burn-out and reaction must keep `Material::mass` (whole units; air and flames are 0). The registry/ReactionTable reject anything else at load. With the gas grid on, conserved mass is `World::totalMass * kGasMassOne + GasGrid::massFixed()` (grid mass is fixed-point ints, moved only as integer face fluxes); never delete gas cells, hand them over via `GasExchange`.
- Luau runs only at load time, in `notita_script` (src/script); `notita_sim` never links Luau and nothing scripted runs in the tick. data/reactions.luau must return a plain table.
- Heat runs inside the same checkerboard chunk job as motion and writes 1 px into the right/lower chunk; a chunk runs it when it moved or `heatNext` is set. Conduction pairs are capped (convex averages) so it can't blow up; keep that cap if you retune it.
- The Bash tool eats backslashes in inline heredocs (a `\n` inside arrives as a real newline): put edit scripts that contain escapes in a file first.
- Gas grid (src/sim/GasGrid.*): `GasGrid::step()` runs after `Simulator::tick()` (App::tickSim). `CpuGasSolver` (GasSolverCpu.cpp) and src/app/shaders/gas.hlsl are the same kernels in the same order: change both, then run `notita --gas-selftest` (needs a D3D12 GPU). CMake builds the .dxil with the Windows SDK's dxc into build/shaders/; without them the app silently uses the CPU solver.
- With the grid on, the sim's `ambientRate` is 0 (the grid carries air heat and counts its losses in `lostHeat/sourceHeat/latentHeat`), and every gas pixel is absorbed into the grid each step, so CA gas rules only run with the grid off.
- GasGrid touches pixels outside the tick: row-parallel jobs must stay 4 block rows apart (pushPixels) or run serially (condense, expandPockets, explode).
