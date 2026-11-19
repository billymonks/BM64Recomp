"""Append the main-player and per-player camera hooks to bm64.us.toml."""
from pathlib import Path

TOML = Path(__file__).resolve().parents[2] / "bm64.us.toml"
MARKER = "# BEGIN bm64 main-player readers"

SITES = [
    (0x80243E00, "func_80243DEC", 4),
    (0x80243E68, "func_80243E60", 3),
    (0x802443B4, "func_802443A0", 4),
    (0x802450EC, "func_802450DC", 4),
    (0x80245294, "func_80245280", 4),
    (0x802452F0, "func_802452E8", 3),
    (0x80245590, "func_8024557C", 4),
    (0x802455B8, "func_802455A8", 4),
    (0x8024570C, "func_802456FC", 4),
    (0x802457E8, "func_802457D8", 4),
    (0x80245F88, "func_80245F74", 4),
    (0x80249AE4, "func_80249AD4", 4),
    (0x8024A0EC, "func_8024A0DC", 16),
    (0x8024A82C, "func_8024A824", 8),
    (0x8024D8CC, "func_8024D8C4", 8),
    (0x80250694, "func_8025068C", 3),
    (0x802506C8, "func_802506B8", 31),
    (0x80250DD0, "func_80250DC8", 2),
    (0x80250E34, "func_80250E2C", 3),
    (0x802510D8, "func_802510C8", 4),
    (0x802512DC, "func_802512C0", 4),
    (0x80251300, "func_802512F8", 3),
]


def hook(func, addr, text):
    body = "\n".join(text)
    return (
        f"\n[[patches.hook]]\nfunc = \"{func}\"\nbefore_vram = 0x{addr:08X}\ntext = \"\"\"\n"
        f"{body}\n\"\"\"\n"
    )


def main():
    text = TOML.read_text(encoding="utf-8")
    if MARKER in text:
        raise SystemExit("hooks already appended")
    parts = [MARKER, "# The load of D_802AC894 has already executed. The register becomes the",
             "# per-process slot. With no session that is the word the load just read."]
    out = ["", *parts]
    for addr, func, reg in SITES:
        out.append(hook(func, addr, [
            "    extern int bm64_resolved_slot(uint8_t* rdram);",
            f"    ctx->r{reg} = (int64_t)(int32_t)bm64_resolved_slot(rdram);",
        ]))
    out.append(hook("func_80235D7C", 0x80235E08, [
        "    extern void bm64_camera_enter(uint8_t* rdram);",
        "    bm64_camera_enter(rdram);",
    ]))
    out.append(hook("func_80235D7C", 0x80235F80, [
        "    extern int bm64_camera_next(uint8_t* rdram);",
        "    extern int bm64_camera_mode_word(uint8_t* rdram);",
        "    if (bm64_camera_next(rdram)) {",
        "        ctx->r2 = (int64_t)(int32_t)bm64_camera_mode_word(rdram);",
        "        goto L_80235E08;",
        "    }",
    ]))
    out.append(hook("func_80236F54", 0x80237020, [
        "    extern void bm64_camera_enter(uint8_t* rdram);",
        "    bm64_camera_enter(rdram);",
    ]))
    out.append(hook("func_80236F54", 0x80237028, [
        "    extern int bm64_camera_next(uint8_t* rdram);",
        "    if (bm64_camera_next(rdram)) {",
        "        goto L_80237020;",
        "    }",
    ]))
    out.append(hook("func_80235934", 0x80235978, [
        "    extern int bm64_camera_port(uint8_t* rdram);",
        "    int cam_port = bm64_camera_port(rdram);",
        "    if (cam_port != 0) {",
        "        ctx->r4 = (int64_t)cam_port;",
        "    }",
    ]))
    out.append(hook("func_80234778", 0x80234850, [
        "    extern void bm64_camera_broadcast(uint8_t* rdram);",
        "    bm64_camera_broadcast(rdram);",
    ]))
    out.append(hook("func_802346C8", 0x80234710, [
        "    extern void bm64_camera_broadcast(uint8_t* rdram);",
        "    bm64_camera_broadcast(rdram);",
    ]))
    out.append("# END bm64 main-player readers\n")
    if not text.endswith("\n"):
        text += "\n"
    TOML.write_text(text + "\n".join(out), encoding="utf-8", newline="\n")
    print(f"appended {len(SITES)} reader hooks")


if __name__ == "__main__":
    main()
