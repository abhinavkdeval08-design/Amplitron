#include "gui/pedal_widget.h"
#include "audio/audio_engine.h"
#include "gui/theme.h"
#include "gui/command.h"
#include "gui/command_history.h"
#include "audio/effects/tuner.h"
#include "audio/effects/amp_simulator.h"
#include <cstring>
#include <cmath>

namespace Amplitron {

/** @brief Construct PedalWidget and look up color scheme for the effect type. */
PedalWidget::PedalWidget(AudioEngine& engine, std::shared_ptr<Effect> effect, int index)
    : engine_(engine), effect_(std::move(effect)), index_(index) {
    assign_colors();
}

/** @brief Look up pedal_color_ and led_color_ from the theme's effect color table. */
void PedalWidget::assign_colors() {
    const auto* entry = get_effect_color(effect_->name());
    pedal_color_ = entry->pedal_color;
    led_color_ = entry->led_color;
}

/** @brief Render the full pedal widget (body, knobs, switch, LED). @return true if remove requested. */
bool PedalWidget::render() {
    bool should_remove = false;

    ImGui::PushID(index_);

    float pedal_width = Theme::PEDAL_WIDTH;
    float pedal_height = Theme::PEDAL_HEIGHT;

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    bool is_amp = (std::strcmp(effect_->name(), "Amp Sim") == 0);
    bool enabled = effect_->is_enabled();

    // Pedal body
    ImVec2 p0 = cursor;
    ImVec2 p1 = ImVec2(cursor.x + pedal_width, cursor.y + pedal_height);

    // Shadow
    dl->AddRectFilled(
        ImVec2(p0.x + 4, p0.y + 4),
        ImVec2(p1.x + 4, p1.y + 4),
        Theme::PEDAL_SHADOW, Theme::ROUNDING_MD
    );

    if (is_amp) {
        // ========== AMP CABINET VISUAL ==========
        // Dark cabinet body
        ImU32 cab_body = IM_COL32(30, 22, 16, 255);     // dark walnut
        ImU32 cab_border = IM_COL32(90, 70, 40, 255);   // gold-brown trim
        ImU32 cab_grille = IM_COL32(18, 14, 10, 255);   // grille cloth
        ImU32 cab_grille_line = IM_COL32(38, 30, 22, 180);

        dl->AddRectFilled(p0, p1, cab_body, Theme::ROUNDING_MD);
        dl->AddRect(p0, p1, cab_border, Theme::ROUNDING_MD, 0, 2.5f);

        // Gold accent strip at top
        dl->AddRectFilled(
            ImVec2(p0.x + 6, p0.y + 6),
            ImVec2(p1.x - 6, p0.y + 10),
            Theme::ACCENT_GOLD_DIM, 2.0f);

        // Header plate (brushed metal look)
        ImVec2 plate_p0 = ImVec2(p0.x + 8, p0.y + 14);
        ImVec2 plate_p1 = ImVec2(p1.x - 8, p0.y + 50);
        dl->AddRectFilled(plate_p0, plate_p1,
            IM_COL32(46, 38, 28, 220), Theme::ROUNDING_SM);
        dl->AddRect(plate_p0, plate_p1,
            IM_COL32(70, 58, 38, 180), Theme::ROUNDING_SM, 0, 1.0f);

        // "AMP" label
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 12, p0.y + 18));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Gold());
        ImGui::Text("AMP");
        ImGui::PopStyleColor();

        // Model name
        int model_idx = static_cast<int>(effect_->params()[0].value);
        const auto& models = get_amp_models();
        const char* model_name = "Unknown";
        if (model_idx >= 0 && model_idx < static_cast<int>(models.size())) {
            model_name = models[model_idx].name;
        }
        ImVec2 mn_size = ImGui::CalcTextSize(model_name);
        float mn_x = p0.x + (pedal_width - mn_size.x) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(mn_x, p0.y + 33));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextPrimary());
        ImGui::Text("%s", model_name);
        ImGui::PopStyleColor();

        // LED indicator (power light)
        float led_x = p1.x - 22;
        float led_y = p0.y + 26;
        dl->AddCircleFilled(ImVec2(led_x, led_y), 5, Theme::LED_GREEN);
        dl->AddCircleFilled(ImVec2(led_x, led_y), 8, Theme::LED_GREEN_GLOW & 0x30FFFFFF);

        // Speaker grille area (bottom portion of the amp)
        float grille_top = p1.y - 100;
        float grille_bottom = p1.y - 12;
        float grille_left = p0.x + 12;
        float grille_right = p1.x - 12;

        // Grille background
        dl->AddRectFilled(
            ImVec2(grille_left, grille_top),
            ImVec2(grille_right, grille_bottom),
            cab_grille, Theme::ROUNDING_SM);
        dl->AddRect(
            ImVec2(grille_left, grille_top),
            ImVec2(grille_right, grille_bottom),
            IM_COL32(50, 40, 28, 180), Theme::ROUNDING_SM, 0, 1.0f);

        // Grille cloth horizontal lines
        for (float gy = grille_top + 6; gy < grille_bottom - 4; gy += 5.0f) {
            dl->AddLine(
                ImVec2(grille_left + 4, gy),
                ImVec2(grille_right - 4, gy),
                cab_grille_line, 1.0f);
        }

        // Gold accent strip at bottom
        dl->AddRectFilled(
            ImVec2(p0.x + 6, p1.y - 10),
            ImVec2(p1.x - 6, p1.y - 6),
            Theme::ACCENT_GOLD_DIM, 2.0f);
    } else {
        // ========== STANDARD PEDAL VISUAL ==========
        ImU32 body_color = ImGui::ColorConvertFloat4ToU32(pedal_color_);
        dl->AddRectFilled(p0, p1, body_color, Theme::ROUNDING_MD);
        dl->AddRect(p0, p1, Theme::PEDAL_BORDER, Theme::ROUNDING_MD, 0, 2.0f);

        // Metallic top plate
        ImVec2 plate_p0 = ImVec2(p0.x + 8, p0.y + 8);
        ImVec2 plate_p1 = ImVec2(p1.x - 8, p0.y + 45);
        dl->AddRectFilled(plate_p0, plate_p1,
            Theme::PEDAL_PLATE, Theme::ROUNDING_SM);

        // Effect name
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 12, p0.y + 14));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextPrimary());
        ImGui::Text("%s", effect_->name());
        ImGui::PopStyleColor();

        // LED indicator
        float led_x = p0.x + pedal_width - 25;
        float led_y = p0.y + 20;
        ImU32 led_col = enabled ?
            ImGui::ColorConvertFloat4ToU32(led_color_) :
            Theme::LED_OFF;
        dl->AddCircleFilled(ImVec2(led_x, led_y), 6, led_col);
        if (enabled) {
            dl->AddCircleFilled(ImVec2(led_x, led_y), 10,
                IM_COL32(
                    static_cast<int>(led_color_.x * 255),
                    static_cast<int>(led_color_.y * 255),
                    static_cast<int>(led_color_.z * 255),
                    40
                ));
        }
    }

    // Dim the pedal body when bypassed so the inactive state is immediately obvious
    if (!enabled && !is_amp) {
        dl->AddRectFilled(p0, p1, Theme::PEDAL_BYPASS_OVERLAY, Theme::ROUNDING_MD);
    }

    // --- Tuner custom display ---
    bool is_tuner = !is_amp && (std::strcmp(effect_->name(), "Tuner") == 0);
    if (is_tuner) {
        auto* tuner = dynamic_cast<TunerPedal*>(effect_.get());
        if (tuner) {
            float cx = p0.x + pedal_width * 0.5f;

            // Note name (large)
            bool has_signal = tuner->signal_detected.load(std::memory_order_relaxed);
            int note_idx = tuner->detected_note.load(std::memory_order_relaxed);
            int octave = tuner->detected_octave.load(std::memory_order_relaxed);
            float cents = tuner->detected_cents.load(std::memory_order_relaxed);
            float freq = tuner->detected_freq.load(std::memory_order_relaxed);

            float display_y = p0.y + 55;

            if (has_signal && note_idx >= 0) {
                // Note name + octave
                char note_buf[16];
                snprintf(note_buf, sizeof(note_buf), "%s%d",
                         TunerPedal::note_name(note_idx), octave);
                ImVec2 note_size = ImGui::CalcTextSize(note_buf);
                // Scale: draw it large by using the draw list directly
                float note_x = cx - note_size.x * 1.5f;
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 2.0f,
                    ImVec2(note_x, display_y),
                    Theme::TEXT_PRIMARY, note_buf);

                display_y += 45;

                // Cents text
                char cents_buf[32];
                snprintf(cents_buf, sizeof(cents_buf), "%+.1f cents", cents);
                ImVec2 cents_text_size = ImGui::CalcTextSize(cents_buf);
                ImGui::SetCursorScreenPos(ImVec2(cx - cents_text_size.x * 0.5f, display_y));
                float abs_cents = std::fabs(cents);
                ImVec4 cents_col = (abs_cents < 2.0f)
                    ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f)   // green = in tune
                    : (abs_cents < 15.0f)
                        ? ImVec4(0.9f, 0.8f, 0.2f, 1.0f) // yellow
                        : ImVec4(0.9f, 0.2f, 0.2f, 1.0f); // red = out of tune
                ImGui::PushStyleColor(ImGuiCol_Text, cents_col);
                ImGui::TextUnformatted(cents_buf);
                ImGui::PopStyleColor();

                display_y += 22;

                // Cents deviation bar
                float bar_w = pedal_width - 30;
                float bar_h = 10;
                float bar_x = p0.x + 15;
                float bar_y = display_y;
                // Background
                dl->AddRectFilled(
                    ImVec2(bar_x, bar_y),
                    ImVec2(bar_x + bar_w, bar_y + bar_h),
                    Theme::KNOB_BG, 3.0f);
                // Center line
                float center_x = bar_x + bar_w * 0.5f;
                dl->AddLine(
                    ImVec2(center_x, bar_y - 1),
                    ImVec2(center_x, bar_y + bar_h + 1),
                    Theme::TEXT_DIM, 1.5f);
                // Needle: cents range [-50, +50] mapped to bar
                float needle_norm = clamp(cents / 50.0f, -1.0f, 1.0f);
                float needle_x = center_x + needle_norm * (bar_w * 0.5f);
                ImU32 needle_col = ImGui::ColorConvertFloat4ToU32(cents_col);
                dl->AddRectFilled(
                    ImVec2(needle_x - 3, bar_y - 2),
                    ImVec2(needle_x + 3, bar_y + bar_h + 2),
                    needle_col, 2.0f);

                display_y += bar_h + 14;

                // Frequency
                char freq_buf[32];
                snprintf(freq_buf, sizeof(freq_buf), "%.1f Hz", freq);
                ImVec2 freq_size = ImGui::CalcTextSize(freq_buf);
                ImGui::SetCursorScreenPos(ImVec2(cx - freq_size.x * 0.5f, display_y));
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextSecondary());
                ImGui::TextUnformatted(freq_buf);
                ImGui::PopStyleColor();

                display_y += 22;
            } else {
                // No signal
                const char* no_sig = "---";
                ImVec2 ns_size = ImGui::CalcTextSize(no_sig);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 2.0f,
                    ImVec2(cx - ns_size.x * 1.5f, display_y),
                    Theme::TEXT_DIM, no_sig);
                display_y += 45;

                const char* waiting = "Play a note...";
                ImVec2 wt_size = ImGui::CalcTextSize(waiting);
                ImGui::SetCursorScreenPos(ImVec2(cx - wt_size.x * 0.5f, display_y));
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim());
                ImGui::TextUnformatted(waiting);
                ImGui::PopStyleColor();

                display_y += 22;
            }

            // Mute indicator
            display_y += 8;
            bool mute_on = effect_->params()[0].value >= 0.5f;
            const char* mute_label = mute_on ? "[MUTE ON]" : "[MUTE OFF]";
            ImVec2 ml_size = ImGui::CalcTextSize(mute_label);
            ImGui::SetCursorScreenPos(ImVec2(cx - ml_size.x * 0.5f, display_y));
            ImGui::PushStyleColor(ImGuiCol_Text,
                mute_on ? ImVec4(0.9f, 0.3f, 0.3f, 1.0f) : ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
            ImGui::TextUnformatted(mute_label);
            ImGui::PopStyleColor();

            // Clickable area to toggle mute
            ImGui::SetCursorScreenPos(ImVec2(cx - ml_size.x * 0.5f, display_y));
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##tuner_mute_toggle", ml_size);
            if (ImGui::IsItemClicked()) {
                float new_val = mute_on ? 0.0f : 1.0f;
                effect_->params()[0].value = new_val;
                engine_.push_param_change(index_, 0, new_val);
            }
            if (ImGui::IsItemHovered()) {
                if (!effect_->params()[0].tooltip.empty()) {
                    ImGui::SetTooltip("Click to toggle mute\n\n%s", effect_->params()[0].tooltip.c_str());
                } else {
                    ImGui::SetTooltip("Click to toggle mute");
                }
            }
        }
    }

    // Knobs area (skip for tuner — it has a custom display above)
    // For amp, knobs start after the header; skip model selector param (index 0)
    float knob_y_start = p0.y + Theme::KNOB_Y_START;
    auto& params = effect_->params();
    int num_params = is_tuner ? 0 : static_cast<int>(params.size());
    int param_offset = 0;
    if (is_amp) {
        param_offset = 1; // skip model selector param
        num_params = std::max(0, num_params - 1);
    }

    float knob_radius    = Theme::KNOB_RADIUS;
    float knob_spacing_x = Theme::KNOB_SPACING_X;
    float knob_spacing_y = Theme::KNOB_SPACING_Y;
    float knob_hit_size  = knob_radius * Theme::KNOB_HIT_MULT;

    // Knob arc constants: 270° sweep from bottom-left to bottom-right
    constexpr float PI = 3.14159265f;
    constexpr float TWO_PI = 6.28318530f;
    constexpr float ARC_START = 2.356f;   // 135° (7:30 position)
    constexpr float ARC_RANGE = 4.712f;   // 270° sweep clockwise

    // Left edge of the 2-column grid, centered horizontally in the pedal
    float knob_grid_left = p0.x + (pedal_width - 2.0f * knob_spacing_x) * 0.5f;

    for (int i = 0; i < num_params && i < 6; ++i) {
        int pi = i + param_offset; // actual param index
        int col = i % 2;
        int row = i / 2;

        // When the last knob is alone in its row, center it instead of
        // leaving it left-aligned (e.g. a 3-knob pedal like Reverb).
        bool is_last_alone = (i == num_params - 1) && (num_params % 2 == 1);
        float kx = is_last_alone
            ? p0.x + (pedal_width - knob_spacing_x) * 0.5f
            : knob_grid_left + col * knob_spacing_x;
        float ky = knob_y_start + row * knob_spacing_y;

        ImVec2 knob_center = ImVec2(kx + knob_spacing_x * 0.5f, ky + knob_radius + 2);

        char label[64];
        snprintf(label, sizeof(label), "##knob_%s_%d_%d", effect_->name(), index_, pi);

        // Invisible interaction area centered on knob — allow overlap so knobs
        // near pedal edges don't block adjacent pedals' controls.
        ImGui::SetCursorScreenPos(ImVec2(
            knob_center.x - knob_hit_size * 0.5f,
            knob_center.y - knob_hit_size * 0.5f));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(label, ImVec2(knob_hit_size, knob_hit_size));

        bool is_hovered = ImGui::IsItemHovered();
        bool is_active = ImGui::IsItemActive();

        // --- Interaction ---
        float range = params[pi].max_val - params[pi].min_val;

        // Track drag start for undo coalescing
        if (is_active && !knob_was_active_) {
            active_param_index_ = pi;
            param_value_before_drag_ = params[pi].value;
        }

        if (is_active) {
            float mdx = ImGui::GetIO().MouseDelta.x;
            float mdy = ImGui::GetIO().MouseDelta.y;

            if (mdx != 0.0f || mdy != 0.0f) {
                ImVec2 mouse = ImGui::GetIO().MousePos;
                float dx = mouse.x - knob_center.x;
                float dy = mouse.y - knob_center.y;
                float dist = std::sqrt(dx * dx + dy * dy);

                float value_delta = 0.0f;

                if (dist > 5.0f && dist < knob_radius * 5.0f) {
                    // ANGULAR MODE: mouse is near the knob — track rotation
                    // Compute angular delta between previous and current mouse
                    float prev_x = mouse.x - mdx;
                    float prev_y = mouse.y - mdy;
                    float curr_angle = std::atan2(
                        mouse.y - knob_center.y, mouse.x - knob_center.x);
                    float prev_angle = std::atan2(
                        prev_y - knob_center.y, prev_x - knob_center.x);

                    float angle_delta = curr_angle - prev_angle;
                    // Unwrap: keep delta in [-PI, PI]
                    if (angle_delta > PI)  angle_delta -= TWO_PI;
                    if (angle_delta < -PI) angle_delta += TWO_PI;

                    // Clockwise on screen (positive atan2 delta) = increase value
                    value_delta = (angle_delta / ARC_RANGE) * range;
                } else {
                    // LINEAR MODE: mouse far from knob — use vertical drag
                    float sensitivity = 0.007f;
                    value_delta = -mdy * sensitivity * range;
                }

                // Modifiers
                if (ImGui::GetIO().KeyShift) value_delta *= 0.2f;
                if (ImGui::GetIO().KeyCtrl)  value_delta *= 3.0f;

                float new_val = clamp(params[pi].value + value_delta, params[pi].min_val, params[pi].max_val);
                if (new_val != params[pi].value) {
                    params[pi].value = new_val;
                    engine_.push_param_change(index_, pi, new_val);
                }
            }
        }

        // Commit param change when drag ends
        if (knob_was_active_ && !is_active && active_param_index_ == pi) {
            float new_val = params[pi].value;
            if (new_val != param_value_before_drag_) {
                commit_param_change(pi, param_value_before_drag_, new_val);
            }
            active_param_index_ = -1;
            knob_was_active_ = false;
        }

        // Update active tracking for this knob
        if (active_param_index_ == pi) {
            knob_was_active_ = is_active;
        }

        // Scroll wheel
        if (is_hovered && std::fabs(ImGui::GetIO().MouseWheel) > 0.0f) {
            float old_val = params[pi].value;
            float step = range * 0.03f;
            if (ImGui::GetIO().KeyShift) step *= 0.2f;
            float new_val = clamp(params[pi].value + ImGui::GetIO().MouseWheel * step,
                                    params[pi].min_val, params[pi].max_val);
            if (new_val != old_val) {
                params[pi].value = new_val;
                engine_.push_param_change(index_, pi, new_val);
                commit_param_change(pi, old_val, new_val);
            }
        }

        // Double-click to reset
        if (is_hovered && ImGui::IsMouseDoubleClicked(0)) {
            float old_val = params[pi].value;
            float new_val = params[pi].default_val;
            if (new_val != old_val) {
                params[pi].value = new_val;
                engine_.push_param_change(index_, pi, new_val);
                commit_param_change(pi, old_val, new_val);
            }
        }

        // Right-click for direct input
        if (is_hovered && ImGui::IsMouseClicked(1)) {
            ImGui::OpenPopup(label);
        }
        if (ImGui::BeginPopup(label)) {
            ImGui::Text("%s", params[pi].name.c_str());
            ImGui::SetNextItemWidth(120);
            float slider_val = params[pi].value;
            ImGui::SliderFloat("##edit", &slider_val,
                               params[pi].min_val, params[pi].max_val, "%.2f");
            if (slider_val != params[pi].value) {
                params[pi].value = slider_val;
                engine_.push_param_change(index_, pi, slider_val);
            }
            if (ImGui::IsItemActivated()) {
                popup_active_param_index_ = pi;
                popup_param_value_before_edit_ = params[pi].value;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && popup_active_param_index_ == pi) {
                if (params[pi].value != popup_param_value_before_edit_) {
                    engine_.push_param_change(index_, pi, params[pi].value);
                    commit_param_change(pi, popup_param_value_before_edit_, params[pi].value);
                }
                popup_active_param_index_ = -1;
            }
            if (ImGui::Button("Reset")) {
                float old_val = params[pi].value;
                float new_val = params[pi].default_val;
                if (new_val != old_val) {
                    params[pi].value = new_val;
                    engine_.push_param_change(index_, pi, new_val);
                    commit_param_change(pi, old_val, new_val);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // --- Drawing ---
        float normalized = (params[pi].value - params[pi].min_val) / range;

        // Outer ring / track (colored arc)
        float track_radius = knob_radius + 3;
        int segments = 40;
        for (int s = 0; s < segments; ++s) {
            float t0 = static_cast<float>(s) / segments;
            float t1 = static_cast<float>(s + 1) / segments;
            float a0 = ARC_START + t0 * ARC_RANGE;
            float a1 = ARC_START + t1 * ARC_RANGE;

            bool filled = t0 <= normalized;
            ImU32 seg_color = filled ?
                ImGui::ColorConvertFloat4ToU32(led_color_) :
                Theme::KNOB_TRACK_OFF;

            dl->AddLine(
                ImVec2(knob_center.x + std::cos(a0) * track_radius,
                       knob_center.y + std::sin(a0) * track_radius),
                ImVec2(knob_center.x + std::cos(a1) * track_radius,
                       knob_center.y + std::sin(a1) * track_radius),
                seg_color, 3.0f);
        }

        // Knob body (highlights on hover/active)
        ImU32 knob_bg = is_active ? Theme::KNOB_ACTIVE :
                        is_hovered ? Theme::KNOB_HOVER :
                                     Theme::KNOB_FACE;
        dl->AddCircleFilled(knob_center, knob_radius, Theme::KNOB_BG);
        dl->AddCircleFilled(knob_center, knob_radius - 1, knob_bg);

        // Pointer line (rotates with value)
        float pointer_angle = ARC_START + normalized * ARC_RANGE;
        float ptr_inner = knob_radius * 0.25f;
        float ptr_outer = knob_radius - 3.0f;
        ImVec2 ptr_from = ImVec2(
            knob_center.x + std::cos(pointer_angle) * ptr_inner,
            knob_center.y + std::sin(pointer_angle) * ptr_inner);
        ImVec2 ptr_to = ImVec2(
            knob_center.x + std::cos(pointer_angle) * ptr_outer,
            knob_center.y + std::sin(pointer_angle) * ptr_outer);

        ImU32 ptr_color = is_active ?
            Theme::ACCENT_GOLD_HOT :
            Theme::ACCENT_GOLD;
        dl->AddLine(ptr_from, ptr_to, ptr_color, 2.5f);
        dl->AddCircleFilled(ptr_to, 3.0f, ptr_color);

        // Tooltip
        if (is_hovered || is_active) {
            std::string val_str  = Theme::formatParameterValue(params[pi].value, params[pi].unit);
            std::string min_str  = Theme::formatParameterValue(params[pi].min_val, params[pi].unit);
            std::string max_str  = Theme::formatParameterValue(params[pi].max_val, params[pi].unit);
            if (params[pi].tooltip.empty()) {
                ImGui::SetTooltip("%s: %s\nRange: [%s, %s]\n\nRotate or drag to adjust\nScroll wheel also works\nShift=fine  Ctrl=coarse\nDbl-click=reset  Right-click=edit",
                    params[pi].name.c_str(), val_str.c_str(), min_str.c_str(), max_str.c_str());
            } else {
                ImGui::SetTooltip("%s: %s\nRange: [%s, %s]\n\n%s\n\nRotate or drag to adjust\nScroll wheel also works\nShift=fine  Ctrl=coarse\nDbl-click=reset  Right-click=edit",
                    params[pi].name.c_str(), val_str.c_str(), min_str.c_str(), max_str.c_str(),
                    params[pi].tooltip.c_str());
            }
        }

        // Parameter name below knob (centered)
        const char* pname = params[pi].name.c_str();
        ImVec2 text_size = ImGui::CalcTextSize(pname);
        ImGui::SetCursorScreenPos(ImVec2(
            knob_center.x - text_size.x * 0.5f,
            knob_center.y + knob_radius + 8));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextSecondary());
        ImGui::TextUnformatted(pname);
        ImGui::PopStyleColor();

        // Value text above knob (centered), using unit-aware formatter
        std::string val_display = Theme::formatParameterValue(params[pi].value, params[pi].unit);
        ImVec2 val_size = ImGui::CalcTextSize(val_display.c_str());
        ImGui::SetCursorScreenPos(ImVec2(
            knob_center.x - val_size.x * 0.5f,
            knob_center.y - knob_radius - 20));
        ImGui::PushStyleColor(ImGuiCol_Text,
            is_active ? Theme::GoldHot() :
                        Theme::TextDim());
        ImGui::TextUnformatted(val_display.c_str());
        ImGui::PopStyleColor();
    }

    // knob_was_active_ is updated per-knob inside the loop above

    // LED tooltip — hover area over the LED indicator
    if (!is_amp) {
        float led_x = p0.x + pedal_width - 25;
        float led_y = p0.y + 20;
        ImGui::SetCursorScreenPos(ImVec2(led_x - 10, led_y - 10));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##led_tip", ImVec2(20, 20));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(enabled ? "Effect active" : "Effect bypassed");
        }
    }

    // Footswitch (toggle on/off) — amps are always on, no footswitch
    if (!is_amp) {
        float switch_y = p0.y + pedal_height - Theme::SWITCH_BOTTOM_OFFSET;
        float switch_x = p0.x + (pedal_width - 50) / 2;
        ImGui::SetCursorScreenPos(ImVec2(switch_x, switch_y));

        // Draw footswitch
        ImVec2 sw_center = ImVec2(switch_x + 25, switch_y + 15);
        dl->AddCircleFilled(sw_center, 18, Theme::SWITCH_BODY);
        dl->AddCircle(sw_center, 18, Theme::SWITCH_RING, 0, 2.0f);
        dl->AddCircleFilled(sw_center, 12,
            enabled ? Theme::SWITCH_ACTIVE : Theme::SWITCH_IDLE);

        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##switch", ImVec2(50, 30));
        if (ImGui::IsItemClicked()) {
            bool new_enabled = !enabled;
            effect_->set_enabled(new_enabled);
            engine_.push_effect_enabled(index_, new_enabled ? 1.0f : 0.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(enabled ? "Click to bypass" : "Click to enable");
        }
    }

    // Remove button (small X at top-right) — not shown for amp
    if (!is_amp) {
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 22, p0.y + 2));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.1f, 0.1f, 0.8f));
        char remove_label[32];
        snprintf(remove_label, sizeof(remove_label), "X##rm%d", index_);
        if (ImGui::SmallButton(remove_label)) {
            should_remove = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove %s from chain", effect_->name());
        }
        ImGui::PopStyleColor(2);
    }

    // Advance cursor for next pedal
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pedal_width + 15, cursor.y));

    ImGui::PopID();
    return should_remove;
}

/** @brief Push a ParameterChangeCommand to the history without executing (value already applied). */
void PedalWidget::commit_param_change(int param_index, float old_val, float new_val) {
    if (!history_) return;
    auto cmd = std::make_unique<ParameterChangeCommand>(
        engine_, effect_, param_index, old_val, new_val);
    history_->push_executed(std::move(cmd));
}

} // namespace Amplitron
