# notita: notes for agents

## Project notes
- Build on this PC: system CMake is 3.25 (no VS 2026 generator) and there is no Ninja; use `build.bat` (vcvars64 + NMake) or call vcvars64.bat then `cmake --build build`. Bash `cmd //c "..."` quoting silently skips builds: run builds from PowerShell.
- `src/sim` must stay SDL-free; tests (`notita_tests [filter]`) and `notita_bench` link only `notita_sim`.
- Threading contract: a chunk update may read/write at most `kMaxReach` (32 px) outside its chunk; keep max fall/spread per tick <= 16 or the checkerboard passes race.
- Determinism contract: all randomness inside a tick comes from the per-chunk `Rng` passed to the update; never use global/thread-local RNGs or iterate unordered containers in the tick. `determinism_independent_of_thread_count` guards this.
- Cell is 16 bytes on purpose (`static_assert`); temperature/life/aux are reserved for heat (phase 2) and bodies (phase 4). Adding fields means revisiting the size.
- Unknown keys in data/materials.txt land in `Material::extra`, so later phases can add properties without touching the parser. Material names/art are original; don't copy another game's.
- Rule tests use their own material table (tests/test_sim.cpp), so retuning data/materials.txt doesn't break them.
