// #484: the one tabbed menu and the small HUD. Every system adds its tab or its
// sections from a register function (registerUi below calls them); UiMenu does the
// window, the tabs and imgui.ini. To add a feature's controls: write
// App::registerFooUi() next to the feature, add menu_.add(tab, section, order, draw)
// calls there and one registerFooUi() line in registerUi().
#include "App.h"
#include "Brand.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace notita {

namespace {

ImU32 toImColor(std::uint32_t rgba) { return rgba | 0xFF000000u; } // ImU32 is also ABGR in memory

// A row of buttons that wraps to the next line when the next one doesn't fit.
struct ButtonFlow {
    float right;
    bool first = true;
    ButtonFlow() : right(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x) {}
    bool operator()(const char* label) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float w = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2.0f;
        if (!first && ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right) ImGui::SameLine();
        first = false;
        return ImGui::Button(label);
    }
};

// Greyed-out help text that wraps to the menu's width.
void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

} // namespace

void App::registerUi() {
    menu_.addTab("Materials", 10);
    menu_.addTab("Player", 20);
    menu_.addTab("World", 30);
    menu_.addTab("Sim", 40);
    menu_.addTab("View", 50);
    menu_.addTab("Performance", 60);
    menu_.addTab("Debug", 70);
    registerSandboxUi();
    registerPlayerUi();
    registerWorldUi();
    registerTopDownUi(); // AppTopDown.cpp (#483)
    registerGasUi();
    registerFlameUi();
    registerElectricUi();
    registerBodiesUi();
    registerLiquidUi();
    registerLightUi();
    registerTextureUi();
    registerStatsUi();
}

void App::drawUi() {
    char title[128];
    // "###Menu" keeps the window's ImGui id (and its saved place in imgui.ini) whatever the title says
    std::snprintf(title, sizeof title, "%s   [Tab / F1: menu, Shift+Tab: HUD]###Menu", brand::kName);
    menu_.draw(title);
    drawHud();
}

void App::setSlowMo(bool on) {
    if (on == slowMo_) return;
    slowMo_ = on;
    if (slowMo_) { savedSpeed_ = speed_; speed_ = 0.25f; } else speed_ = savedSpeed_;
}

void App::clearWorld() {
    particles_->clear();
    bodies_->clear();
    world_->clear();
    gas_->clear();
    electric_->clear();
}

void App::drawHud() {
    if (!menu_.hud) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    const MaterialId m = playerOn_ ? playerMat_ : brush_.material;
    if (m != kEmpty && m < mats_.size()) {
        ImGui::ColorButton("##m", ImGui::ColorConvertU32ToFloat4(toImColor(displayPalette_[m][160])), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
    }
    if (playerOn_) {
        const Inventory& inv = player_->inventory();
        if (creative_) ImGui::Text("%s  |  %s", m ? mats_[m].label.c_str() : "-", toolName(playerIn_.tool));
        else ImGui::Text("%s x %lld  |  %s", m ? mats_[m].label.c_str() : "-", inv.count(m), toolName(playerIn_.tool));
        const float hp = std::max(0.0f, player_->health()) / player_->params().maxHealth;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hp > 0.5f ? ImVec4(0.3f, 0.8f, 0.4f, 1) : hp > 0.25f ? ImVec4(0.9f, 0.7f, 0.2f, 1) : ImVec4(0.9f, 0.25f, 0.2f, 1));
        char hpText[32];
        std::snprintf(hpText, sizeof hpText, "%.0f hp", player_->health());
        ImGui::ProgressBar(hp, ImVec2(ImGui::GetFontSize() * 12.0f, ImGui::GetFontSize() * 0.9f), hpText);
        ImGui::PopStyleColor();
    } else {
        const char* mode = brush_.mode == BrushMode::Heat ? "  heat" : brush_.mode == BrushMode::Cool ? "  cool" : "";
        ImGui::Text("%s  |  brush %d%s", m ? mats_[m].label.c_str() : "-", brush_.radius, mode);
    }
    ImGui::Text("%.0f fps  tick %.2f ms%s", io.Framerate, sim_->stats().tickMs,
                paused_ ? "  PAUSED" : slowMo_ ? "  slow-mo" : "");
    if (!menu_.open) ImGui::TextDisabled("Tab: menu");
    ImGui::End();
}

// ---- Materials, World (time, scenes), Sim (heat), View (overlays, old/new) ----
void App::registerSandboxUi() {
    menu_.add("Materials", "Material", 10, [this] {
        ImGui::BeginChild("##materials", ImVec2(0, ImGui::GetFontSize() * 20.0f), ImGuiChildFlags_Borders);
        for (std::size_t i = 1; i < mats_.size(); ++i) {
            const Material& m = mats_[static_cast<MaterialId>(i)];
            ImGui::PushID(static_cast<int>(i));
            const ImU32 c = toImColor(displayPalette_[i][160]);
            ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(c), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
            ImGui::SameLine();
            char label[96];
            std::snprintf(label, sizeof label, i <= 9 ? "%s  [%zu]" : "%s", m.label.c_str(), i);
            if (ImGui::Selectable(label, brush_.material == m.id)) brush_.material = m.id;
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s, density %.1f%s", kindName(m.kind), m.density,
                            m.kind == MaterialKind::Liquid ? (m.viscosity > 0.5f ? ", viscous" : "") : "");
                ImGui::Text("conductivity %.2f, heat capacity %.2f, placed at %.0f C", m.conductivity, m.heatCapacity, m.spawnTemp);
                if (m.hot.valid) ImGui::Text("above %.0f C -> %s", m.hot.point, mats_[m.hot.into].label.c_str());
                if (m.cold.valid) ImGui::Text("below %.0f C -> %s", m.cold.point, mats_[m.cold.into].label.c_str());
                if (m.flammable()) ImGui::Text("ignites at %.0f C, burns into %s", m.ignitePoint, mats_[m.burnsInto.into].label.c_str());
                for (const auto& e : reactions_->of(m.id)) {
                    const Reaction& r = reactions_->reactions()[e.index];
                    ImGui::Text("+ %s -> %s + %s", mats_[e.partner].label.c_str(), mats_[e.swapped ? r.outB : r.outA].label.c_str(),
                                mats_[e.swapped ? r.outA : r.outB].label.c_str());
                }
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::TextDisabled("1-9 pick the first nine; hover for properties");
    });
    menu_.add("Materials", "Brush", 20, [this] {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 11.0f);
        ImGui::SliderInt("Size", &brush_.radius, 0, 64);
        int shape = brush_.shape == BrushShape::Circle ? 0 : 1;
        if (ImGui::Combo("Shape", &shape, "Circle\0Square\0")) brush_.shape = shape == 0 ? BrushShape::Circle : BrushShape::Square;
        int mode = brush_.mode == BrushMode::Place ? 0 : brush_.mode == BrushMode::Replace ? 1 : brush_.mode == BrushMode::Heat ? 2 : 3;
        if (ImGui::Combo("Mode", &mode, "Fill empty\0Replace all\0Heat (H)\0Cool (J)\0")) {
            const BrushMode modes[] = {BrushMode::Place, BrushMode::Replace, BrushMode::Heat, BrushMode::Cool};
            brush_.mode = modes[mode];
        }
        if (brush_.mode == BrushMode::Heat || brush_.mode == BrushMode::Cool)
            ImGui::SliderFloat("Heat", &brush_.heat, 1.0f, 200.0f, "%.0f C per tick");
        ImGui::SliderFloat("Spray", &brush_.density, 0.05f, 1.0f, "%.2f");
        ImGui::PopItemWidth();
        ImGui::TextDisabled("LMB paint, RMB erase, Shift = replace");
        ImGui::TextDisabled("Ctrl+LMB heat, Ctrl+RMB cool, P paint");
        ImGui::TextDisabled("Wheel / [ ] size, MMB / WASD pan");
    });

    menu_.add("World", "Time", 10, [this] {
        ImGui::Checkbox("Paused (Space)", &paused_);
        ImGui::SameLine();
        if (ImGui::Button("Step (.)")) { paused_ = true; ++stepRequests_; }
        ImGui::SameLine();
        bool slow = slowMo_;
        if (ImGui::Checkbox("Slow motion (T)", &slow)) setSlowMo(slow);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
        ImGui::SliderFloat("Speed", &speed_, 0.1f, 4.0f, "%.2fx");
    });
    menu_.add("World", "Scenes", 20, [this] {
        ButtonFlow b;
        if (b("Showcase (I)")) loadScene("showcase");
        if (b("World (O)")) loadScene("world");
        if (b("Play (L)")) loadScene("play");
        if (b("Demo (R)")) loadScene("demo");
        if (b("Forge (F)")) loadScene("forge");
        if (b("Full churn (B)")) loadScene("churn");
        if (b("Gas lab (V)")) loadScene("gas");
        if (b("Quarry (Q)")) loadScene("quarry");
        if (b("Circuits (U)")) loadScene("circuits");
        if (b("Clear (C)")) clearWorld();
    });

    menu_.add("Sim", "Heat & reactions", 10, [this] {
        SimParams& sp = sim_->params();
        ImGui::Checkbox("Heat", &sp.heat);
        ImGui::SameLine();
        ImGui::Checkbox("Reactions", &sp.reactions);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 11.0f);
        ImGui::SliderFloat("Conduction", &sp.conduction, 0.05f, 1.0f, "%.2f");
        ImGui::SliderFloat("Air cooling", &sp.ambientRate, 0.0f, 0.03f, "%.4f");
        ImGui::PopItemWidth();
        if (gas_->params().enabled) ImGui::TextDisabled("(air cooling is 0 while the gas grid carries the air's heat)");
    });

    menu_.add("View", "Overlays", 10, [this] {
        ImGui::Checkbox("HUD (Shift+Tab)", &menu_.hud);
        ImGui::Checkbox("Temperature view (F5)", &showTemp_);
        ImGui::SameLine();
        ImGui::Checkbox("Glow", &glow_);
        ImGui::Checkbox("Show gas", &showGas_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::Combo("Gas view (F6)", &gasView_, "Gas\0Velocity\0Pressure\0");
        ImGui::Checkbox("Electricity (M)", &showElectric_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::Combo("##ev", &electricView_, "Current\0Potential\0");
    });
    menu_.add("View", "Old / new", 20, [this] {
        bool both = newLook_ && smoothPowder_ && calmLiquid_;
        if (ImGui::Checkbox("New liquid look + motion (F10)", &both)) setNewLookAndMotion(both);
        bool flames = newFlames_;
        if (ImGui::Checkbox("Flames on the gas grid (F11)", &flames)) setNewFlames(flames);
        ImGui::Checkbox("Surface relief + wet look (F12)", &relief_);
        ImGui::TextDisabled("Off = the older look; the sections below tune each one");
    });
}

// ---- Sim: the gas grid ----
void App::registerGasUi() {
    menu_.add("Sim", "Gas grid", 20, [this] {
        GasGridParams& gp = gas_->params();
        GasSolveParams& sp = gas_->solveParams();
        const GasGridStats& gs = gas_->stats();
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::Checkbox("Gas grid on (G)", &gp.enabled);
        if (gpuGas_) {
            ImGui::SameLine();
            if (ImGui::Checkbox("GPU solver", &useGpuGas_)) gas_->setSolver(useGpuGas_ ? gpuGas_.get() : nullptr);
        } else {
            ImGui::TextDisabled("GPU solver unavailable: %s", gpuGasError_.c_str());
        }
        ImGui::SliderInt("Pressure iterations", &sp.pressureIters, 4, 120);
        ImGui::SliderFloat("Heat buoyancy", &sp.buoyancyHeat, 0.0f, 0.004f, "%.4f");
        ImGui::SliderFloat("Gas buoyancy", &sp.buoyancyDensity, 0.0f, 0.05f, "%.3f");
        ImGui::SliderFloat("Vorticity", &sp.vorticity, 0.0f, 0.5f, "%.2f");
        ImGui::SliderFloat("Damping", &sp.damping, 0.95f, 1.0f, "%.3f");
        ImGui::SliderFloat("Air exchange", &gp.airExchange, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Loss to ambient", &gp.ambientLoss, 0.0f, 0.02f, "%.4f");
        ImGui::SliderFloat("Push on pixels", &gp.push, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Blast power", &blastPower_, 0.2f, 4.0f, "%.1f");
        ImGui::SliderInt("Blast radius", &blastRadius_, 4, 40);
        ImGui::PopItemWidth();
        ImGui::TextDisabled("X: blast at the cursor");
        const double grams = static_cast<double>(gas_->massFixed()) / kGasMassOne;
        ImGui::Text("%s solver %.2f ms (%s), coupling %.2f ms", gas_->solver().name(), gs.solveMs,
                    gpuGas_ && useGpuGas_ ? "upload+kernels+download" : "threads", gs.coupleMs);
        ImGui::Text("grid %dx%d, %.0f mass units of gas", gas_->fields().w, gas_->fields().h, grams);
        ImGui::Text("max speed %.2f px/tick, max |p| %.2f", gs.maxSpeed * kGasCell, gs.maxPressure);
        ImGui::Text("in %lld, condensed %lld, burnt %lld, pushed %d px, %d shoves", gs.absorbed, gs.condensed, gs.burned, gs.pushed, gs.shoved);
    });
}

// ---- Sim: particles, bodies ----
void App::registerBodiesUi() {
    menu_.add("Sim", "Particles", 50, [this] {
        SimParams& sp = sim_->params();
        ParticleParams& pp = particles_->params();
        const ParticleStats& ps = particles_->stats();
        ImGui::Checkbox("Splash particles", &particlesOn_);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::SliderFloat("Splash speed", &sp.splashSpeed, 2.0f, 12.0f, "%.1f px/tick");
        ImGui::SliderFloat("Restitution", &sp.restitution, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Mix rate", &sp.mixRate, 0.0f, 0.5f, "%.2f");
        ImGui::SliderFloat("Wind on particles", &pp.gasDrag, 0.0f, 0.5f, "%.2f");
        ImGui::PopItemWidth();
        ImGui::Text("particles %d (+%d, settled %d, waiting %d) %.2f ms", ps.count, ps.spawned, ps.settled, ps.waiting, ps.stepMs);
        ImGui::Text("splashed %d px, momentum out/in %.2f", sim_->stats().splashed,
                    sim_->stats().splashMomentumIn > 0 ? sim_->stats().splashMomentumOut / sim_->stats().splashMomentumIn : 0.0);
    });
    menu_.add("Sim", "Rigid bodies", 60, [this] {
        BodyParams& bp = bodies_->params();
        const BodyStats& bs = bodies_->stats();
        ImGui::Checkbox("Rigid bodies", &bp.enabled);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::SliderFloat("Shatter speed", &bp.shatterSpeed, 1.0f, 20.0f, "%.1f px/tick");
        ImGui::SliderInt("Min body px", &bp.minPixels, 1, 64);
        ImGui::SliderFloat("Wind on bodies", &bp.gasDrag, 0.0f, 0.2f, "%.3f");
        ImGui::SliderFloat("Pressure on bodies", &bp.gasPush, 0.0f, 4.0f, "%.2f");
        ImGui::PopItemWidth();
        ImGui::TextDisabled("K: cut the clump under the cursor loose, X: blast");
        ImGui::Text("bodies %d (%d awake, %d px, %d hidden) %.2f ms", bs.bodies, bs.awake, bs.pixels, bs.hidden, bs.stepMs);
        ImGui::Text("  search %.2f ms, box2d %.2f ms, %d terrain boxes", bs.findMs, bs.solveMs, bs.terrainShapes);
        ImGui::Text("  this tick: +%d, frozen %d, shattered %d, shoved %d px", bs.created, bs.frozen, bs.shattered, bs.displaced);
    });
}

// ---- Sim: electricity ----
void App::registerElectricUi() {
    menu_.add("Sim", "Electricity", 40, [this] {
        const ElectricStats& es = electric_->stats();
        ElectricParams& ep = electric_->params();
        ImGui::Checkbox("Electricity on", &ep.enabled);
        ImGui::SameLine();
        ImGui::Checkbox("Overlay (M)", &showElectric_);
        ImGui::Text("%d live px in %d circuits (%d solved, %d settling), %d CG iterations", es.nodes, es.components, es.solved,
                    es.unsettled, es.iterations);
        ImGui::Text("power %.1f, heat %.1f, %d lamp px lit, max %.0f V, %.2f ms", es.power, es.heat, es.lit,
                    static_cast<double>(electric_->maxPotential()), es.ms);
        ImGui::Text("sparks %d, splits %d (total %lld)", es.sparks, es.splits, es.splitsTotal);
        if (playerOn_ && player_->stats().shock > 0.0f) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "shock! %.2f", player_->stats().shock);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 11.0f);
        ImGui::SliderFloat("Heat gain", &ep.heatGain, 0.0f, 2.0f, "%.2f");
        ImGui::SliderInt("CG iterations / tick", &ep.maxIterations, 1, 512);
        ImGui::SliderFloat("Spark power", &ep.sparkPower, 0.2f, 20.0f, "%.1f");
        ImGui::SliderFloat("Electrolysis rate", &ep.splitRate, 0.0f, 0.002f, "%.5f");
        ImGui::PopItemWidth();
        ImGui::TextDisabled("E flip switch (cursor / in reach), Y lightning at the cursor's column");
        ImGui::TextDisabled("U circuits scene: volt cell -> wire -> lamp / heater -> ground rod");
    });
}

// ---- Player and inventory ----
void App::registerPlayerUi() {
    menu_.add("Player", "", 10, [this] {
        if (!playerOn_) {
            ImGui::TextDisabled("No player. L: play scene, F7: spawn at the cursor");
            ImGui::Checkbox("Creative (F2)", &creative_);
            return;
        }
        const PlayerStats& ps = player_->stats();
        const PlayerParams& pp = player_->params();
        Inventory& inv = player_->inventory();
        const float hp = std::max(0.0f, player_->health()) / pp.maxHealth;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hp > 0.5f ? ImVec4(0.3f, 0.8f, 0.4f, 1) : hp > 0.25f ? ImVec4(0.9f, 0.7f, 0.2f, 1) : ImVec4(0.9f, 0.25f, 0.2f, 1));
        char hpText[32];
        std::snprintf(hpText, sizeof hpText, "%.0f hp", player_->health());
        ImGui::ProgressBar(hp, ImVec2(ImGui::GetFontSize() * 14.0f, 0), hpText);
        ImGui::PopStyleColor();
        ImGui::Checkbox("Creative (F2)", &creative_);
        ImGui::SameLine();
        ImGui::Checkbox("Build fixed (Z)", &placePinned_);
        ImGui::SeparatorText("Tool (1-6, LMB)");
        for (int t = 0; t < static_cast<int>(Tool::Count); ++t) {
            char label[48];
            std::snprintf(label, sizeof label, "%d %s", t + 1, toolName(static_cast<Tool>(t)));
            if (t > 0 && t != 3) ImGui::SameLine();
            if (ImGui::RadioButton(label, playerIn_.tool == static_cast<Tool>(t))) playerIn_.tool = static_cast<Tool>(t);
        }
        switch (playerIn_.tool) {
        case Tool::Dig: ImGui::TextDisabled("dig under the brush into the inventory"); break;
        case Tool::Jet: {
            const MaterialId g = player_->jetGas(playerMat_);
            ImGui::TextDisabled("boils or burns the material in hand (or any that works) into gas");
            ImGui::TextDisabled("thrust away from the cursor; %s", g ? ("in hand makes " + mats_[g].label).c_str() : "in hand doesn't work");
            break;
        }
        case Tool::Grapple: ImGui::TextDisabled("click: rope to the cursor / let go; W reel in, S let out"); break;
        case Tool::Blast: ImGui::TextDisabled("click: set off a charge (aim at your feet to jump)"); break;
        case Tool::Raft: ImGui::TextDisabled("click: a %dx%d raft of the solid in hand (timber floats)", pp.raftW, pp.raftH); break;
        case Tool::Freeze: ImGui::TextDisabled("hold: freeze water to walk on"); break;
        default: break;
        }
        ImGui::SeparatorText("In hand (wheel, RMB places)");
        if (playerMat_ != kEmpty) {
            ImGui::ColorButton("##hand", ImGui::ColorConvertU32ToFloat4(toImColor(displayPalette_[playerMat_][160])), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
            ImGui::SameLine();
            if (creative_) ImGui::Text("%s (unlimited)", mats_[playerMat_].label.c_str());
            else ImGui::Text("%s x %lld", mats_[playerMat_].label.c_str(), inv.count(playerMat_));
        }
        if (creative_) {
            ImGui::TextDisabled("creative: every material, charges unlimited");
            // the full palette, like the Materials tab
            ImGui::BeginChild("##all", ImVec2(0, ImGui::GetFontSize() * 10.0f), ImGuiChildFlags_Borders);
            for (std::size_t i = 1; i < mats_.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(mats_[static_cast<MaterialId>(i)].label.c_str(), playerMat_ == i)) playerMat_ = static_cast<MaterialId>(i);
                ImGui::PopID();
            }
            ImGui::EndChild();
        } else {
            ImGui::Text("carrying %lld / %lld px, %d charges", inv.total(), inv.capacity, inv.charges);
            ImGui::BeginChild("##inv", ImVec2(0, ImGui::GetFontSize() * 10.0f), ImGuiChildFlags_Borders);
            for (std::size_t i = 1; i < mats_.size(); ++i) {
                const long long n = inv.count(static_cast<MaterialId>(i));
                if (n <= 0) continue;
                ImGui::PushID(static_cast<int>(i));
                ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(toImColor(displayPalette_[i][160])), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                ImGui::SameLine();
                char label[96];
                std::snprintf(label, sizeof label, "%s  %lld", mats_[static_cast<MaterialId>(i)].label.c_str(), n);
                if (ImGui::Selectable(label, playerMat_ == i)) playerMat_ = static_cast<MaterialId>(i);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::SeparatorText("Feel");
        ImGui::Text("%s, in liquid %.0f%%, wind (%.1f, %.1f) px/tick", player_->grounded() ? "on the ground" : "in the air", ps.submerged * 100.0f,
                    ps.windX, ps.windY);
        ImGui::Text("hottest nearby %.0f C, jet gas %lld units, deaths %d", ps.hottest, ps.jetMassTotal, ps.deaths);
        if (player_->roped()) ImGui::Text("rope %.0f px%s", player_->ropeLength(), player_->ropeBody() ? " (to a body)" : "");
        ImGui::TextDisabled("A/D walk, W/Space jump & swim (hold in a fall to glide), S dive");
        ImGui::TextDisabled("P pause, T slow-mo, F7 leave");
    });
}

// ---- World: the streamed world, save / load ----
void App::registerWorldUi() {
    menu_.add("World", "Streamed world & saves", 30, [this] {
        if (!streaming_) {
            ImGui::TextDisabled("Fixed sandbox world: the streamed procedural world is O");
            if (ImGui::Button("Open the world (O)")) loadScene("world");
            ImGui::SameLine();
            if (ImGui::Button("Load save (F9)")) loadWorld();
            if (!worldMsg_.empty()) ImGui::TextWrapped("%s", worldMsg_.c_str());
            return;
        }
        const StreamStats& st = stream_->stats();
        const int fx = static_cast<int>(std::floor(playerOn_ ? player_->x() : camX_ + kViewW * 0.5f));
        if (gen_->overlay() == showcase_.get()) {
            const int fy = static_cast<int>(std::floor(playerOn_ ? player_->centreY() : camY_ + kViewH * 0.5f));
            const char* here = "outside";
            for (const Showcase::Room& r : showcase_->rooms())
                if (fx >= r.x0 && fx <= r.x1 && fy >= r.y0 && fy <= r.y1) { here = r.name.c_str(); break; }
            ImGui::Text("Showcase: %s   (I restarts it, F8/F9 save/load)", here);
        }
        ImGui::Text("Seed %llu   biome %s   surface y %d", static_cast<unsigned long long>(gen_->seed()), biomeName(gen_->biome(fx)),
                    gen_->surfaceY(fx));
        ImGui::Text("Awake window: chunks (%d, %d) .. (%d, %d)", stream_->windowX(), stream_->windowY(),
                    stream_->windowX() + stream_->params().chunksW - 1, stream_->windowY() + stream_->params().chunksH - 1);
        ImGui::Text("Sleeping: %zu chunks, %.1f KiB encoded", stream_->storedChunks(), static_cast<double>(st.storeBytes) / 1024.0);
        ImGui::Text("Shifts %d (last %.2f ms)  generated %d  woken %d  slept %d", st.shifts, st.lastShiftMs, st.generated, st.woken, st.slept);
        const long long live = world_->totalMass(mats_) + particles_->totalMass() + bodies_->totalMass() +
                               (playerOn_ ? player_->inventory().totalMass(mats_) : 0);
        ImGui::Text("Mass: live %lld + sleeping %lld px, gas %.1f + %.1f", live, stream_->storedMass(),
                    static_cast<double>(gas_->massFixed()) / kGasMassOne, static_cast<double>(stream_->storedGasFixed()) / kGasMassOne);
        if (ImGui::Button("Save (F8)")) saveWorld();
        ImGui::SameLine();
        if (ImGui::Button("Load (F9)")) loadWorld();
        ImGui::SameLine();
        if (ImGui::Button("New world")) startWorld();
        ImGui::TextDisabled("%s save: %s", brand::kName, savePath_.c_str());
        if (!worldMsg_.empty()) ImGui::TextWrapped("%s", worldMsg_.c_str());
    });
}

// ---- Performance, Debug ----
void App::registerStatsUi() {
    menu_.add("Performance", "Frame & tick", 10, [this] {
        const ImGuiIO& io = ImGui::GetIO();
        const SimStats& st = sim_->stats();
        float avg = 0, peak = 0;
        for (float v : tickHistory_) { avg += v; peak = std::max(peak, v); }
        avg /= static_cast<float>(tickHistory_.size());
        ImGui::Text("%.0f fps  frame %.2f ms  %d tick(s) this frame", io.Framerate, frameMs_, ticksThisFrame_);
        ImGui::Text("tick %.3f ms  (avg %.3f, max %.3f over %zu ticks)", st.tickMs, avg, peak, tickHistory_.size());
        ImGui::PlotLines("##ticks", tickHistory_.data(), static_cast<int>(tickHistory_.size()), tickHistoryPos_, nullptr, 0.0f,
                         std::max(2.0f, peak * 1.2f), ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 4.0f));
        ImGui::Text("chunks %d / %d moving, %d heat", st.activeChunks, st.totalChunks, st.heatChunks);
        ImGui::Text("events %llu (changes, burns, reactions)", static_cast<unsigned long long>(st.events));
        ImGui::Text("cells moved/visited %llu", static_cast<unsigned long long>(st.cellsProcessed));
        ImGui::Text("threads %d", pool_->threadCount());
    });
    menu_.add("Performance", "Per system", 20, [this] {
        if (!ImGui::BeginTable("##sys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) return;
        auto row = [](const char* name, const char* fmt, double v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::Text(fmt, v);
        };
        const GasGridStats& gs = gas_->stats();
        const BodyStats& bs = bodies_->stats();
        row("cells (sim tick)", "%.3f ms", sim_->stats().tickMs);
        row("electricity", "%.3f ms", electric_->stats().ms);
        row("rigid bodies", "%.3f ms", bs.stepMs);
        row("  search / box2d", "%.3f ms", bs.findMs + bs.solveMs);
        row("particles", "%.3f ms", particles_->stats().stepMs);
        row(gpuGas_ && useGpuGas_ ? "gas solve (GPU, with copies)" : "gas solve (CPU)", "%.3f ms", gs.solveMs);
        row("gas coupling", "%.3f ms", gs.coupleMs);
        row("light inputs (CPU)", "%.3f ms", lightInputMs_);
        if (!(useGpuLight_ && gpuLight_)) row("light pass (CPU)", "%.3f ms", cpuLight_.lastMs());
        else if (lightGpuMs_ > 0) row("light pass (GPU, measured)", "%.3f ms", lightGpuMs_);
        if (streaming_) row("last stream shift", "%.3f ms", stream_->stats().lastShiftMs);
        ImGui::EndTable();
        hint("View > Lighting measures the GPU light pass; notita_bench runs the full benchmark.");
    });

    menu_.add("Debug", "State", 10, [this] {
        const SimStats& st = sim_->stats();
        ImGui::TextDisabled("%s %s", brand::kName, brand::kVersion);
        ImGui::Text("tick #%llu  threads %d  seed %llu", static_cast<unsigned long long>(st.tick), pool_->threadCount(),
                    static_cast<unsigned long long>(sim_->seed()));
        ImGui::Text("%s", paused_ ? "PAUSED" : (slowMo_ ? "slow motion (T)" : speed_ != 1.0f ? "speed changed" : "60 Hz fixed tick"));
        ImGui::Text("particles %d  bodies %d (%d px)", static_cast<int>(particles_->count()), bodies_->stats().bodies, bodies_->stats().pixels);
        if (playerOn_)
            ImGui::Text("player (%.1f, %.1f) v (%.2f, %.2f) %s", player_->x(), player_->y(), player_->vx(), player_->vy(),
                        player_->grounded() ? "grounded" : "airborne");
        ImGui::Checkbox("Chunks (F3)", &showChunks_);
        ImGui::SameLine();
        ImGui::Checkbox("Dirty rects (Shift+F4)", &showDirty_);
        if (!materialErrors_.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%zu material/reaction error(s):", materialErrors_.size());
            for (const std::string& e : materialErrors_) ImGui::BulletText("%s", e.c_str());
        }
    });
    menu_.add("Debug", "Under the cursor", 20, [this] {
        int wx, wy;
        if (!screenToWorld(mouseX_, mouseY_, wx, wy)) {
            ImGui::TextDisabled("(cursor outside the view)");
            return;
        }
        if (const Cell* c = world_->cell(wx, wy))
            ImGui::Text("(%d, %d) %s%s%s%s  %.1f C  vy %.2f", wx, wy, mats_[c->material].label.c_str(),
                        (c->flags & kFlagBurning) ? " (burning)" : "", (c->flags & kFlagBody) ? " (body)" : "",
                        c->aux == kAuxPinned ? " (pinned)" : c->aux == kAuxSettled ? " (settled)" : "", c->temperature, c->vy / 256.0);
        const int i = (wx - gas_->originX()) / kGasCell, j = (wy - gas_->originY()) / kGasCell;
        const GasFields& f = gas_->fields();
        if (i >= 0 && j >= 0 && i < f.w && j < f.h) {
            const int c = j * f.w + i;
            ImGui::Text("gas cell (%d, %d): %.0f C, p %.3f, v (%.2f, %.2f)", i, j, gas_->temperature(c), f.p[c], f.uc(i, j), f.vc(i, j));
            for (int k = 0; k < f.species; ++k)
                if (f.m(k, c) > 0) ImGui::Text("  %s %.2f", mats_[gas_->species(k)].label.c_str(), static_cast<double>(f.m(k, c)) / kGasMassOne);
        }
    });
    menu_.add("Debug", "Keys", 30, [] {
        hint("Tab / F1 menu, Shift+Tab / Shift+F1 HUD, Esc quit");
        hint("Space pause, . / N step, T slow-mo, C clear");
        hint("Scenes: I showcase, O world, L play, R demo, F forge, B churn, V gas, Q quarry, U circuits");
        hint("F2 creative, F7 player in/out, F8 save, F9 load");
        hint("F3 chunks, F4 light (Shift: dirty rects), F5 temperature, F6 gas view, M electricity");
        hint("F10 old/new liquids, F11 old/new flames, F12 relief, G gas grid");
        hint("E switch, Y lightning, K cut clump loose, X blast, H heat / J cool / P paint brush");
        hint("Playing: A/D walk, W/Space jump, S dive, 1-6 tools, P pause, Z build fixed, wheel material");
    });
}

} // namespace notita
