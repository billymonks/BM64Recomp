#include "recomp_ui.h"
#include "zelda_config.h"
#include "zelda_support.h"
#include "bm64_debug.h"
#include "bm64_online.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "RmlUi/Core.h"
#include "nfd.h"
#include <filesystem>

static std::string version_string;
static std::string players_label = "Players: 2";
static std::string delay_label = "Input delay: 2";
static std::string lives_label = "Lives: shared";
static int launch_players = 2;
static int launch_delay = 2;
static bool launch_unlimited = false;
static bool launch_bound = false;

Rml::DataModelHandle model_handle;

static void publish_launch_options() {
    players_label = "Players: " + std::to_string(launch_players);
    delay_label = "Input delay: " + std::to_string(launch_delay);
    lives_label = std::string("Lives: ") + (launch_unlimited ? "unlimited" : "shared");
    bm64::online::set_launch_options(launch_players, launch_delay, launch_unlimited);
    if (launch_bound) {
        model_handle.DirtyVariable("players_label");
        model_handle.DirtyVariable("delay_label");
        model_handle.DirtyVariable("lives_label");
    }
}

bool mm_rom_valid = false;

extern std::vector<recomp::GameEntry> supported_games;

void select_rom() {
    nfdnchar_t* native_path = nullptr;
    zelda64::open_file_dialog([](bool success, const std::filesystem::path& path) {
        if (success) {
            recomp::RomValidationError rom_error = recomp::select_rom(path, supported_games[0].game_id);
            switch (rom_error) {
                case recomp::RomValidationError::Good:
                    mm_rom_valid = true;
                    model_handle.DirtyVariable("mm_rom_valid");
                    break;
                case recomp::RomValidationError::FailedToOpen:
                    recompui::message_box("Failed to open ROM file.");
                    break;
                case recomp::RomValidationError::NotARom:
                    recompui::message_box("This is not a valid ROM file.");
                    break;
                case recomp::RomValidationError::IncorrectRom:
                    recompui::message_box("This ROM is not the correct game.");
                    break;
                case recomp::RomValidationError::NotYet:
                    recompui::message_box("This game isn't supported yet.");
                    break;
                case recomp::RomValidationError::IncorrectVersion:
                    recompui::message_box(
                            "This ROM is the correct game, but the wrong version.\nThis project requires the NTSC-U N64 version of the game.");
                    break;
                case recomp::RomValidationError::OtherError:
                    recompui::message_box("An unknown error has occurred.");
                    break;
            }
        }
    });
}

recompui::ContextId launcher_context;

recompui::ContextId recompui::get_launcher_context_id() {
	return launcher_context;
}

bool recompui::try_autostart_game() {
    static bool attempted = false;
    if (attempted || !bm64::debug::autostart_requested()) {
        return false;
    }
    attempted = true;
    if (!mm_rom_valid) {
        return false;
    }
    recomp::start_game(supported_games[0].game_id);
    return true;
}

class LauncherMenu : public recompui::MenuController {
public:
    LauncherMenu() {
        mm_rom_valid = recomp::is_rom_valid(supported_games[0].game_id);
    }
    ~LauncherMenu() override {

    }
    void load_document() override {
		launcher_context = recompui::create_context(zelda64::get_asset_path("launcher.rml"));
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "select_rom",
            [](const std::string& param, Rml::Event& event) {
                select_rom();
            }
        );
        recompui::register_event(listener, "rom_selected",
            [](const std::string& param, Rml::Event& event) {
                mm_rom_valid = true;
                model_handle.DirtyVariable("mm_rom_valid");
            }
        );
        recompui::register_event(listener, "start_game",
            [](const std::string& param, Rml::Event& event) {
                recomp::start_game(supported_games[0].game_id);
                recompui::hide_all_contexts();
            }
        );
        recompui::register_event(listener, "cycle_players",
            [](const std::string&, Rml::Event&) {
                launch_players = launch_players >= 4 ? 2 : launch_players + 1;
                publish_launch_options();
            }
        );
        recompui::register_event(listener, "cycle_delay",
            [](const std::string&, Rml::Event&) {
                launch_delay = launch_delay >= 4 ? 0 : launch_delay + 1;
                publish_launch_options();
            }
        );
        recompui::register_event(listener, "cycle_lives",
            [](const std::string&, Rml::Event&) {
                launch_unlimited = !launch_unlimited;
                publish_launch_options();
            }
        );
        recompui::register_event(listener, "host_local",
            [](const std::string&, Rml::Event&) {
                if (!mm_rom_valid) {
                    return;
                }
                bm64::online::prepare_loopback(true);
                recomp::start_game(supported_games[0].game_id);
                recompui::hide_all_contexts();
            }
        );
        recompui::register_event(listener, "join_local",
            [](const std::string&, Rml::Event&) {
                if (!mm_rom_valid) {
                    return;
                }
                bm64::online::prepare_loopback(false);
                recomp::start_game(supported_games[0].game_id);
                recompui::hide_all_contexts();
            }
        );
        recompui::register_event(listener, "open_controls",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::Controls);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_settings",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::General);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_mods",
            [](const std::string &param, Rml::Event &event) {
                recompui::set_config_tab(recompui::ConfigTab::Mods);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "exit_game",
            [](const std::string& param, Rml::Event& event) {
                ultramodern::quit();
            }
        );
    }
    void make_bindings(Rml::Context* context) override {
        Rml::DataModelConstructor constructor = context->CreateDataModel("launcher_model");

        constructor.Bind("mm_rom_valid", &mm_rom_valid);

        version_string = recomp::get_project_version().to_string();
        constructor.Bind("version_number", &version_string);
        constructor.Bind("players_label", &players_label);
        constructor.Bind("delay_label", &delay_label);
        constructor.Bind("lives_label", &lives_label);
        publish_launch_options();

        model_handle = constructor.GetModelHandle();
        launch_bound = true;
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_launcher_menu() {
    return std::make_unique<LauncherMenu>();
}
