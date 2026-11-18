"""Write the save-sandbox copies of librecomp's pi.cpp and pak.cpp into the build directory.

The originals stay untouched. CMake compiles the copies instead, so a session can keep
EEPROM in memory and Controller Pak access can be refused without forking N64ModernRuntime.
"""
import pathlib
import sys


def patch_pi(text: str) -> str:
    if "save_flush_enabled" in text:
        return text
    old_inc = "#include <mutex>\n"
    new_inc = "#include <mutex>\n#include <atomic>\n"
    if old_inc not in text:
        raise SystemExit("pi.cpp: #include <mutex> not found")
    text = text.replace(old_inc, new_inc, 1)

    needle = "} save_context;\n"
    insert = r'''} save_context;

// When false, EEPROM writes stay in save_buffer and the saving thread is not woken.
std::atomic<bool> save_flush_enabled{true};

static void signal_save_write() {
    if (save_flush_enabled.load(std::memory_order_relaxed)) {
        save_context.write_sempahore.signal();
    }
}

namespace ultramodern {
void set_save_flush_enabled(bool enabled);
bool import_save_image(const void* data, size_t size);
size_t save_image_size();
void export_save_image(void* out, size_t size);
}

void ultramodern::set_save_flush_enabled(bool enabled) {
    save_flush_enabled.store(enabled, std::memory_order_relaxed);
}

bool ultramodern::import_save_image(const void* data, size_t size) {
    std::lock_guard lock{ save_context.save_buffer_mutex };
    if (data == nullptr || save_context.save_buffer.empty() || size > save_context.save_buffer.size()) {
        return false;
    }
    std::memcpy(save_context.save_buffer.data(), data, size);
    if (size < save_context.save_buffer.size()) {
        std::memset(save_context.save_buffer.data() + size, 0, save_context.save_buffer.size() - size);
    }
    return true;
}

size_t ultramodern::save_image_size() {
    return save_context.save_buffer.size();
}

void ultramodern::export_save_image(void* out, size_t size) {
    std::lock_guard lock{ save_context.save_buffer_mutex };
    size_t have = save_context.save_buffer.size();
    size_t n = size < have ? size : have;
    if (out != nullptr && n != 0) {
        std::memcpy(out, save_context.save_buffer.data(), n);
    }
    if (out != nullptr && n < size) {
        std::memset(static_cast<char*>(out) + n, 0, size - n);
    }
}

'''
    if needle not in text:
        raise SystemExit("pi.cpp: save_context definition not found")
    old_signal = "save_context.write_sempahore.signal();"
    count = text.count(old_signal)
    if count != 3:
        raise SystemExit(f"pi.cpp: expected 3 save signals, found {count}")
    text = text.replace(old_signal, "signal_save_write();")
    return text.replace(needle, insert, 1)


def patch_pak(text: str) -> str:
    if "bm64_controller_pak_allowed" in text:
        return text
    anchor = '#include "helpers.hpp"\n'
    if anchor not in text:
        raise SystemExit("pak.cpp: helpers include not found")
    text = text.replace(
        "recomp_context* ctx) {\n",
        "recomp_context* ctx) {\n    if (pak_refused(ctx)) return;\n",
    )
    text = text.replace(
        "recomp_context * ctx) {\n",
        "recomp_context * ctx) {\n    if (pak_refused(ctx)) return;\n",
    )
    text = text.replace(
        anchor,
        anchor
        + "\n"
        + "extern \"C\" int bm64_controller_pak_allowed(void);\n"
        + "\n"
        + "// Sandbox sessions have no Controller Pak. Outside a sandbox the calls still\n"
        + "// return PFS_ERR_NOPACK, because this runtime does not implement the pak.\n"
        + "static int pak_refused(recomp_context* ctx) {\n"
        + "    if (bm64_controller_pak_allowed()) {\n"
        + "        return 0;\n"
        + "    }\n"
        + "    ctx->r2 = 1; // PFS_ERR_NOPACK\n"
        + "    return 1;\n"
        + "}\n",
        1,
    )
    # NumFiles reports the error and still zeroes the counts the caller asked for.
    start = text.find('extern "C" void osPfsNumFiles_recomp')
    end = text.find('extern "C" void osPfsRepairId_recomp')
    if start < 0 or end < 0 or end < start:
        raise SystemExit("pak.cpp: could not isolate osPfsNumFiles_recomp")
    numfiles = """extern \"C\" void osPfsNumFiles_recomp(uint8_t * rdram, recomp_context * ctx) {
    s32* max_files = _arg<1, s32*>(rdram, ctx);
    s32* files_used = _arg<2, s32*>(rdram, ctx);

    *max_files = 0;
    *files_used = 0;
    (void)pak_refused(ctx);
    _return<s32>(ctx, 1); // PFS_ERR_NOPACK
}

"""
    return text[:start] + numfiles + text[end:]


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: prepare_sandbox_sources.py pi.cpp pak.cpp out_dir")
    pi_path, pak_path, out_dir = sys.argv[1:]
    out = pathlib.Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    pi = patch_pi(pathlib.Path(pi_path).read_text(encoding="utf-8"))
    pak = patch_pak(pathlib.Path(pak_path).read_text(encoding="utf-8"))
    (out / "pi.cpp").write_text(pi, encoding="utf-8", newline="\n")
    (out / "pak.cpp").write_text(pak, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
