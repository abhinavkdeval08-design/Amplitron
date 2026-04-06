#include "gui/gui_manager.h"
#include "gui/pedal_board.h"
#include "gui/theme.h"
#include "gui/file_dialog.h"
#include "gui/command.h"
#include "preset_manager.h"

#include "gui/gl_setup.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

#pragma GCC diagnostic push
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#pragma GCC diagnostic pop

namespace Amplitron {

GuiManager::GuiManager(AudioEngine& engine)
    : engine_(engine),
      gui_settings_(engine),
      gui_presets_(engine, command_history_),
      gui_recording_(engine),
      gui_tuner_(engine, std::make_shared<TunerPedal>()),
      gui_analyzer_(engine) {}

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

    // --- DPI scaling and font loading ---
    float dpi_scale = 1.0f;
    {
        int draw_w = window_width_, draw_h = window_height_;
        SDL_GL_GetDrawableSize(window_, &draw_w, &draw_h);
        if (window_width_ > 0)
            dpi_scale = static_cast<float>(draw_w) / static_cast<float>(window_width_);
    }

    {
        const float base_font_size = 14.0f;
        const float scaled_size    = base_font_size * dpi_scale;

        ImFont* loaded_font = nullptr;
        auto try_font = [&](const std::string& path) {
            if (!loaded_font)
                loaded_font = io.Fonts->AddFontFromFileTTF(path.c_str(), scaled_size);
        };

        char* base_path = SDL_GetBasePath();
        if (base_path) {
            try_font(std::string(base_path) + "assets/fonts/Roboto-Medium.ttf");
            // macOS app bundle: executable is in Contents/MacOS/, assets in Contents/Resources/
            try_font(std::string(base_path) + "../Resources/assets/fonts/Roboto-Medium.ttf");
            SDL_free(base_path);
        }
        try_font("assets/fonts/Roboto-Medium.ttf");
        try_font("../assets/fonts/Roboto-Medium.ttf");
        try_font("external/imgui/misc/fonts/Roboto-Medium.ttf");
        try_font("../external/imgui/misc/fonts/Roboto-Medium.ttf");

        if (!loaded_font)
            io.Fonts->AddFontDefault();

        io.FontGlobalScale = 1.0f / dpi_scale;
    }

    // Load window icon from assets/icon.svg
    {
        std::string icon_path;
        char* base = SDL_GetBasePath();
        if (base) {
            icon_path = std::string(base) + "assets/icon.svg";
            SDL_free(base);
        }
        NSVGimage* svg = nullptr;
        if (!icon_path.empty())
            svg = nsvgParseFromFile(icon_path.c_str(), "px", 96.0f);
        if (!svg)
            svg = nsvgParseFromFile("../assets/icon.svg", "px", 96.0f);
        if (!svg)
            svg = nsvgParseFromFile("assets/icon.svg", "px", 96.0f);
        if (svg) {
            const int icon_size = 64;
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
    gui_presets_.set_pedal_board(pedal_board_.get());

    PresetManager::load_config();

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

    // Keyboard shortcuts for undo/redo
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            bool mod = io.KeySuper || io.KeyCtrl;
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
    gui_recording_.render_controls();

    ImGui::Separator();

    float analyzer_reserved_h = gui_analyzer_.analyzer_reserved_height();
    ImGui::BeginChild("PedalBoardRegion", ImVec2(0, -analyzer_reserved_h), false);
    if (pedal_board_) {
        pedal_board_->render();
    }
    ImGui::EndChild();

    ImGui::Separator();
    gui_analyzer_.render();

    ImGui::End();

    // Popups / floating windows
    if (show_settings_) {
        gui_settings_.render(show_settings_);
    }
    if (show_save_preset_) {
        gui_presets_.render_save_popup(show_save_preset_);
    }
    if (show_load_preset_) {
        gui_presets_.render_load_popup(show_load_preset_);
    }
    if (gui_recording_.show_save()) {
        gui_recording_.render_save_dialog(gui_recording_.show_save());
    }
    if (show_tuner_) {
        gui_tuner_.render(show_tuner_);
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

void GuiManager::render_menu_bar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Preset...")) {
                gui_presets_.begin_new_preset();
                show_save_preset_ = true;
            }
            if (ImGui::MenuItem("Save Preset...", "Ctrl+S")) {
                gui_presets_.begin_save_preset();
                show_save_preset_ = true;
            }
            if (ImGui::MenuItem("Load Preset...", "Ctrl+O")) {
                show_load_preset_ = true;
                gui_presets_.ensure_factory_presets();
                gui_presets_.refresh_presets(true);
            }
            bool has_selected_preset = gui_presets_.selected_preset_index() >= 0 &&
                                       gui_presets_.selected_preset_index() < gui_presets_.preset_count();
            if (ImGui::MenuItem("Delete Selected Preset", nullptr, false, has_selected_preset)) {
                gui_presets_.delete_preset_by_index(gui_presets_.selected_preset_index());
            }
            ImGui::Separator();
#ifndef EMSCRIPTEN
            if (ImGui::MenuItem("Change Presets Directory...")) {
                std::string chosen = show_folder_dialog("Select Presets Directory");
                if (!chosen.empty()) {
                    PresetManager::set_presets_dir(chosen);
                    PresetManager::save_config();
                    gui_presets_.refresh_presets(false);
                }
            }
            if (ImGui::MenuItem("Reset to Default Presets Directory")) {
                PresetManager::set_presets_dir("");
                PresetManager::save_config();
                gui_presets_.refresh_presets(false);
            }
#endif
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
                gui_tuner_.toggle(show_tuner_);
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
            ImGui::TextColored(Theme::GoldHot(), "New Release Available: %s", update_version.c_str());
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

} // namespace Amplitron
