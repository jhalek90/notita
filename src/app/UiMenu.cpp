#include "UiMenu.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiSettingsHandler, MarkIniSettingsDirty

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace notita {

UiMenu::Tab& UiMenu::tab(const std::string& name) {
    for (Tab& t : tabs_)
        if (t.name == name) return t;
    tabs_.push_back(Tab{name, 1000, {}});
    return tabs_.back();
}

void UiMenu::addTab(const std::string& name, int order) {
    tab(name).order = order;
    std::stable_sort(tabs_.begin(), tabs_.end(), [](const Tab& a, const Tab& b) { return a.order < b.order; });
}

void UiMenu::add(const std::string& tabName, const std::string& section, int order, DrawFn draw) {
    Tab& t = tab(tabName);
    t.sections.push_back(Section{section, order, std::move(draw)});
    std::stable_sort(t.sections.begin(), t.sections.end(), [](const Section& a, const Section& b) { return a.order < b.order; });
    std::stable_sort(tabs_.begin(), tabs_.end(), [](const Tab& a, const Tab& b) { return a.order < b.order; });
}

std::vector<std::string> UiMenu::tabNames() const {
    std::vector<std::string> out;
    for (const Tab& t : tabs_) out.push_back(t.name);
    return out;
}

void UiMenu::markDirty() {
    if (ImGui::GetCurrentContext()) ImGui::MarkIniSettingsDirty();
}

// imgui.ini:  [FineGrainMenu][Menu]  Tab=Sim  Open=1  Hud=1
void UiMenu::installSettings() {
    ImGuiSettingsHandler h;
    h.TypeName = "FineGrainMenu";
    h.TypeHash = ImHashStr("FineGrainMenu");
    h.UserData = this;
    h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* hd, const char*) -> void* { return hd->UserData; };
    h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
        auto* m = static_cast<UiMenu*>(entry);
        int v = 0;
        if (!std::strncmp(line, "Tab=", 4)) {
            if (!m->pending_.empty()) return; // a tab picked before the load (--tab) wins
            m->pending_ = line + 4;
            while (!m->pending_.empty() && (m->pending_.back() == '\r' || m->pending_.back() == '\n')) m->pending_.pop_back();
        } else if (std::sscanf(line, "Open=%d", &v) == 1) {
            m->open = m->lastOpen_ = v != 0;
        } else if (std::sscanf(line, "Hud=%d", &v) == 1) {
            m->hud = m->lastHud_ = v != 0;
        }
    };
    h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* hd, ImGuiTextBuffer* out) {
        const auto* m = static_cast<const UiMenu*>(hd->UserData);
        out->appendf("[%s][Menu]\n", hd->TypeName);
        if (!m->current_.empty()) out->appendf("Tab=%s\n", m->current_.c_str());
        out->appendf("Open=%d\nHud=%d\n\n", m->open ? 1 : 0, m->hud ? 1 : 0);
    };
    ImGui::AddSettingsHandler(&h);
}

void UiMenu::draw(const char* title) {
    if (open != lastOpen_ || hud != lastHud_) {
        lastOpen_ = open;
        lastHud_ = hud;
        markDirty();
    }
    if (!open) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(em * 40.0f, em * 44.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(em * 18.0f, em * 12.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (Tab& t : tabs_) {
            const ImGuiTabItemFlags f = pending_ == t.name ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(t.name.c_str(), nullptr, f)) continue;
            if (current_ != t.name) { current_ = t.name; markDirty(); }
            // the tabs stay put, the body scrolls
            ImGui::BeginChild("##body", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
            for (Section& s : t.sections) {
                ImGui::PushID(s.title.c_str());
                if (s.title.empty() || ImGui::CollapsingHeader(s.title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    s.draw();
                    if (!s.title.empty()) ImGui::Spacing();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // a saved tab that no longer exists, or one picked from outside: drop it once shown
        bool known = false;
        for (const Tab& t : tabs_) known |= t.name == pending_;
        if (!known || pending_ == current_) pending_.clear();
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace notita
