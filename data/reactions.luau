-- notita reaction table.
--
-- This file is data. It runs once at load time in a sandboxed Luau VM (no io,
-- os, require or loadstring; a time and memory budget) and returns a list of
-- reactions. Nothing here runs during the simulation.
--
-- Each entry: when a cell of `a` touches a cell of `b` (up/down/left/right):
--   a = "...", b = "..."        reactants
--   into = { "pa", "pb" }       a becomes pa, b becomes pb ("pb" defaults to "air")
--   heat = 0                    energy released (> 0) or absorbed (< 0); both
--                               products end up at one shared temperature
--   chance = 1                  chance per tick for each touching pair
--   min_temp / max_temp         only when a's temperature is in this range
-- Mass must balance: mass(a) + mass(b) == mass(pa) + mass(pb) (see
-- data/materials.txt; air and flames have mass 0). Entries that don't are
-- rejected with a message when the game loads.

local reactions = {
	-- etchant eats iron: rust salt drops out and hydrogen bubbles off
	{ a = "etchant", b = "iron", into = { "rust_salt", "hydrogen" }, heat = 45, chance = 0.04 },
	-- etchant dissolves cinder grit into a heavy, flammable murk
	{ a = "etchant", b = "cinder_grit", into = { "water", "murk" }, heat = 10, chance = 0.02 },
	-- rust salt melts frost (and stays salt)
	{ a = "frost", b = "rust_salt", into = { "water", "rust_salt" }, heat = -4, chance = 0.05 },
	-- water hitting magma: a crust of basalt and a burst of steam
	{ a = "water", b = "magma", into = { "steam", "basalt" }, heat = -900, chance = 0.25 },
	-- quickmetal cools molten glass straight into glass
	{ a = "molten_glass", b = "quickmetal", into = { "glass", "quickmetal" }, heat = -300, chance = 0.02 },
}

-- Flames are put out by water and frost. (A loop is fine: the result is just a table.)
for _, wet in { "water", "frost" } do
	table.insert(reactions, {
		a = "flame",
		b = wet,
		into = { "air", if wet == "water" then "steam" else "water" },
		heat = -60,
		chance = 0.6,
	})
end

return reactions
