#include "ReactionScript.h"

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace notita {

namespace {

struct VmBudget {
    std::size_t used = 0, limit = 0;
    std::uint64_t interrupts = 0, maxInterrupts = 0;
};

void* budgetAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* b = static_cast<VmBudget*>(ud);
    if (nsize == 0) {
        b->used -= osize;
        std::free(ptr);
        return nullptr;
    }
    if (nsize > osize && b->used + (nsize - osize) > b->limit) return nullptr; // VM raises "not enough memory"
    void* p = std::realloc(ptr, nsize);
    if (p) b->used = b->used - osize + nsize;
    return p;
}

void budgetInterrupt(lua_State* L, int gc) {
    if (gc >= 0) return; // GC steps don't count
    auto* b = static_cast<VmBudget*>(lua_callbacks(L)->userdata);
    if (++b->interrupts > b->maxInterrupts) {
        lua_callbacks(L)->interrupt = nullptr; // so the error itself isn't interrupted again
        luaL_error(L, "script ran too long (instruction budget used up)");
    }
}

// Closes the VM on every path.
struct VmGuard {
    lua_State* L = nullptr;
    ~VmGuard() { if (L) lua_close(L); }
};

std::string str(lua_State* L, int idx) {
    std::size_t len = 0;
    const char* s = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, &len) : nullptr;
    return s ? std::string(s, len) : std::string();
}

// Reads entry fields with raw access only: no metamethods (no script code) run after the pcall.
bool readEntry(lua_State* L, int entryIdx, ReactionSpec& out, std::string& err) {
    auto field = [&](const char* k) { return lua_rawgetfield(L, entryIdx, k); }; // pushes the value
    auto number = [&](const char* k, float& v) {
        const int t = field(k);
        bool ok = true;
        if (t == LUA_TNUMBER) v = static_cast<float>(lua_tonumberx(L, -1, nullptr));
        else if (t != LUA_TNIL) { err = std::string("'") + k + "' must be a number"; ok = false; }
        lua_pop(L, 1);
        return ok;
    };
    auto name = [&](const char* k, std::string& v) {
        const int t = field(k);
        bool ok = t == LUA_TSTRING;
        if (ok) v = str(L, -1);
        else err = std::string("'") + k + "' must be a material name";
        lua_pop(L, 1);
        return ok;
    };
    if (!name("a", out.a) || !name("b", out.b)) return false;

    const int t = field("into");
    if (t == LUA_TSTRING) {
        out.outA = str(L, -1);
        out.outB = "air";
    } else if (t == LUA_TTABLE) {
        const int n = lua_objlen(L, -1);
        if (n < 1 || n > 2) { lua_pop(L, 1); err = "'into' needs one or two products"; return false; }
        lua_rawgeti(L, -1, 1);
        out.outA = str(L, -1);
        lua_pop(L, 1);
        out.outB = "air";
        if (n == 2) {
            lua_rawgeti(L, -1, 2);
            out.outB = str(L, -1);
            lua_pop(L, 1);
        }
        if (out.outA.empty() || out.outB.empty()) { lua_pop(L, 1); err = "products must be material names"; return false; }
    } else {
        lua_pop(L, 1);
        err = "'into' must be a product name or a list of two";
        return false;
    }
    lua_pop(L, 1);
    return number("heat", out.heat) && number("chance", out.chance) && number("min_temp", out.minTemp) &&
           number("max_temp", out.maxTemp);
}

} // namespace

std::vector<ReactionSpec> runReactionScript(std::string_view source, std::string_view chunkName,
                                            std::vector<std::string>* errors, const ScriptLimits& limits) {
    std::vector<ReactionSpec> specs;
    const std::string chunk(chunkName);
    auto report = [&](const std::string& msg) { if (errors) errors->push_back(chunk + ": " + msg); };

    std::size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
    if (!bytecode) { report("compile failed"); return specs; }

    VmBudget budget;
    budget.limit = limits.memoryBytes;
    budget.maxInterrupts = limits.interrupts;
    VmGuard vm;
    vm.L = lua_newstate(budgetAlloc, &budget);
    lua_State* L = vm.L;
    if (!L) { std::free(bytecode); report("could not create the script VM"); return specs; }

    // only pure libraries: no os, io, debug, require, loadstring
    const luaL_Reg libs[] = {{"", luaopen_base}, {LUA_STRLIBNAME, luaopen_string},
                             {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math}};
    for (const luaL_Reg& lib : libs) {
        lua_pushcfunction(L, lib.func, nullptr);
        lua_pushstring(L, lib.name);
        lua_call(L, 1, 0);
    }
    // print would be the only way out of the sandbox; drop it, along with fenv tricks
    for (const char* g : {"print", "getfenv", "setfenv", "newproxy", "gcinfo", "collectgarbage"}) {
        lua_pushnil(L);
        lua_setglobal(L, g);
    }
    luaL_sandbox(L); // globals and libraries become read-only

    lua_State* T = lua_newthread(L);
    luaL_sandboxthread(T); // the script gets its own globals table on top of the read-only ones
    lua_callbacks(L)->userdata = &budget;
    lua_callbacks(L)->interrupt = budgetInterrupt;

    const int loaded = luau_load(T, chunk.c_str(), bytecode, bytecodeSize, 0);
    std::free(bytecode);
    if (loaded != 0) {
        report(str(T, -1));
        return specs;
    }
    if (lua_pcall(T, 0, 1, 0) != LUA_OK) {
        report(lua_type(T, -1) == LUA_TSTRING ? str(T, -1) : std::string("script error"));
        return specs;
    }
    lua_callbacks(L)->interrupt = nullptr;

    if (lua_type(T, -1) != LUA_TTABLE) {
        report("must return a table of reactions");
        return specs;
    }
    const int n = lua_objlen(T, -1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(T, -1, i);
        if (lua_type(T, -1) != LUA_TTABLE) {
            report("entry " + std::to_string(i) + ": must be a table");
        } else {
            ReactionSpec spec;
            std::string err;
            if (readEntry(T, lua_gettop(T), spec, err)) specs.push_back(std::move(spec));
            else report("entry " + std::to_string(i) + ": " + err);
        }
        lua_pop(T, 1);
    }
    return specs;
}

bool loadReactions(ReactionTable& table, std::string_view source, std::string_view chunkName,
                   std::vector<std::string>* errors, const ScriptLimits& limits) {
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;
    const std::size_t before = errs.size();
    const std::vector<ReactionSpec> specs = runReactionScript(source, chunkName, &errs, limits);
    for (const ReactionSpec& s : specs) {
        std::string err;
        if (!table.add(s, &err)) errs.push_back(std::string(chunkName) + ": " + err);
    }
    return errs.size() == before;
}

bool loadDefaultReactions(ReactionTable& table, std::vector<std::string>* errors) {
    const char* candidates[] = {"data/reactions.luau", "../data/reactions.luau",
#ifdef NOTITA_DATA_DIR
                                NOTITA_DATA_DIR "/reactions.luau",
#endif
    };
    for (const char* path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        return loadReactions(table, ss.str(), "reactions.luau", errors);
    }
    if (errors) errors->push_back("data/reactions.luau not found: no reactions");
    return false;
}

} // namespace notita
