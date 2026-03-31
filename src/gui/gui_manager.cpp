#include "gui/gui_manager.h"
#include "gui/pedal_board.h"
#include "gui/theme.h"
#include "gui/file_dialog.h"
#include "gui/command.h"

#include "gui/gl_setup.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

namespace GuitarAmp {

GuiManager::GuiManager(AudioEngine& engine)
    : engine_(engine),
      tuner_instance_(std::make_shared<TunerPedal>()),
      spectrum_analyzer_(std::make_unique<SpectrumAnalyzer>()) {}

std::string GuiManager::preset_name_from_path(const std::string& filepath) const {
    size_t slash = filepath.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? filepath : filepath.substr(slash + 1);
    if (name.size() > 5 && name.substr(name.size() - 5) == ".json") {
        name = name.substr(0, name.size() - 5);
    }
    return name;
}

std::string GuiManager::preset_path_from_name(const std::string& preset_name) const {
    std::string filename = preset_name;
    for (char& c : filename) {
        if (c == ' ') c = '_';
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    if (filename.empty()) return "";
    return PresetManager::get_presets_dir() + "/" + filename + ".json";
}

void GuiManager::refresh_presets(bool preserve_selection) {
    std::string selected_path;
    if (preserve_selection && selected_preset_index_ >= 0 &&
        selected_preset_index_ < static_cast<int>(preset_files_.size())) {
        selected_path = preset_files_[selected_preset_index_];
    }

    preset_files_ = PresetManager::list_presets();
    std::sort(preset_files_.begin(), preset_files_.end());

    selected_preset_index_ = -1;
    if (!selected_path.empty()) {
        for (int i = 0; i < static_cast<int>(preset_files_.size()); ++i) {
            if (preset_files_[i] == selected_path) {
                selected_preset_index_ = i;
                break;
            }
        }
    }
    if (selected_preset_index_ < 0 && !preset_files_.empty()) {
        selected_preset_index_ = 0;
    }

    if (selected_preset_index_ >= 0 && selected_preset_index_ < static_cast<int>(preset_files_.size())) {
        std::snprintf(preset_name_buf_, sizeof(preset_name_buf_), "%s",
                      preset_name_from_path(preset_files_[selected_preset_index_]).c_str());
    }
}

bool GuiManager::save_named_preset(const std::string& preset_name,
                                   const std::string& description) {
    if (preset_name.empty()) {
        preset_status_msg_ = "Error: Preset name cannot be empty.";
        return false;
    }

    std::string path = preset_path_from_name(preset_name);
    if (path.empty()) {
        preset_status_msg_ = "Error: Invalid preset name.";
        return false;
    }

    if (PresetManager::save_preset(path, preset_name, description, engine_)) {
        preset_status_msg_ = "Saved: " + preset_name;
        refresh_presets(true);
        for (int i = 0; i < static_cast<int>(preset_files_.size()); ++i) {
            if (preset_files_[i] == path) {
                selected_preset_index_ = i;
                break;
            }
        }
        if (pedal_board_) pedal_board_->rebuild_widgets();
        return true;
    }

    preset_status_msg_ = "Error: " + PresetManager::last_error();
    return false;
}

bool GuiManager::load_preset_by_index(int index) {
    if (index < 0 || index >= static_cast<int>(preset_files_.size())) {
        preset_status_msg_ = "Error: No preset selected.";
        return false;
    }

    const std::string& path = preset_files_[index];
    std::vector<LoadPresetCommand::EffectSnapshot> before_state;
    for (auto& fx : engine_.effects()) {
        LoadPresetCommand::EffectSnapshot snap;
        snap.effect = fx;
        snap.enabled = fx->is_enabled();
        snap.mix = fx->get_mix();
        for (auto& p : fx->params()) snap.param_values.push_back(p.value);
        before_state.push_back(std::move(snap));
    }
    float before_in = engine_.get_input_gain();
    float before_out = engine_.get_output_gain();

    if (PresetManager::load_preset(path, engine_)) {
        std::vector<LoadPresetCommand::EffectSnapshot> after_state;
        for (auto& fx : engine_.effects()) {
            LoadPresetCommand::EffectSnapshot snap;
            snap.effect = fx;
            snap.enabled = fx->is_enabled();
            snap.mix = fx->get_mix();
            for (auto& p : fx->params()) snap.param_values.push_back(p.value);
            after_state.push_back(std::move(snap));
        }
        float after_in = engine_.get_input_gain();
        float after_out = engine_.get_output_gain();

        command_history_.clear();
        auto cmd = std::make_unique<LoadPresetCommand>(
            engine_, std::move(before_state), before_in, before_out,
            std::move(after_state), after_in, after_out);
        command_history_.push_executed(std::move(cmd));

        selected_preset_index_ = index;
        std::string display = preset_name_from_path(path);
        std::snprintf(preset_name_buf_, sizeof(preset_name_buf_), "%s", display.c_str());
        preset_status_msg_ = "Loaded: " + display;
        if (pedal_board_) pedal_board_->rebuild_widgets(true);
        return true;
    }

    preset_status_msg_ = "Error: " + PresetManager::last_error();
    return false;
}

bool GuiManager::delete_preset_by_index(int index) {
    if (index < 0 || index >= static_cast<int>(preset_files_.size())) {
        preset_status_msg_ = "Error: No preset selected.";
        return false;
    }

    std::string path = preset_files_[index];
    std::string display = preset_name_from_path(path);
    if (std::remove(path.c_str()) == 0) {
        preset_status_msg_ = "Deleted: " + display;
        refresh_presets(false);
        return true;
    }

    preset_status_msg_ = "Error: Could not delete preset file.";
    return false;
}

void GuiManager::ensure_factory_presets() {
    if (factory_presets_initialized_) return;
    factory_presets_initialized_ = true;

    if (!PresetManager::list_presets().empty()) return;

    std::vector<PresetData> factory_presets;

    PresetData clean;
    clean.name = "Clean";
    clean.description = "Low gain, slight reverb, flat EQ";
    clean.input_gain = 0.6f;
    clean.output_gain = 0.85f;
    clean.effects.push_back({"Compressor", true, 0.25f, {}});
    clean.effects.push_back({"Equalizer", true, 1.0f, {}});
    clean.effects.push_back({"Reverb", true, 0.2f, {}});
    clean.effects.push_back({"Cabinet", true, 1.0f, {}});
    factory_presets.push_back(clean);

    PresetData crunch;
    crunch.name = "Crunch";
    crunch.description = "Mild overdrive with mid-forward response";
    crunch.input_gain = 0.85f;
    crunch.output_gain = 0.9f;
    crunch.effects.push_back({"Noise Gate", true, 0.35f, {}});
    crunch.effects.push_back({"Overdrive", true, 0.55f, {}});
    crunch.effects.push_back({"Equalizer", true, 1.0f, {}});
    crunch.effects.push_back({"Cabinet", true, 1.0f, {}});
    factory_presets.push_back(crunch);

    PresetData metal;
    metal.name = "Metal";
    metal.description = "High distortion with scooped mids and tight cabinet";
    metal.input_gain = 1.15f;
    metal.output_gain = 0.75f;
    metal.effects.push_back({"Noise Gate", true, 0.85f, {}});
    metal.effects.push_back({"Distortion", true, 0.9f, {}});
    metal.effects.push_back({"Equalizer", true, 1.0f, {}});
    metal.effects.push_back({"Cabinet", true, 1.0f, {}});
    factory_presets.push_back(metal);

    PresetData jazz;
    jazz.name = "Jazz";
    jazz.description = "Clean, warm tone with light compression";
    jazz.input_gain = 0.55f;
    jazz.output_gain = 0.9f;
    jazz.effects.push_back({"Compressor", true, 0.4f, {}});
    jazz.effects.push_back({"Equalizer", true, 1.0f, {}});
    jazz.effects.push_back({"Reverb", true, 0.12f, {}});
    jazz.effects.push_back({"Cabinet", true, 1.0f, {}});
    factory_presets.push_back(jazz);

    for (const auto& preset : factory_presets) {
        PresetManager::save_preset_data(preset_path_from_name(preset.name), preset);
    }
}

GuiManager::~GuiManager() {
    shutdown();
}

bool GuiManager::initialize(int width, int height) {
    window_width_ = width;
    window_height_ = height;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, GLSetup::GL_CONTEXT_PROFILE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GLSetup::GL_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GLSetup::GL_MINOR);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window_ = SDL_CreateWindow(
        Theme::WINDOW_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        window_width_, window_height_,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window_) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        return false;
    }

    gl_context_ = SDL_GL_CreateContext(window_);
    SDL_GL_MakeCurrent(window_, gl_context_);
    SDL_GL_SetSwapInterval(1); // vsync

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Amplitron design system
    ImGui::StyleColorsDark();
    Theme::ApplyStyle();

    // Load window icon from assets/icon.svg (resolve path relative to executable)
    {
        std::string icon_path;
        char* base = SDL_GetBasePath();
        if (base) {
            icon_path = std::string(base) + "assets/icon.svg";
            SDL_free(base);
        }
        // Fallback paths for development and installed layouts
        NSVGimage* svg = nullptr;
        if (!icon_path.empty())
            svg = nsvgParseFromFile(icon_path.c_str(), "px", 96.0f);
        if (!svg)
            svg = nsvgParseFromFile("../assets/icon.svg", "px", 96.0f);
        if (!svg)
            svg = nsvgParseFromFile("assets/icon.svg", "px", 96.0f);
        if (svg) {
            const int icon_size = 64;  // 64x64 icon
            NSVGrasterizer* rast = nsvgCreateRasterizer();
            if (rast) {
                unsigned char* img = new unsigned char[icon_size * icon_size * 4];
                nsvgRasterize(rast, svg, 0, 0,
                             icon_size / svg->width,
                             img, icon_size, icon_size,
                             icon_size * 4);

                SDL_Surface* icon = SDL_CreateRGBSurfaceFrom(
                    img, icon_size, icon_size, 32, icon_size * 4,
                    0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
                if (icon) {
                    SDL_SetWindowIcon(window_, icon);
                    SDL_FreeSurface(icon);
                }
                delete[] img;
                nsvgDeleteRasterizer(rast);
            }
            nsvgDelete(svg);
        } else {
            std::cerr << "Warning: Could not load assets/icon.svg" << std::endl;
        }
    }

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_context_);
    ImGui_ImplOpenGL3_Init(GLSetup::GLSL_VERSION);

    pedal_board_ = std::make_unique<PedalBoard>(engine_, command_history_);

#ifndef EMSCRIPTEN
    update_check_thread_ = std::thread([this]() { this->check_for_updates(); });
#endif

    initialized_ = true;
    return true;
}

void GuiManager::shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    if (update_check_thread_.joinable()) {
        update_check_thread_.join();
    }

    engine_.clear_tuner_tap();
    pedal_board_.reset();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (gl_context_) {
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

bool GuiManager::run_frame() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) return false;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE &&
            event.window.windowID == SDL_GetWindowID(window_))
            return false;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Keyboard shortcuts for undo/redo (Cmd+Z / Ctrl+Z, Cmd+Shift+Z / Ctrl+Y)
    // Skip when a text input is active so Ctrl+Z works normally in text fields.
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            bool mod = io.KeySuper || io.KeyCtrl;  // Cmd on macOS, Ctrl on Win/Linux
            if (mod && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) {
                if (command_history_.undo() && pedal_board_) {
                    pedal_board_->rebuild_widgets();
                }
            }
            if (mod && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) {
                if (command_history_.redo() && pedal_board_) {
                    pedal_board_->rebuild_widgets();
                }
            }
            if (mod && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y)) {
                if (command_history_.redo() && pedal_board_) {
                    pedal_board_->rebuild_widgets();
                }
            }
        }
    }

    // Main menu bar
    render_menu_bar();

    // Full-window layout
    SDL_GetWindowSize(window_, &window_width_, &window_height_);

    ImGui::SetNextWindowPos(ImVec2(0, 20));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(window_width_),
                                     static_cast<float>(window_height_) - 20));
    ImGui::Begin("##MainArea", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    render_master_controls();

    ImGui::Separator();

    // Recording controls (above pedal board)
    render_recording_controls();

    ImGui::Separator();

    float analyzer_reserved_h = analyzer_panel_expanded_ ? 245.0f : 38.0f;
    ImGui::BeginChild("PedalBoardRegion", ImVec2(0, -analyzer_reserved_h), false);
    if (pedal_board_) {
        pedal_board_->render();
    }
    ImGui::EndChild();

    ImGui::Separator();
    render_analyzer_panel();

    ImGui::End();

    // Popups / floating windows
    if (show_settings_) {
        render_settings_window();
    }
    if (show_save_preset_) {
        render_save_preset_popup();
    }
    if (show_load_preset_) {
        render_load_preset_popup();
    }
    if (show_recording_save_) {
        render_recording_save_dialog();
    }
    if (show_tuner_) {
        render_tuner_modal();
    }

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    SDL_GL_GetDrawableSize(window_, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.078f, 0.071f, 0.063f, 1.0f);  // #141210 BG_DARKEST
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);

    return true;
}

void GuiManager::render_vu_bar(const char* id,
                               const char* label,
                               float rms_level,
                               float peak_hold,
                               bool clip_active,
                               float clip_flash,
                               ImU32 base_color,
                               ImU32 peak_color) {
    ImGui::PushID(id);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    float db_value = (rms_level > 0.0001f) ? (20.0f * std::log10(rms_level)) : -96.0f;
    ImGui::TextColored(ImVec4(0.80f, 0.80f, 0.80f, 1.0f), "%.1f dB", db_value);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = 18.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg_col = Theme::METER_BG;
    if (clip_active || clip_flash > 0.01f) {
        const float flash = clamp(clip_flash, 0.0f, 1.0f);
        int alpha = static_cast<int>(90.0f + flash * 130.0f);
        bg_col = IM_COL32(180, 30, 30, alpha);
    }

    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg_col, Theme::ROUNDING_SM);

    float rms_fill = clamp(rms_level, 0.0f, 1.0f) * width;
    dl->AddRectFilled(pos, ImVec2(pos.x + rms_fill, pos.y + height), base_color, Theme::ROUNDING_SM);

    float peak_x = pos.x + clamp(peak_hold, 0.0f, 1.0f) * width;
    dl->AddLine(ImVec2(peak_x, pos.y - 1.0f), ImVec2(peak_x, pos.y + height + 1.0f), peak_color, 2.0f);

    if (clip_active || clip_flash > 0.01f) {
        dl->AddText(ImVec2(pos.x + width - 32.0f, pos.y - 1.0f), IM_COL32(255, 90, 90, 255), "CLIP");
    }

    ImGui::Dummy(ImVec2(width, height + 6.0f));
    ImGui::PopID();
}

void GuiManager::render_analyzer_panel() {
    float panel_h = analyzer_panel_expanded_ ? 230.0f : 34.0f;
    ImGui::BeginChild("AnalyzerPanel", ImVec2(0, panel_h), true, ImGuiWindowFlags_NoScrollbar);

    const bool expanded = ImGui::CollapsingHeader("Real-Time Analyzer", ImGuiTreeNodeFlags_DefaultOpen);
    analyzer_panel_expanded_ = expanded;
    engine_.set_analyzer_enabled(expanded);
    if (!expanded) {
        ImGui::EndChild();
        return;
    }

    const float dt = std::max(ImGui::GetIO().DeltaTime, 1.0f / 240.0f);

    const float input_rms = engine_.get_input_rms();
    const float output_rms = engine_.get_output_rms();
    smoothed_input_rms_ += (input_rms - smoothed_input_rms_) * 0.22f;
    smoothed_output_rms_ += (output_rms - smoothed_output_rms_) * 0.22f;

    const float peak_decay = 0.45f;
    input_rms_peak_hold_ = std::max(smoothed_input_rms_, input_rms_peak_hold_ - peak_decay * dt);
    output_rms_peak_hold_ = std::max(smoothed_output_rms_, output_rms_peak_hold_ - peak_decay * dt);

    const bool input_clipped = engine_.consume_input_clipped();
    const bool output_clipped = engine_.consume_output_clipped();
    if (input_clipped) input_clip_flash_ = 1.0f;
    if (output_clipped) output_clip_flash_ = 1.0f;
    input_clip_flash_ = std::max(0.0f, input_clip_flash_ - dt * 2.0f);
    output_clip_flash_ = std::max(0.0f, output_clip_flash_ - dt * 2.0f);

    int mode_index = static_cast<int>(analyzer_mode_);
    ImGui::TextUnformatted("Spectrum:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::Combo("##AnalyzerMode", &mode_index, "Input\0Output\0Overlay\0")) {
        analyzer_mode_ = static_cast<SpectrumAnalyzer::DisplayMode>(mode_index);
    }

    ImGui::Columns(2, "analyzer_vu_cols", false);
    render_vu_bar("input_vu",
                  "INPUT RMS",
                  smoothed_input_rms_,
                  input_rms_peak_hold_,
                  input_clipped,
                  input_clip_flash_,
                  IM_COL32(60, 200, 110, 230),
                  IM_COL32(255, 230, 120, 255));
    ImGui::NextColumn();
    render_vu_bar("output_vu",
                  "OUTPUT RMS",
                  smoothed_output_rms_,
                  output_rms_peak_hold_,
                  output_clipped,
                  output_clip_flash_,
                  IM_COL32(80, 170, 245, 230),
                  IM_COL32(255, 230, 120, 255));
    ImGui::Columns(1);

    const uint64_t analyzer_seq = engine_.get_analyzer_sequence();
    if (analyzer_seq != analyzer_last_sequence_) {
        if (engine_.copy_analyzer_snapshot(analyzer_input_buf_.data(),
                                           analyzer_output_buf_.data(),
                                           AudioEngine::ANALYZER_FFT_SIZE)) {
            spectrum_analyzer_->update(analyzer_input_buf_.data(),
                                       analyzer_output_buf_.data(),
                                       engine_.get_sample_rate(),
                                       dt);
            analyzer_last_sequence_ = analyzer_seq;
        }
    }

    ImVec2 plot_pos = ImGui::GetCursorScreenPos();
    ImVec2 plot_size(ImGui::GetContentRegionAvail().x, 112.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(plot_pos,
                      ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y),
                      IM_COL32(20, 22, 28, 255),
                      Theme::ROUNDING_SM);

    if (spectrum_analyzer_) {
        spectrum_analyzer_->draw(dl, plot_pos, plot_size, analyzer_mode_);
    }

    ImGui::Dummy(plot_size);

    const float axis_left = ImGui::GetCursorPosX();
    const float axis_w = ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(Theme::TextSecondary(), "20 Hz");
    ImGui::SameLine(axis_left + axis_w * 0.48f);
    ImGui::TextColored(Theme::TextSecondary(), "1 kHz");
    ImGui::SameLine(axis_left + axis_w - 52.0f);
    ImGui::TextColored(Theme::TextSecondary(), "20 kHz");

    ImGui::EndChild();
}

void GuiManager::render_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Preset...")) {
                selected_preset_index_ = -1;
                preset_name_buf_[0] = '\0';
                preset_desc_buf_[0] = '\0';
                preset_dialog_is_new_ = true;
                show_save_preset_ = true;
            }
            if (ImGui::MenuItem("Save Preset...", "Ctrl+S")) {
                preset_dialog_is_new_ = false;
                show_save_preset_ = true;
            }
            if (ImGui::MenuItem("Load Preset...", "Ctrl+O")) {
                show_load_preset_ = true;
                refresh_presets(true);
            }
            bool has_selected_preset = selected_preset_index_ >= 0 &&
                                       selected_preset_index_ < static_cast<int>(preset_files_.size());
            if (ImGui::MenuItem("Delete Selected Preset", nullptr, false, has_selected_preset)) {
                delete_preset_by_index(selected_preset_index_);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Settings")) show_settings_ = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) {
                SDL_Event quit_event;
                quit_event.type = SDL_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            bool can_undo = command_history_.can_undo();
            bool can_redo = command_history_.can_redo();

            const char* undo_label = command_history_.undo_description();
            char undo_buf[64] = "Undo";
            if (undo_label) snprintf(undo_buf, sizeof(undo_buf), "Undo %s", undo_label);

            const char* redo_label = command_history_.redo_description();
            char redo_buf[64] = "Redo";
            if (redo_label) snprintf(redo_buf, sizeof(redo_buf), "Redo %s", redo_label);

            if (ImGui::MenuItem(undo_buf, "Ctrl+Z", false, can_undo)) {
                if (command_history_.undo() && pedal_board_) {
                    pedal_board_->rebuild_widgets();
                }
            }
            if (ImGui::MenuItem(redo_buf, "Ctrl+Shift+Z", false, can_redo)) {
                if (command_history_.redo() && pedal_board_) {
                    pedal_board_->rebuild_widgets();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Audio")) {
            if (engine_.is_running()) {
                if (ImGui::MenuItem("Stop Audio")) engine_.stop();
            } else {
                if (ImGui::MenuItem("Start Audio")) {
                    engine_.restart();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Restart Audio")) {
                engine_.restart();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Utilities")) {
            if (ImGui::MenuItem("Open Tuner", nullptr, show_tuner_)) {
                show_tuner_ = !show_tuner_;
                if (show_tuner_) {
                    tuner_instance_->set_enabled(true);
                    engine_.set_tuner_tap(tuner_instance_);
                } else {
                    engine_.clear_tuner_tap();
                    tuner_instance_->set_enabled(false);
                }
            }
            ImGui::EndMenu();
        }

        // Status bar (right side)
        float bar_w = ImGui::GetWindowWidth();

        // Recording indicator
        ImGui::SameLine(bar_w - 400);
        if (engine_.recorder().is_recording()) {
            float t = static_cast<float>(ImGui::GetTime());
            ImGui::TextColored(Theme::RecBlink(t), "REC");
            ImGui::SameLine();
            ImGui::Text("%.1fs", engine_.recorder().get_duration());
        }

        // Audio status
        ImGui::SameLine(bar_w - 200);
        if (engine_.is_running()) {
            ImGui::TextColored(Theme::Live(), "LIVE");
        } else {
            ImGui::TextColored(Theme::Stopped(), "STOPPED");
        }
        ImGui::SameLine();
        ImGui::Text("%dHz", engine_.get_sample_rate());

        bool show_update = false;
        std::string update_version;
        std::string update_url;
        {
            std::lock_guard<std::mutex> lock(update_mutex_);
            if (has_new_release_) {
                show_update = true;
                update_version = new_release_version_;
                update_url = new_release_url_;
            }
        }

        if (show_update) {
            ImGui::SameLine(bar_w - 600);
            ImGui::TextColored(Theme::GoldHot(), "New Release: %s", update_version.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to open release in browser");
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked()) {
#if defined(_WIN32)
                std::string cmd = "start " + update_url;
                std::system(cmd.c_str());
#elif defined(__APPLE__)
                std::string cmd = "open " + update_url;
                std::system(cmd.c_str());
#elif defined(__linux__)
                std::string cmd = "xdg-open " + update_url;
                std::system(cmd.c_str());
#endif
            }
        }

        ImGui::EndMainMenuBar();
    }

    // Error banner when audio is stopped
    if (!engine_.is_running()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.08f, 0.08f, 0.95f));
        ImGui::BeginChild("AudioErrorBanner", ImVec2(0, 36), true);
        ImGui::TextColored(Theme::Stopped(), "Audio stream is STOPPED.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart Audio")) {
            engine_.restart();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Settings")) {
            show_settings_ = true;
        }
        std::string err = engine_.get_last_error();
        if (!err.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(Theme::GoldHot(), "  %s", err.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void GuiManager::render_master_controls() {
    // Smooth metering
    float input_lvl = engine_.get_input_level();
    float output_lvl = engine_.get_output_level();
    smoothed_input_level_ += (input_lvl - smoothed_input_level_) * 0.3f;
    smoothed_output_level_ += (output_lvl - smoothed_output_level_) * 0.3f;

    ImGui::BeginChild("MasterControls", ImVec2(0, 80), true);

    ImGui::Columns(4, "master_cols", false);

    // Input gain
    ImGui::Text("INPUT");
    float input_gain = engine_.get_input_gain();
    if (ImGui::SliderFloat("##InputGain", &input_gain, 0.0f, 5.0f, "%.2f")) {
        engine_.set_input_gain(input_gain);
    }

    ImGui::NextColumn();

    // Input meter
    ImGui::Text("IN LEVEL");
    ImVec2 meter_pos = ImGui::GetCursorScreenPos();
    float meter_w = ImGui::GetColumnWidth() - 20;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(meter_pos, ImVec2(meter_pos.x + meter_w, meter_pos.y + 20),
                       Theme::METER_BG, Theme::ROUNDING_SM);
    float fill = std::min(smoothed_input_level_, 1.0f) * meter_w;
    ImU32 meter_color = (smoothed_input_level_ > 0.9f) ? Theme::METER_RED :
                        (smoothed_input_level_ > 0.6f) ? Theme::METER_YELLOW :
                                                          Theme::METER_GREEN;
    dl->AddRectFilled(meter_pos, ImVec2(meter_pos.x + fill, meter_pos.y + 20),
                       meter_color, Theme::ROUNDING_SM);
    ImGui::Dummy(ImVec2(meter_w, 20));

    ImGui::NextColumn();

    // Output meter
    ImGui::Text("OUT LEVEL");
    meter_pos = ImGui::GetCursorScreenPos();
    meter_w = ImGui::GetColumnWidth() - 20;
    dl->AddRectFilled(meter_pos, ImVec2(meter_pos.x + meter_w, meter_pos.y + 20),
                       Theme::METER_BG, Theme::ROUNDING_SM);
    fill = std::min(smoothed_output_level_, 1.0f) * meter_w;
    meter_color = (smoothed_output_level_ > 0.9f) ? Theme::METER_RED :
                  (smoothed_output_level_ > 0.6f) ? Theme::METER_YELLOW :
                                                     Theme::METER_GREEN;
    dl->AddRectFilled(meter_pos, ImVec2(meter_pos.x + fill, meter_pos.y + 20),
                       meter_color, Theme::ROUNDING_SM);
    ImGui::Dummy(ImVec2(meter_w, 20));

    ImGui::NextColumn();

    // Output gain
    ImGui::Text("OUTPUT");
    float output_gain = engine_.get_output_gain();
    if (ImGui::SliderFloat("##OutputGain", &output_gain, 0.0f, 2.0f, "%.2f")) {
        engine_.set_output_gain(output_gain);
    }

    ImGui::Columns(1);
    ImGui::EndChild();
}

void GuiManager::render_settings_window() {
    ImGui::SetNextWindowSize(ImVec2(600, 550), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Settings", &show_settings_)) {
        ImGui::End();
        return;
    }

    // --- Current routing summary ---
    ImGui::TextColored(Theme::Gold(), "SIGNAL ROUTING");
    ImGui::BeginChild("RoutingSummary", ImVec2(0, 60), true);
    ImGui::TextColored(Theme::Live(), "Guitar IN:");
    ImGui::SameLine();
    ImGui::Text("%s", engine_.get_input_device_name().c_str());
    ImGui::TextColored(ImVec4(0.35f, 0.60f, 0.95f, 1.0f), "Speaker OUT:");
    ImGui::SameLine();
    ImGui::Text("%s", engine_.get_output_device_name().c_str());
    ImGui::EndChild();

    ImGui::Spacing();

    // --- Latency settings ---
    ImGui::TextColored(Theme::Gold(), "LATENCY");

    // Buffer size
    ImGui::Text("Buffer Size (lower = less latency, more CPU):");
    int buf_size = engine_.get_buffer_size();
    const int buf_sizes[] = {32, 64, 128, 256, 512};
    const char* buf_labels[] = {"32", "64", "128", "256", "512"};
    int current_idx = 1;
    for (int i = 0; i < 5; ++i) {
        if (buf_sizes[i] == buf_size) { current_idx = i; break; }
    }
    if (ImGui::Combo("Buffer Size", &current_idx, buf_labels, 5)) {
        engine_.set_buffer_size(buf_sizes[current_idx]);
    }

    float latency_ms = 1000.0f * engine_.get_buffer_size() / engine_.get_sample_rate();
    ImGui::Text("Estimated latency: %.1f ms", latency_ms);

    // CPU load watchdog & auto-tuning
    float cpu_load = engine_.get_cpu_load();
    ImGui::Spacing();
    ImVec4 load_color = (cpu_load > 0.80f) ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f) :
                         (cpu_load > 0.50f) ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) :
                                              ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    ImGui::TextColored(load_color, "CPU Load: %.0f%%", cpu_load * 100.0f);
    ImGui::SameLine();
    ImGui::ProgressBar(cpu_load, ImVec2(150, 0));

    int suggested = engine_.get_suggested_buffer_size();
    if (suggested != engine_.get_buffer_size()) {
        ImGui::SameLine();
        char suggest_label[64];
        std::snprintf(suggest_label, sizeof(suggest_label),
                      "Switch to %d", suggested);
        if (ImGui::SmallButton(suggest_label)) {
            engine_.set_buffer_size(suggested);
        }
    }

    bool auto_buf = engine_.is_auto_buffer_enabled();
    if (ImGui::Checkbox("Auto-tune buffer size", &auto_buf)) {
        engine_.set_auto_buffer_enabled(auto_buf);
    }
    if (auto_buf && suggested != engine_.get_buffer_size()) {
        engine_.set_buffer_size(suggested);
    }
    ImGui::Spacing();

    // Sample rate
    int sr = engine_.get_sample_rate();
    const int rates[] = {44100, 48000, 96000};
    const char* rate_labels[] = {"44100", "48000", "96000"};
    int sr_idx = 1;
    for (int i = 0; i < 3; ++i) {
        if (rates[i] == sr) { sr_idx = i; break; }
    }
    if (ImGui::Combo("Sample Rate", &sr_idx, rate_labels, 3)) {
        engine_.set_sample_rate(rates[sr_idx]);
    }

    ImGui::Separator();

    // --- Input device (USB Guitar Cable) ---
    ImGui::TextColored(Theme::Gold(),
        "INPUT DEVICE (USB Guitar Cable)");
    ImGui::TextWrapped(
        "Select your USB guitar cable or audio interface. "
        "USB devices are highlighted with [USB].");

    int current_input = engine_.get_input_device();
    auto input_devs = engine_.get_input_devices();
    ImGui::BeginChild("InputDevices", ImVec2(0, 120), true);
    for (auto& dev : input_devs) {
        bool is_selected = (dev.index == current_input);
        std::string label = dev.name;
        if (dev.is_usb_device) {
            label += "  [USB]";
        }

        if (dev.is_usb_device) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldHot());
        }

        if (ImGui::Selectable(label.c_str(), is_selected)) {
            engine_.set_input_device(dev.index);
        }

        if (dev.is_usb_device) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // --- Output device ---
    ImGui::TextColored(Theme::Gold(), "OUTPUT DEVICE (Speakers/Headphones)");

    int current_output = engine_.get_output_device();
    auto output_devs = engine_.get_output_devices();
    ImGui::BeginChild("OutputDevices", ImVec2(0, 120), true);
    for (auto& dev : output_devs) {
        bool is_selected = (dev.index == current_output);
        std::string label = dev.name;
        if (dev.is_usb_device) {
            label += "  [USB - not recommended]";
        }

        if (dev.is_usb_device) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 0.7f));
        }

        if (ImGui::Selectable(label.c_str(), is_selected)) {
            engine_.set_output_device(dev.index);
        }

        if (dev.is_usb_device) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void GuiManager::render_save_preset_popup() {
    ImGui::SetNextWindowSize(ImVec2(420, 250), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Save Preset", &show_save_preset_)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Save current pedal configuration as a preset.");
    ImGui::Spacing();

    ImGui::Text("Preset Name:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##preset_name", preset_name_buf_, sizeof(preset_name_buf_));

    ImGui::Spacing();
    ImGui::Text("Description (optional):");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##preset_desc", preset_desc_buf_, sizeof(preset_desc_buf_),
                               ImVec2(-1, 60));

    ImGui::Spacing();
    if (preset_dialog_is_new_) {
        if (ImGui::Button("Save", ImVec2(100, 30))) {
            if (save_named_preset(std::string(preset_name_buf_), std::string(preset_desc_buf_))) {
                show_save_preset_ = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(100, 30))) {
            show_save_preset_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 30))) {
            show_save_preset_ = false;
        }
    } else {
        if (ImGui::Button("Save", ImVec2(120, 30))) {
            if (save_named_preset(std::string(preset_name_buf_), std::string(preset_desc_buf_))) {
                show_save_preset_ = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) {
            show_save_preset_ = false;
        }
    }

    if (!preset_status_msg_.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", preset_status_msg_.c_str());
    }

    ImGui::End();
}

void GuiManager::render_load_preset_popup() {
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Load Preset", &show_load_preset_)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Select a preset to load:");
    ImGui::Spacing();

    if (ImGui::Button("Refresh List")) {
        refresh_presets(true);
    }

    ImGui::Spacing();
    ImGui::BeginChild("PresetList", ImVec2(0, -70), true);

    if (preset_files_.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "No presets found in '%s/' folder.\nSave a preset first, or place .json files there.",
            PresetManager::get_presets_dir().c_str());
    }

    for (int i = 0; i < static_cast<int>(preset_files_.size()); ++i) {
        auto& filepath = preset_files_[i];
        // Show just the filename
        std::string display = preset_name_from_path(filepath);

        bool is_selected = (i == selected_preset_index_);
        if (ImGui::Selectable(display.c_str(), is_selected)) {
            if (load_preset_by_index(i)) {
                show_load_preset_ = false;
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(120, 30))) {
        show_load_preset_ = false;
    }

    if (!preset_status_msg_.empty()) {
        ImGui::SameLine();
        ImGui::TextWrapped("%s", preset_status_msg_.c_str());
    }

    ImGui::End();
}

void GuiManager::render_recording_controls() {
    auto& rec = engine_.recorder();
    bool is_recording = rec.is_recording();
    bool is_paused = rec.is_paused();
    bool has_unsaved = rec.has_unsaved();

    float panel_height = is_recording ? 120.0f : 40.0f;
    ImGui::BeginChild("RecordingPanel", ImVec2(0, panel_height), true,
                       ImGuiWindowFlags_NoScrollbar);

    if (is_recording) {
        // === RECORDING ACTIVE ===

        // Top row: controls + timer
        // Record/Pause button
        if (is_paused) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
            if (ImGui::Button("RESUME", ImVec2(80, 28))) {
                rec.resume();
            }
            ImGui::PopStyleColor(2);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.5f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));
            if (ImGui::Button("PAUSE", ImVec2(80, 28))) {
                rec.pause();
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::SameLine();

        // Stop button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("STOP", ImVec2(80, 28))) {
            rec.stop();
            rec.write_metadata(rec.filepath(), engine_);
            show_recording_save_ = true;
            recording_save_pending_ = true;
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        // Blinking REC indicator
        float t = static_cast<float>(ImGui::GetTime());
        if (is_paused) {
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.2f, 1.0f), "  PAUSED");
        } else {
            float blink = (std::sin(t * 4.0f) > 0.0f) ? 1.0f : 0.3f;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.15f, 0.15f, blink));
            ImGui::Text("  REC");
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();

        // Timer MM:SS.ms
        float duration = rec.get_duration();
        int mins = static_cast<int>(duration) / 60;
        int secs = static_cast<int>(duration) % 60;
        int ms = static_cast<int>((duration - static_cast<int>(duration)) * 10);
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                           "  %02d:%02d.%d", mins, secs, ms);

        ImGui::SameLine();

        // Peak meter (compact)
        float peak = rec.get_current_peak();
        ImGui::TextColored(peak > 0.9f ? ImVec4(1, 0.2f, 0.2f, 1) :
                           peak > 0.6f ? ImVec4(1, 0.8f, 0.2f, 1) :
                                         ImVec4(0.2f, 0.8f, 0.2f, 1),
                           "  Peak: %.1f dB",
                           peak > 0.0001f ? 20.0f * std::log10(peak) : -96.0f);

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);
        int64_t file_bytes = rec.get_samples_written() * 2; // 16-bit PCM
        if (file_bytes > 1024 * 1024)
            ImGui::Text("%.1f MB", file_bytes / (1024.0f * 1024.0f));
        else
            ImGui::Text("%.0f KB", file_bytes / 1024.0f);

        // === WAVEFORM DISPLAY ===
        ImGui::Spacing();
        rec.get_waveform(rec_waveform_buf_, Recorder::WAVEFORM_SIZE);

        ImVec2 wave_pos = ImGui::GetCursorScreenPos();
        float wave_w = ImGui::GetContentRegionAvail().x;
        float wave_h = 50.0f;

        ImDrawList* draw = ImGui::GetWindowDrawList();

        // Dark background for waveform
        draw->AddRectFilled(wave_pos,
                            ImVec2(wave_pos.x + wave_w, wave_pos.y + wave_h),
                            IM_COL32(20, 18, 16, 255), 4.0f);

        // Center line
        float center_y = wave_pos.y + wave_h * 0.5f;
        draw->AddLine(ImVec2(wave_pos.x, center_y),
                      ImVec2(wave_pos.x + wave_w, center_y),
                      IM_COL32(60, 55, 48, 255));

        // Waveform bars (mirrored around center)
        ImU32 wave_color = is_paused ? IM_COL32(180, 160, 50, 200)
                                      : IM_COL32(200, 80, 60, 220);
        ImU32 wave_color_bright = is_paused ? IM_COL32(220, 200, 80, 255)
                                             : IM_COL32(255, 100, 70, 255);

        int num_bars = static_cast<int>(wave_w);
        float samples_per_pixel = static_cast<float>(Recorder::WAVEFORM_SIZE) / num_bars;

        for (int i = 0; i < num_bars; ++i) {
            int idx = static_cast<int>(i * samples_per_pixel);
            if (idx >= Recorder::WAVEFORM_SIZE) idx = Recorder::WAVEFORM_SIZE - 1;
            float val = rec_waveform_buf_[idx];
            float bar_h = val * wave_h * 0.48f;
            if (bar_h < 0.5f) continue;

            float x = wave_pos.x + i;
            ImU32 col = val > 0.8f ? wave_color_bright : wave_color;
            draw->AddLine(ImVec2(x, center_y - bar_h),
                          ImVec2(x, center_y + bar_h), col);
        }

        // Border
        draw->AddRect(wave_pos,
                      ImVec2(wave_pos.x + wave_w, wave_pos.y + wave_h),
                      IM_COL32(70, 65, 55, 255), 4.0f);

        ImGui::Dummy(ImVec2(wave_w, wave_h));

    } else if (has_unsaved) {
        // === UNSAVED RECORDING ===
        ImGui::TextColored(Theme::Gold(), "Recording complete");
        ImGui::SameLine();
        ImGui::Text("  %.1f s  |  ", rec.get_duration());
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        if (ImGui::Button("Save As...", ImVec2(100, 24))) {
            show_recording_save_ = true;
            recording_save_pending_ = true;
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Discard", ImVec2(80, 24))) {
            rec.discard();
            preset_status_msg_ = "Recording discarded.";
        }
        ImGui::PopStyleColor(2);

    } else {
        // === READY STATE ===
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.05f, 0.05f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("REC", ImVec2(90, 28))) {
            std::string filepath = Recorder::generate_filename();
            rec.start(filepath, engine_.get_sample_rate());
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "  Ready to record  |  WAV 16-bit %d Hz",
                           engine_.get_sample_rate());
    }

    ImGui::EndChild();
}

void GuiManager::render_recording_save_dialog() {
    if (!recording_save_pending_) {
        show_recording_save_ = false;
        return;
    }

    // Launch native save dialog (runs on this frame, blocks briefly)
    recording_save_pending_ = false;
    show_recording_save_ = false;

    auto& rec = engine_.recorder();
    std::string dest = show_save_dialog("recording.wav", "WAV Audio", "wav");

    if (!dest.empty()) {
        if (rec.save_to(dest)) {
            preset_status_msg_ = "Saved: " + dest;
        } else {
            preset_status_msg_ = "Failed to save recording.";
        }
    }
    // If cancelled, keep as unsaved — user can save later or discard
}

void GuiManager::render_tuner_modal() {
    ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
    bool open = show_tuner_;
    if (!ImGui::Begin("Chromatic Tuner", &open)) {
        ImGui::End();
        if (!open) {
            show_tuner_ = false;
            engine_.clear_tuner_tap();
            tuner_instance_->set_enabled(false);
        }
        return;
    }
    if (!open) {
        show_tuner_ = false;
        engine_.clear_tuner_tap();
        tuner_instance_->set_enabled(false);
        ImGui::End();
        return;
    }

    TunerPedal* tuner = tuner_instance_.get();
    bool has_signal = tuner->signal_detected.load(std::memory_order_relaxed);
    int note_idx = tuner->detected_note.load(std::memory_order_relaxed);
    int octave = tuner->detected_octave.load(std::memory_order_relaxed);
    float cents = tuner->detected_cents.load(std::memory_order_relaxed);
    float freq = tuner->detected_freq.load(std::memory_order_relaxed);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float win_w = ImGui::GetContentRegionAvail().x;

    if (has_signal && note_idx >= 0) {
        // Note name (large centered)
        char note_buf[16];
        std::snprintf(note_buf, sizeof(note_buf), "%s%d",
                      TunerPedal::note_name(note_idx), octave);
        ImVec2 note_size = ImGui::CalcTextSize(note_buf);
        float scale = 3.0f;
        float note_w = note_size.x * scale;
        ImVec2 note_pos = ImGui::GetCursorScreenPos();
        note_pos.x += (win_w - note_w) * 0.5f;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
                    note_pos, Theme::TEXT_PRIMARY, note_buf);
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * scale + 8));

        // Cents text (colored)
        float abs_cents = std::fabs(cents);
        ImVec4 cents_col = (abs_cents < 2.0f)
            ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f)
            : (abs_cents < 15.0f)
                ? ImVec4(0.9f, 0.8f, 0.2f, 1.0f)
                : ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
        char cents_buf[32];
        std::snprintf(cents_buf, sizeof(cents_buf), "%+.1f cents", cents);
        ImVec2 cents_size = ImGui::CalcTextSize(cents_buf);
        ImGui::SetCursorPosX((win_w - cents_size.x) * 0.5f);
        ImGui::TextColored(cents_col, "%s", cents_buf);

        ImGui::Spacing();

        // Cents deviation bar
        float bar_w = win_w - 40;
        float bar_h = 14;
        ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        bar_pos.x += 20;
        dl->AddRectFilled(bar_pos,
            ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
            Theme::KNOB_BG, 4.0f);

        // Center tick
        float cx = bar_pos.x + bar_w * 0.5f;
        dl->AddLine(ImVec2(cx, bar_pos.y - 2), ImVec2(cx, bar_pos.y + bar_h + 2),
                    Theme::TEXT_DIM, 2.0f);

        // Needle
        float needle_norm = clamp(cents / 50.0f, -1.0f, 1.0f);
        float needle_x = cx + needle_norm * (bar_w * 0.5f);
        ImU32 needle_col = ImGui::ColorConvertFloat4ToU32(cents_col);
        dl->AddRectFilled(
            ImVec2(needle_x - 4, bar_pos.y - 3),
            ImVec2(needle_x + 4, bar_pos.y + bar_h + 3),
            needle_col, 3.0f);

        ImGui::Dummy(ImVec2(0, bar_h + 12));

        // Frequency
        char freq_buf[32];
        std::snprintf(freq_buf, sizeof(freq_buf), "%.1f Hz", freq);
        ImVec2 freq_size = ImGui::CalcTextSize(freq_buf);
        ImGui::SetCursorPosX((win_w - freq_size.x) * 0.5f);
        ImGui::TextColored(Theme::TextSecondary(), "%s", freq_buf);
    } else {
        // No signal
        const char* dash = "---";
        ImVec2 dash_size = ImGui::CalcTextSize(dash);
        float scale = 3.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        pos.x += (win_w - dash_size.x * scale) * 0.5f;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
                    pos, Theme::TEXT_DIM, dash);
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * scale + 8));

        const char* waiting = "Play a note...";
        ImVec2 wt_size = ImGui::CalcTextSize(waiting);
        ImGui::SetCursorPosX((win_w - wt_size.x) * 0.5f);
        ImGui::TextColored(Theme::TextDim(), "%s", waiting);

        ImGui::Dummy(ImVec2(0, 40));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Mute toggle
    bool mute_on = tuner->params()[0].value >= 0.5f;
    if (mute_on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.2f, 0.2f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.4f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.55f, 0.2f, 1.0f));
    }
    float btn_w = 140;
    ImGui::SetCursorPosX((win_w - btn_w) * 0.5f);
    if (ImGui::Button(mute_on ? "MUTE ON" : "MUTE OFF", ImVec2(btn_w, 30))) {
        tuner->params()[0].value = mute_on ? 0.0f : 1.0f;
    }
    ImGui::PopStyleColor(2);

    // A4 reference
    ImGui::Spacing();
    float a4_ref = tuner->params()[1].value;
    ImGui::SetNextItemWidth(win_w - 20);
    if (ImGui::SliderFloat("A4 Reference", &a4_ref, 430.0f, 450.0f, "%.0f Hz")) {
        tuner->params()[1].value = a4_ref;
    }

    ImGui::End();
}

void GuiManager::check_for_updates() {
#ifndef EMSCRIPTEN
    FILE* pipe = nullptr;
#ifdef _WIN32
    pipe = _popen("curl -s https://api.github.com/repos/sudip-mondal-2002/Amplitron/releases", "r");
#else
    pipe = popen("curl -s https://api.github.com/repos/sudip-mondal-2002/Amplitron/releases", "r");
#endif

    if (!pipe) return;

    std::string result = "";
    char buffer[256];
    while (!feof(pipe)) {
        if (fgets(buffer, 256, pipe) != nullptr)
            result += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    std::string search_str = "\"tag_name\": \"";
    size_t pos = result.find(search_str);
    if (pos != std::string::npos) {
        pos += search_str.length();
        size_t end_pos = result.find("\"", pos);
        if (end_pos != std::string::npos) {
            std::string latest_version = result.substr(pos, end_pos - pos);
            
            std::string html_url = "";
            std::string url_search_str = "\"html_url\": \"";
            size_t url_pos = result.find(url_search_str);
            if (url_pos != std::string::npos) {
                url_pos += url_search_str.length();
                size_t url_end_pos = result.find("\"", url_pos);
                if (url_end_pos != std::string::npos) {
                    html_url = result.substr(url_pos, url_end_pos - url_pos);
                }
            }

            // Parse version strings and compare numerically
            auto parse_version = [](const std::string& v) -> std::vector<int> {
                std::vector<int> parts;
                std::string s = v;
                if (!s.empty() && s[0] == 'v') s = s.substr(1);
                size_t pos = 0;
                while (pos < s.size()) {
                    size_t dot = s.find('.', pos);
                    if (dot == std::string::npos) dot = s.size();
                    try { parts.push_back(std::stoi(s.substr(pos, dot - pos))); }
                    catch (...) { parts.push_back(0); }
                    pos = dot + 1;
                }
                return parts;
            };

            std::string current_version = "v" AMPLITRON_VERSION;
            if (!latest_version.empty()) {
                auto latest_parts = parse_version(latest_version);
                auto current_parts = parse_version(current_version);
                bool is_newer = false;
                size_t max_len = std::max(latest_parts.size(), current_parts.size());
                for (size_t i = 0; i < max_len; ++i) {
                    int lv = (i < latest_parts.size()) ? latest_parts[i] : 0;
                    int cv = (i < current_parts.size()) ? current_parts[i] : 0;
                    if (lv > cv) { is_newer = true; break; }
                    if (lv < cv) { break; }
                }
                if (is_newer) {
                    std::lock_guard<std::mutex> lock(update_mutex_);
                    new_release_version_ = latest_version;
                    new_release_url_ = html_url;
                    has_new_release_ = true;
                }
            }
        }
    }
#endif
}

} // namespace GuitarAmp
