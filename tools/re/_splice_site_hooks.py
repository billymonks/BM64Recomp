"""Put tools/re/main_player_site_hooks.toml into bm64.us.toml, replacing the block if it is there.

Run catalogue_main_player.py --emit first, then this, then tools/build-windows.ps1 -Regen.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOML = ROOT / "bm64.us.toml"
BLOCK = ROOT / "tools" / "re" / "main_player_site_hooks.toml"
BEGIN = b"# BEGIN bm64 main-player call sites"
END = b"# END bm64 main-player call sites"


def main():
    data = TOML.read_bytes()
    block = BLOCK.read_bytes().rstrip(b"\n")
    if not block.startswith(BEGIN) or not block.endswith(END):
        raise SystemExit("hook file has no markers; rerun catalogue_main_player.py --emit")
    start = data.find(BEGIN)
    if start >= 0:
        end = data.index(END, start) + len(END)
        data = data[:start] + block + data[end:]
    else:
        if not data.endswith(b"\n"):
            data += b"\n"
        data += b"\n" + block + b"\n"
    TOML.write_bytes(data)
    print(f"spliced {block.count(b'[[patches.hook]]')} hooks into {TOML.name}")


if __name__ == "__main__":
    main()
