#pragma once
// Loads the reaction table from a Luau script (data/reactions.luau).
//
// The script is data: it runs once, in a fresh sandboxed VM, and must return
// an array of entries:
//     return {
//         { a = "etchant", b = "iron", into = { "rust_salt", "hydrogen" }, heat = 40, chance = 0.05 },
//         { a = "water", b = "magma", into = { "steam", "basalt" }, heat = -800, min_temp = 0 },
//     }
// Fields: a, b (reactants), into = { product for a, product for b } (the
// second defaults to "air"), heat (energy, default 0), chance (per tick per
// touching pair, default 1), min_temp / max_temp (on a's temperature).
// Helper functions and loops are fine; the result is copied out and the VM is
// closed. Nothing from the script runs during the simulation.
//
// Sandbox: only the base, string, table and math libraries are opened (no
// os, io, debug, require or loadstring), globals are read-only, and the
// script is stopped after a fixed instruction budget and memory limit.
#include "sim/Reactions.h"

#include <string>
#include <string_view>
#include <vector>

namespace notita {

struct ScriptLimits {
    std::size_t memoryBytes = 16u << 20;     // 16 MiB
    std::uint64_t interrupts = 2'000'000;    // VM safepoints (loop back edges, calls) before it is stopped
};

// Runs the script and returns its entries. On a script error the result is
// empty and errors holds the message. Entries with bad fields are skipped
// with a message each.
std::vector<ReactionSpec> runReactionScript(std::string_view source, std::string_view chunkName,
                                            std::vector<std::string>* errors, const ScriptLimits& limits = {});

// Runs the script and adds every valid entry to the table (unknown materials,
// mass mismatches etc. are reported and skipped). Returns true when there
// were no errors at all.
bool loadReactions(ReactionTable& table, std::string_view source, std::string_view chunkName,
                   std::vector<std::string>* errors, const ScriptLimits& limits = {});

// data/reactions.luau next to the materials (same search as materials.txt).
// Returns false when the file can't be found or had errors.
bool loadDefaultReactions(ReactionTable& table, std::vector<std::string>* errors);

} // namespace notita
