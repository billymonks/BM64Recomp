#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include "librecomp/helpers.hpp"
#include "ultramodern/ultramodern.hpp"
#include "bm64_session.h"

namespace {
    bool seed_set = false;
    uint32_t seed_value = 0;

    bool sandbox = false;
    std::filesystem::path export_path;
    bool export_hooked = false;

    bool bias_on = false;
    float bias_x = 0.0f;
    float bias_y = 0.0f;
    float bias_z = 0.0f;

    uint32_t random_seed() {
        std::random_device rd;
        uint32_t a = rd();
        uint32_t b = rd();
        return a ^ (b * 0x9E3779B9u);
    }

    void export_atexit() {
        bm64::session::export_if_requested();
    }
}

void bm64::session::set_seed(uint32_t seed) {
    seed_set = true;
    seed_value = seed;
}

bool bm64::session::seed_assigned() {
    return seed_set;
}

uint32_t bm64::session::resolve_seed() {
    if (!seed_set) {
        seed_value = random_seed();
        seed_set = true;
    }
    printf("[bm64 session] RNG seed %08X\n", seed_value);
    return seed_value;
}

bool bm64::session::begin_sandbox(const uint8_t* image, size_t size) {
    if (image != nullptr) {
        if (!ultramodern::import_save_image(image, size)) {
            fprintf(stderr, "[bm64 session] Save image is %zu bytes; the runtime buffer is %zu. Sandbox not installed.\n",
                size, ultramodern::save_image_size());
            return false;
        }
    }
    ultramodern::set_save_flush_enabled(false);
    sandbox = true;
    printf("[bm64 session] Save sandbox on (%zu bytes). %s will not be written.\n",
        ultramodern::save_image_size(), ultramodern::get_save_file_path().string().c_str());
    return true;
}

bool bm64::session::sandbox_active() {
    return sandbox;
}

void bm64::session::set_export_path(const std::filesystem::path& path) {
    export_path = path;
    if (!export_hooked) {
        std::atexit(export_atexit);
        export_hooked = true;
    }
}

bool bm64::session::export_sandbox(const std::filesystem::path& path) {
    size_t size = ultramodern::save_image_size();
    if (size == 0 || path.empty()) {
        return false;
    }
    std::vector<uint8_t> image(size);
    ultramodern::export_save_image(image.data(), image.size());
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out{ path, std::ios::binary | std::ios::trunc };
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    out.close();
    if (!out) {
        fprintf(stderr, "[bm64 session] Could not export the save image to %s\n", path.string().c_str());
        return false;
    }
    printf("[bm64 session] Exported save image (%zu bytes) to %s\n", image.size(), path.string().c_str());
    return true;
}

void bm64::session::export_if_requested() {
    if (!export_path.empty()) {
        std::filesystem::path path = export_path;
        export_path.clear();
        export_sandbox(path);
    }
}

bool bm64::session::controller_pak_allowed() {
    return !sandbox;
}

void bm64::session::set_camera_bias(float x, float y, float z) {
    bias_x = x;
    bias_y = y;
    bias_z = z;
    bias_on = x != 0.0f || y != 0.0f || z != 0.0f;
}

bool bm64::session::camera_bias_active() {
    return bias_on;
}

extern "C" int bm64_controller_pak_allowed(void) {
    return bm64::session::controller_pak_allowed() ? 1 : 0;
}

// Called from the patched lookAt with a pointer to three floats on the MIPS stack.
// With no bias it returns without writing, so the matrix inputs stay bit-identical.
extern "C" void bm64_camera_bias(uint8_t* rdram, recomp_context* ctx) {
    if (!bias_on) {
        return;
    }
    gpr eye = ctx->r4;
    auto load = [&](int offset) {
        return std::bit_cast<float>(static_cast<uint32_t>(MEM_W(offset, eye)));
    };
    auto store = [&](int offset, float value) {
        MEM_W(offset, eye) = static_cast<int32_t>(std::bit_cast<uint32_t>(value));
    };
    store(0, load(0) + bias_x);
    store(4, load(4) + bias_y);
    store(8, load(8) + bias_z);
}
