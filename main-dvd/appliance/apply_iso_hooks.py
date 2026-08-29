#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Appliance-only: intercept OSD F0 ISO select before user_io_file_tx."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / ".src"
MARKER = "DVD_APPLIANCE_ISO_HOOK"


def once(text: str, needle: str, insert: str) -> str:
    if MARKER in insert and insert.strip() in text:
        return text
    if needle not in text:
        raise SystemExit(f"ISO hook site not found:\n{needle[:160]}")
    return text.replace(needle, insert, 1)


def patch_menu(text: str) -> str:
    if MARKER in text:
        return text
    text = once(
        text,
        '#include "fpga_io.h"\n',
        '#include "fpga_io.h"\n'
        '#include "dvd_main.h" /* DVD_APPLIANCE_ISO_HOOK */\n',
    )
    text = once(
        text,
        "\t\t\tif (selPath[0])\n"
        "\t\t\t{\n"
        "\n"
        "\t\t\t\tchar idx = user_io_ext_idx(selPath, fs_pFileExt) << 6 | ioctl_index;\n",
        "\t\t\tif (dvd_appliance_handle_iso_select(selPath, ioctl_index))\n"
        "\t\t\t{\n"
        "\t\t\t\t/* DVD_APPLIANCE_ISO_HOOK: send path only; never user_io_file_tx */\n"
        "\t\t\t\tMenuHide();\n"
        "\t\t\t\tbreak;\n"
        "\t\t\t}\n"
        "\n"
        "\t\t\tif (selPath[0])\n"
        "\t\t\t{\n"
        "\n"
        "\t\t\t\tchar idx = user_io_ext_idx(selPath, fs_pFileExt) << 6 | ioctl_index;\n",
    )
    return text


def main() -> None:
    path = SRC / "menu.cpp"
    if not path.is_file():
        raise SystemExit(f"missing {path}")
    old = path.read_text()
    new = patch_menu(old)
    if new != old:
        path.write_text(new)
        print(f"patched {path.name}")
    else:
        print(f"unchanged {path.name}")


if __name__ == "__main__":
    main()
