#pragma once
// Phase 6: the streamed world. The simulation runs on a fixed-size window of
// chunks (the "awake" chunks) that follows a focus point (the player or the
// camera). Chunks that leave the window go to sleep: their whole state is
// encoded (ChunkCodec: palette + RLE planes) into the ChunkStore together
// with the gas grid cells over them, the splash particles flying in them and
// the rigid bodies whose centroid is in them. Chunks that enter the window
// come back from the store exactly as they left, or are generated
// (WorldGen) the first time they are seen. Nothing outside the window moves,
// so a sleeping chunk keeps its state for as long as it sleeps.
//
// Mass: a shift never creates or deletes anything; it only moves state
// between the live systems and the store. Conserved total =
//   live (world + particles + bodies + gas/kGasMassOne + inventory)
//   + storedMass() + storedGasFixed() / kGasMassOne,
// exact (whole pixel units and fixed-point gas) across shifts and saves.
//
// Save files (versioned, kSaveVersion) hold every chunk ever seen (awake
// ones encoded on the spot), the material and gas species names (so a
// changed materials.txt is remapped by name), the tick counters, the gas
// grid's heat totals, the bodies, the particles and the player with the
// inventory.
#include "Bodies.h"
#include "ChunkCodec.h"
#include "Electric.h"
#include "GasGrid.h"
#include "Particles.h"
#include "Player.h"
#include "ThreadPool.h"
#include "WorldGen.h"
#include "World.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace notita {

class Simulator;

// 2: electricity (per-chunk potentials, the electric tick and pending lightning)
// 3: the name of the generator's overlay (the showcase building), "" = none
constexpr std::uint32_t kSaveVersion = 3;

struct StoredChunk {
    std::vector<std::uint8_t> cells;  // encodeCells
    std::vector<std::uint8_t> gas;    // encodeGas (empty = no grid)
    std::vector<Particle> particles;
    std::vector<BodyState> bodies;
    std::vector<std::uint8_t> electric; // ElectricSystem::exportChunk (empty = no potentials)
    DirtyRect next;                   // the chunk's pending dirty rect
    bool heatNext = false;
    long long mass = 0;               // cells (no body stamps) + particles + bodies
    std::int64_t gasMass = 0;         // fixed point
    std::size_t bytes() const;
};

struct StreamParams {
    int chunksW = 16, chunksH = 8;    // the awake window
    int slackX = 1, slackY = 1;       // chunks the focus may drift off the window centre before it shifts (the camera clamps inside the window)
};

struct StreamStats {
    int shifts = 0;
    int generated = 0, woken = 0, slept = 0; // totals
    int strays = 0;                   // bodies that fell out of the window and went to sleep where they are
    double lastShiftMs = 0.0;
    std::size_t storeBytes = 0;       // encoded size of the sleeping chunks
    std::size_t storeChunks = 0;
};

// What a save holds besides the chunks.
struct SaveExtras {
    bool hasPlayer = false;
    PlayerState player;
    bool creative = false;
    long long capacity = 8000;
    int charges = 0;
    std::vector<std::pair<MaterialId, long long>> inventory;
};

class Streamer {
public:
    Streamer(World& world, const MaterialRegistry& mats, const WorldGen& gen, StreamParams params = {});

    void attach(Simulator* sim, GasGrid* gas, ParticleSystem* particles, BodySystem* bodies);
    // Generating, encoding and decoding chunks are pure per chunk: with a pool they run in parallel.
    void setPool(ThreadPool* pool) { pool_ = pool; }
    void setElectric(ElectricSystem* electric) { electric_ = electric; }
    // Loading a save made with another generator overlay (WorldOverlay::name)
    // calls this before anything is generated; it switches the overlay and
    // returns false if it can't. Without a hook such saves are rejected.
    void setOverlayHook(std::function<bool(const std::string&)> hook) { overlayHook_ = std::move(hook); }

    // Drops everything (world chunks, store, grid, particles, bodies) and
    // opens the window centred on the chunk holding (x, y).
    void start(int x, int y);
    // Shifts the window when the focus drifted too far. True if it moved.
    bool follow(float x, float y);
    // Moves the window so its top-left chunk is (cx0, cy0).
    void shiftTo(int cx0, int cy0);
    // Puts the world back to a plain empty window at (cx0, cy0) and forgets the store (scene switch).
    void reset(int cx0, int cy0);

    int windowX() const { return wcx_; }
    int windowY() const { return wcy_; }
    const StreamParams& params() const { return params_; }
    const StreamStats& stats() const { return stats_; }
    long long storedMass() const { return storedMass_; }
    std::int64_t storedGasFixed() const { return storedGas_; }
    // Pixel mass the generator has created so far (new terrain is the only source).
    long long generatedMass() const { return generatedMass_; }
    std::size_t storedChunks() const { return store_.size(); }
    bool isStored(int cx, int cy) const { return store_.count(chunkKey(cx, cy)) != 0; }

    // Writes the whole world to a file. The live state is untouched.
    bool save(const std::string& path, const SaveExtras& extras, std::string* error = nullptr);
    bool saveToBytes(std::vector<std::uint8_t>& out, const SaveExtras& extras);
    // Replaces the whole world with a saved one.
    bool load(const std::string& path, SaveExtras& extras, std::string* error = nullptr);
    bool loadFromBytes(const std::vector<std::uint8_t>& in, SaveExtras& extras, std::string* error = nullptr);

private:
    void sleepChunk(int cx, int cy, StoredChunk&& encoded);
    StoredChunk& storeEntry(int cx, int cy); // a sleeping chunk's record, generated first if never seen
    void sleepStrays();
    void wakeChunk(int cx, int cy, bool decoded);
    void parallel(int n, const std::function<void(int)>& fn);
    // cells: the chunk's cell blob when it was already encoded (in parallel), else encoded here
    StoredChunk encodeLive(const Chunk& c, bool clearGas, std::vector<std::uint8_t>* cells = nullptr);
    void forgetStore();

    World& world_;
    const MaterialRegistry& mats_;
    const WorldGen& gen_;
    StreamParams params_;
    StreamStats stats_;
    Simulator* sim_ = nullptr;
    GasGrid* gas_ = nullptr;
    ParticleSystem* particles_ = nullptr;
    BodySystem* bodies_ = nullptr;
    ElectricSystem* electric_ = nullptr;
    ThreadPool* pool_ = nullptr;
    std::function<bool(const std::string&)> overlayHook_;
    std::map<std::uint64_t, StoredChunk> store_; // ordered: deterministic saves
    int wcx_ = 0, wcy_ = 0;
    long long storedMass_ = 0;
    std::int64_t storedGas_ = 0;
    long long generatedMass_ = 0;
};

} // namespace notita
