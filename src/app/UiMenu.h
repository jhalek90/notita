#pragma once
// The one ImGui menu window (#484): tabs, each made of sections that the
// systems register themselves. The menu knows nothing about the sim; a new
// feature adds its tab or section with add() from its own register function
// (see App::registerUi) and never touches this file.
#include <functional>
#include <string>
#include <vector>

namespace notita {

class UiMenu {
public:
    using DrawFn = std::function<void()>;

    // A tab, placed by `order` (low first). Optional: add() makes a missing tab at order 1000.
    void addTab(const std::string& tab, int order);
    // A section of a tab, placed by `order` inside it. A titled section gets a collapsing
    // header (open by default); an untitled one is drawn as is.
    void add(const std::string& tab, const std::string& section, int order, DrawFn draw);

    // After ImGui::CreateContext and before the first frame: keeps the open tab, whether the
    // menu and the HUD are shown in imgui.ini (the window's place and size are ImGui's own).
    void installSettings();
    void draw(const char* title); // the window, when open
    void select(const std::string& tab) { pending_ = tab; } // switch to this tab next frame
    const std::string& current() const { return current_; }
    std::vector<std::string> tabNames() const;

    bool open = true; // the menu window (Tab / F1)
    bool hud = true;  // the small always-on HUD (Shift+Tab / Shift+F1)

private:
    struct Section { std::string title; int order; DrawFn draw; };
    struct Tab { std::string name; int order; std::vector<Section> sections; };
    Tab& tab(const std::string& name);
    void markDirty(); // imgui.ini gets saved
    std::vector<Tab> tabs_;
    std::string current_, pending_;
    bool lastOpen_ = true, lastHud_ = true;
};

} // namespace notita
