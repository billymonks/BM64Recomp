#include "zelda_support.h"
#include <string>
#include <SDL.h>
#include "nfd.h"
#include "RmlUi/Core.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace zelda64 {
    // MARK: - Internal Helpers
    void perform_file_dialog_operation(const std::function<void(bool, const std::filesystem::path&)>& callback) {
        nfdnchar_t* native_path = nullptr;
        nfdresult_t result = NFD_OpenDialogN(&native_path, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::filesystem::path path;

        if (success) {
            path = std::filesystem::path{native_path};
            NFD_FreePathN(native_path);
        }

        callback(success, path);
    }

    void perform_file_dialog_operation_multiple(const std::function<void(bool, const std::list<std::filesystem::path>&)>& callback) {
        const nfdpathset_t* native_paths = nullptr;
        nfdresult_t result = NFD_OpenDialogMultipleN(&native_paths, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::list<std::filesystem::path> paths;
        nfdpathsetsize_t count = 0;

        if (success) {
            NFD_PathSet_GetCount(native_paths, &count);
            for (nfdpathsetsize_t i = 0; i < count; i++) {
                nfdnchar_t* cur_path = nullptr;
                nfdresult_t cur_result = NFD_PathSet_GetPathN(native_paths, i, &cur_path);
                if (cur_result == NFD_OKAY) {
                    paths.emplace_back(std::filesystem::path{cur_path});
                }
            }
            NFD_PathSet_Free(native_paths);
        }

        callback(success, paths);
    }

    // MARK: - Public API

    namespace {
        // True when `base/assets/recomp.rcss` exists. An empty base means the working directory.
        bool directory_has_assets(const std::filesystem::path& base) {
            std::error_code ec;
            return std::filesystem::is_regular_file(base / "assets" / "recomp.rcss", ec);
        }

        std::filesystem::path executable_directory() {
#ifdef _WIN32
            std::wstring buffer(32768, L'\0');
            DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size()) {
                return {};
            }
            buffer.resize(length);
            return std::filesystem::path{buffer}.parent_path();
#else
            return {};
#endif
        }
    }

    std::filesystem::path get_program_path() {
#if defined(__APPLE__)
        return get_bundle_resource_directory();
#elif defined(__linux__) && defined(RECOMP_FLATPAK)
        return "/app/bin";
#else
        // Asset paths are get_program_path()/assets/... . An empty path keeps them relative to
        // the working directory, which is the documented "run the exe from the repo root" launch.
        // Double-clicking build/BM64Recompiled.exe uses the build directory instead, so also
        // accept assets beside the executable or in its parent (the repo root).
        static const std::filesystem::path resolved = [] {
            if (directory_has_assets({})) {
                return std::filesystem::path{};
            }

            const std::filesystem::path exe_dir = executable_directory();
            if (!exe_dir.empty()) {
                if (directory_has_assets(exe_dir)) {
                    return exe_dir;
                }
                const std::filesystem::path parent = exe_dir.parent_path();
                if (directory_has_assets(parent)) {
                    return parent;
                }
            }

            return std::filesystem::path{};
        }();
        return resolved;
#endif
    }

    std::filesystem::path get_asset_path(const char* asset) {
        return get_program_path() / "assets" / asset;
    }

    void open_file_dialog(std::function<void(bool success, const std::filesystem::path& path)> callback) {
#ifdef __APPLE__
        dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation(callback);
        });
#else
        perform_file_dialog_operation(callback);
#endif
    }

    void open_file_dialog_multiple(std::function<void(bool success, const std::list<std::filesystem::path>& paths)> callback) {
#ifdef __APPLE__
        dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation_multiple(callback);
        });
#else
        perform_file_dialog_operation_multiple(callback);
#endif
    }

    void show_error_message_box(const char *title, const char *message) {
#ifdef __APPLE__
    std::string title_copy(title);
    std::string message_copy(message);

    dispatch_on_ui_thread([title_copy, message_copy] {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title_copy.c_str(), message_copy.c_str(), nullptr);
    });
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
#endif
    }
}
