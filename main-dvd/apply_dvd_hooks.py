#!/usr/bin/env python3
"""Idempotent DVD supervisor hooks for the Main_MiSTer snapshot in .src/."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / ".src"
MARKER = "DVD_MAIN_HOOK"


def once(text: str, needle: str, insert: str) -> str:
    if MARKER in insert and insert.strip() in text:
        return text
    if needle not in text:
        raise SystemExit(f"hook site not found:\n{needle[:120]}")
    return text.replace(needle, insert, 1)


def patch_fpga_io(text: str) -> str:
    if MARKER in text:
        return text
    text = once(
        text,
        '#include "offload.h"\n',
        '#include "offload.h"\n'
        '#include "dvd_main.h" /* DVD_MAIN_HOOK */\n',
    )
    text = once(
        text,
        "void reboot(int cold)\n{\n\tsync();\n",
        "void reboot(int cold)\n{\n"
        "\tdvd_main_stop_all(); /* DVD_MAIN_HOOK: before SoC reset */\n"
        "\tsync();\n",
    )
    text = once(
        text,
        "void app_restart(const char *path, const char *xml, const char *exe)\n{\n\tsync();\n",
        "void app_restart(const char *path, const char *xml, const char *exe)\n{\n"
        "\t/* DVD_MAIN_HOOK: current Main double-forks; original PID dies.\n"
        "\t   Stop DVD processes synchronously before reset/teardown/fork. */\n"
        "\tdvd_main_stop_all();\n"
        "\tsync();\n",
    )
    return text


def patch_user_io(text: str) -> str:
    if MARKER in text:
        return text
    text = once(
        text,
        '#include "fpga_io.h"\n',
        '#include "fpga_io.h"\n'
        '#include "dvd_main.h" /* DVD_MAIN_HOOK */\n',
    )
    text = once(
        text,
        "\t\tapp_restart(path, xml, main);\n"
        "\t}\n"
        "\n"
        "\tuint8_t hotswap[4] = {};\n",
        "\t\tapp_restart(path, xml, main);\n"
        "\t}\n"
        "\n"
        "\tdvd_main_on_core_ready(); /* DVD_MAIN_HOOK: staying on this binary */\n"
        "\n"
        "\tuint8_t hotswap[4] = {};\n",
    )
    text = once(
        text,
        "void user_io_poll()\n"
        "{\n"
        "\t#ifdef PROFILING\n"
        "\t\tPROFILE_FUNCTION();\n"
        "\t#endif\n",
        "void user_io_poll()\n"
        "{\n"
        "\t#ifdef PROFILING\n"
        "\t\tPROFILE_FUNCTION();\n"
        "\t#endif\n"
        "\n"
        "\tdvd_main_poll(); /* DVD_MAIN_HOOK */\n",
    )
    return text


def patch_makefile(text: str) -> str:
    if "neon-vfpv3" in text and "PRJ = MiSTer_DVD" in text:
        return text
    text = text.replace(
        'MAKEFLAGS += "-j $(shell nproc)"',
        'NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)\n'
        'MAKEFLAGS += "-j $(NPROC)"',
        1,
    )
    text = text.replace(
        "BASE    = arm-none-linux-gnueabihf",
        "BASE    = arm-unknown-linux-gnueabihf",
        1,
    )
    text = text.replace("PRJ = MiSTer\n", "PRJ = MiSTer_DVD\n", 1)
    if "neon-vfpv3" not in text:
        text += (
            "\n# DVD_MAIN_HOOK: Cortex-A9 VFPv3 (never VFPv4 / armv7-unknown toolchain)\n"
            "CFLAGS += -march=armv7-a -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard\n"
        )
    return text


def main() -> None:
    if not SRC.is_dir():
        raise SystemExit(f"missing snapshot: {SRC}")
    mapping = {
        SRC / "fpga_io.cpp": patch_fpga_io,
        SRC / "user_io.cpp": patch_user_io,
        SRC / "Makefile": patch_makefile,
    }
    for path, fn in mapping.items():
        old = path.read_text()
        new = fn(old)
        if new != old:
            path.write_text(new)
            print(f"patched {path.name}")
        else:
            print(f"unchanged {path.name}")
    for name in ("dvd_main.cpp", "dvd_main.h"):
        src = ROOT / name
        dest = SRC / name
        dest.write_bytes(src.read_bytes())
        print(f"copied {name}")


if __name__ == "__main__":
    main()
