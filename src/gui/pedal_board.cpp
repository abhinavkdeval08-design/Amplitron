#include "gui/pedal_board.h"
#include "gui/pedal_widget.h"
#include "gui/theme.h"
#include "gui/command.h"

#include "audio/effects/noise_gate.h"
#include "audio/effects/compressor.h"
#include "audio/effects/overdrive.h"
#include "audio/effects/distortion.h"
#include "audio/effects/equalizer.h"
#include "audio/effects/chorus.h"
#include "audio/effects/delay.h"
#include "audio/effects/reverb.h"
#include "audio/effects/cabinet_sim.h"
#include "audio/effects/amp_simulator.h"
#include <cstring>
#include <cstdio>
#include <set>

#include <imgui.h>

namespace GuitarAmp {

/** @brief Construct PedalBoard and build initial widget list from engine state. */
PedalBoard::PedalBoard(AudioEngine& engine, CommandHistory& history)
    : engine_(engine), history_(history) {
    rebuild_widgets();
}

/** @brief Default destructor. */
PedalBoard::~PedalBoard() = default;

/** @brief Update which pedals are visible based on current state. */
void PedalBoard::update_visible_pedals() {
    visible_indices_.clear();
    auto& effects = engine_.effects();
    
    // Always show EQ and Reverb if they exist
    for (int i = 0; i < static_cast<int>(effects.size()); ++i) {
        const char* name = effects[i]->name();
        if (std::strcmp(name, "Equalizer") == 0 || std::strcmp(name, "Reverb") == 0) {
            visible_indices_.insert(i);
        }
    }
    
    // Add any other pedals that were explicitly added (not just enabled)
    // We'll track this in rebuild_widgets() when new effects are added
}

/** @brief Recreate PedalWidget list to match the engine's current effect chain. */
void PedalBoard::rebuild_widgets() {
    // Remember which effects were visible before rebuild
    std::set<Effect*> visible_ptrs;
    for (int idx : visible_indices_) {
        if (idx < static_cast<int>(widgets_.size())) {
            visible_ptrs.insert(widgets_[idx]->get_effect().get());
        }
    }
    
    widgets_.clear();
    auto& effects = engine_.effects();
    for (int i = 0; i < static_cast<int>(effects.size()); ++i) {
        auto w = std::make_unique<PedalWidget>(engine_, effects[i], i);
        w->set_history(&history_);
        widgets_.push_back(std::move(w));
    }
    
    // Restore visibility and add any new effects
    update_visible_pedals();
    for (int i = 0; i < static_cast<int>(effects.size()); ++i) {
        const char* name = effects[i]->name();
        bool is_amp = (std::strcmp(name, "Amp Sim") == 0);
        bool is_special = (std::strcmp(name, "Equalizer") == 0 || std::strcmp(name, "Reverb") == 0);
        bool was_visible = visible_ptrs.count(effects[i].get()) > 0;
        
        // Always show amps, EQ, and Reverb
        // Show other effects if they were visible before
        if (is_amp || is_special || was_visible) {
            visible_indices_.insert(i);
        }
    }
}

/** @brief Find the index of the current AmpSimulator in the effect chain (-1 if none). */
int PedalBoard::find_amp_index() const {
    auto& fx = engine_.effects();
    for (int i = 0; i < static_cast<int>(fx.size()); ++i) {
        if (std::strcmp(fx[i]->name(), "Amp Sim") == 0) return i;
    }
    return -1;
}

/** @brief Render the toolbar (add/reset) and the scrollable signal chain area. */
void PedalBoard::render() {
    ImGui::BeginChild("PedalToolbar", ImVec2(0, 35), true);
    render_add_pedal_menu();
    ImGui::SameLine();

    if (ImGui::Button("Reset All")) {
        for (auto& fx : engine_.effects()) {
            fx->reset();
            auto& p = fx->params();
            for (auto& param : p) {
                param.value = param.default_val;
            }
        }
    }
    ImGui::SameLine();

    if (ImGui::Button("Clear All")) {
        if (!engine_.effects().empty()) {
            history_.execute(std::make_unique<ClearAllCommand>(engine_));
            rebuild_widgets();
        }
    }
    ImGui::SameLine();

    ImGui::SameLine();

    // Amp selector (separate dropdown to switch model)
    render_amp_selector();

    ImGui::SameLine();
    int pedal_count = static_cast<int>(engine_.effects().size());
    ImGui::TextColored(Theme::TextSecondary(),
        "  %d effects | Drag knobs to adjust", pedal_count);

    ImGui::EndChild();

    // Pedal board area with horizontal scroll
    ImGui::BeginChild("PedalArea", ImVec2(0, 0), true,
        ImGuiWindowFlags_HorizontalScrollbar);

    render_signal_chain();

    ImGui::EndChild();
}

/** @brief Add an effect to the chain via undo system, rebuild widgets, and make it visible. */
void PedalBoard::add_effect_and_show(std::shared_ptr<Effect> effect) {
    auto effect_ptr = effect.get();
    history_.execute(std::make_unique<AddEffectCommand>(engine_, std::move(effect)));
    rebuild_widgets();
    
    // Find the newly added effect and make it visible
    auto& effects = engine_.effects();
    for (int i = 0; i < static_cast<int>(effects.size()); ++i) {
        if (effects[i].get() == effect_ptr) {
            visible_indices_.insert(i);
            break;
        }
    }
}

/** @brief Render the "+ Add Pedal" button and category popup with effect menu items.
 *  Amps and Tuner are handled separately (amp selector dropdown, tuner modal). */
void PedalBoard::render_add_pedal_menu() {
    if (ImGui::Button("+ Add Pedal")) {
        ImGui::OpenPopup("AddPedalPopup");
    }

    if (ImGui::BeginPopup("AddPedalPopup")) {
        ImGui::TextColored(Theme::Gold(), "DRIVE");
        if (ImGui::MenuItem("Overdrive")) {
            add_effect_and_show(std::make_shared<Overdrive>());
        }
        if (ImGui::MenuItem("Distortion")) {
            add_effect_and_show(std::make_shared<Distortion>());
        }

        ImGui::Separator();
        ImGui::TextColored(Theme::Live(), "DYNAMICS");
        if (ImGui::MenuItem("Noise Gate")) {
            add_effect_and_show(std::make_shared<NoiseGate>());
        }
        if (ImGui::MenuItem("Compressor")) {
            add_effect_and_show(std::make_shared<Compressor>());
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.35f, 0.60f, 0.95f, 1.0f), "MODULATION");
        if (ImGui::MenuItem("Chorus")) {
            add_effect_and_show(std::make_shared<Chorus>());
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.65f, 0.35f, 0.95f, 1.0f), "TIME");
        if (ImGui::MenuItem("Delay")) {
            add_effect_and_show(std::make_shared<Delay>());
        }
        if (ImGui::MenuItem("Reverb")) {
            add_effect_and_show(std::make_shared<Reverb>());
        }

        ImGui::Separator();
        ImGui::TextColored(Theme::GoldDim(), "TONE");
        if (ImGui::MenuItem("Equalizer")) {
            add_effect_and_show(std::make_shared<Equalizer>());
        }
        if (ImGui::MenuItem("Cabinet Sim")) {
            add_effect_and_show(std::make_shared<CabinetSim>());
        }

        ImGui::EndPopup();
    }
}

/** @brief Render the amp model selector dropdown (max 1 amp, always present). */
void PedalBoard::render_amp_selector() {
    const auto& models = get_amp_models();
    int amp_idx = find_amp_index();

    // Auto-add default amp if none exists
    if (amp_idx < 0) {
        auto amp = std::make_shared<AmpSimulator>();
        amp->params()[0].value = 0.0f; // first model
        engine_.add_effect(amp);
        rebuild_widgets();
        amp_idx = find_amp_index();
    }

    // Current model label
    const char* current_label = "Amp";
    int current_model = 0;
    if (amp_idx >= 0) {
        auto& amp_fx = engine_.effects()[amp_idx];
        current_model = static_cast<int>(amp_fx->params()[0].value);
        if (current_model >= 0 && current_model < static_cast<int>(models.size())) {
            current_label = models[current_model].name;
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.18f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.25f, 0.10f, 1.0f));
    char amp_label[64];
    std::snprintf(amp_label, sizeof(amp_label), "Amp: %s", current_label);
    if (ImGui::Button(amp_label)) {
        ImGui::OpenPopup("AmpSelectorPopup");
    }
    ImGui::PopStyleColor(2);

    if (ImGui::BeginPopup("AmpSelectorPopup")) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.20f, 1.0f), "AMP MODEL");
        for (int m = 0; m < static_cast<int>(models.size()); ++m) {
            bool is_selected = (current_model == m);
            if (ImGui::MenuItem(models[m].name, models[m].inspiration, is_selected)) {
                if (amp_idx >= 0) {
                    engine_.effects()[amp_idx]->params()[0].value = static_cast<float>(m);
                }
            }
        }
        ImGui::EndPopup();
    }
}

/** @brief Draw the signal flow line, render each pedal widget, and handle drag-and-drop reordering.
 *  Uses visibility set to determine which pedals to show. */
void PedalBoard::render_signal_chain() {
    // Build list of visible widget indices from visibility set
    std::vector<int> visible;
    for (int idx : visible_indices_) {
        if (idx >= 0 && idx < static_cast<int>(widgets_.size())) {
            visible.push_back(idx);
        }
    }

    if (visible.empty()) {
        ImGui::SetCursorPos(ImVec2(
            ImGui::GetWindowWidth() / 2 - 150,
            ImGui::GetWindowHeight() / 2 - 30
        ));
        ImGui::TextColored(Theme::TextDim(),
            "No pedals in chain.\nClick '+ Add Pedal' to get started.");
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    // Draw signal flow line
    float line_y = origin.y + 160;
    float total_width = visible.size() * 195.0f + 40;
    dl->AddLine(
        ImVec2(origin.x, line_y),
        ImVec2(origin.x + total_width, line_y),
        Theme::CHAIN_LINE, 3.0f
    );

    // Input jack
    dl->AddCircleFilled(ImVec2(origin.x + 5, line_y), 6, Theme::CHAIN_JACK);
    dl->AddCircle(ImVec2(origin.x + 5, line_y), 6, Theme::BORDER_DARK, 0, 1.5f);

    // Render each visible pedal
    float pedal_x = origin.x + 20;
    int remove_idx = -1;

    for (int vi = 0; vi < static_cast<int>(visible.size()); ++vi) {
        int i = visible[vi];
        ImGui::SetCursorScreenPos(ImVec2(pedal_x, origin.y + 5));

        if (widgets_[i]->render()) {
            remove_idx = i;
        }

        // Drag-and-drop reordering
        ImVec2 pedal_min = ImVec2(pedal_x, origin.y + 5);
        ImGui::SetCursorScreenPos(pedal_min);
        char dnd_id[32];
        snprintf(dnd_id, sizeof(dnd_id), "##dnd_%d", i);
        ImGui::InvisibleButton(dnd_id, ImVec2(Theme::PEDAL_WIDTH, Theme::PEDAL_HEIGHT));

        bool is_amp = std::strcmp(widgets_[i]->get_effect()->name(), "Amp Sim") == 0;
        if (!is_amp) {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("PEDAL_REORDER", &i, sizeof(int));
                ImGui::Text("Move %s", widgets_[i]->get_effect()->name());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PEDAL_REORDER")) {
                    int source_idx = *static_cast<const int*>(payload->Data);
                    if (source_idx != i) {
                        history_.execute(std::make_unique<ReorderEffectCommand>(engine_, source_idx, i));
                        rebuild_widgets();
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        // Connection dot between pedals
        if (vi < static_cast<int>(visible.size()) - 1) {
            float dot_x = pedal_x + 190;
            dl->AddCircleFilled(ImVec2(dot_x, line_y), 4, Theme::CHAIN_DOT);
        }

        pedal_x += 195;
    }

    // Output jack
    dl->AddCircleFilled(ImVec2(pedal_x, line_y), 6, Theme::CHAIN_JACK);
    dl->AddCircle(ImVec2(pedal_x, line_y), 6, Theme::BORDER_DARK, 0, 1.5f);

    // Handle removal
    if (remove_idx >= 0) {
        visible_indices_.erase(remove_idx);
        history_.execute(std::make_unique<RemoveEffectCommand>(engine_, remove_idx));
        rebuild_widgets();
    }

    // Reserve space for horizontal scrolling
    ImGui::Dummy(ImVec2(total_width + 20, 340));
}

} // namespace GuitarAmp
