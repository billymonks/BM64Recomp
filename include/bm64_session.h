#ifndef __BM64_SESSION_H__
#define __BM64_SESSION_H__

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// Session seed and save sandbox for netplay and the determinism tests.
// Both have to be in place before the game thread seeds the RNG and reads EEPROM.
// bm64_debug_boot applies the BM64_* environment variables; netplay calls these directly.
namespace bm64::session {
    // The value g_initRandom will use. A later call replaces one that has not been consumed yet.
    void set_seed(uint32_t seed);
    bool seed_assigned();
    // Assigned seed, or a random one the first time this is called. Replay does not use this.
    uint32_t resolve_seed();

    // EEPROM reads come from this image. Writes update it and are not flushed to disk.
    // A null image keeps the save the runtime already loaded. Returns false if an image was
    // given and it does not fit the runtime buffer; flushing stays enabled in that case.
    bool begin_sandbox(const uint8_t* image, size_t size);
    bool sandbox_active();
    // Where export_if_requested writes the image when the process exits.
    void set_export_path(const std::filesystem::path& path);
    bool export_sandbox(const std::filesystem::path& path);
    void export_if_requested();

    // False while a sandbox is active, so Controller Pak calls fail with PFS_ERR_NOPACK.
    bool controller_pak_allowed();

    // Added to the eye of the lookAt matrix only. The camera's own state is not written.
    void set_camera_bias(float x, float y, float z);
    bool camera_bias_active();
    void apply_camera_bias(float& x, float& y, float& z);
}

// Implemented by the save runtime (see tools/runtime/prepare_sandbox_sources.py).
namespace ultramodern {
    void set_save_flush_enabled(bool enabled);
    bool import_save_image(const void* data, size_t size);
    size_t save_image_size();
    void export_save_image(void* out, size_t size);
}

#endif
