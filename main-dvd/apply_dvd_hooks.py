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


def patch_video(text: str) -> str:
    if "DVD_MAIN_HOOK: TMDS follows Monitor Sense" in text:
        return text
    text = once(
        text,
        "static int hdmi_power = 1;\n"
        "static int hdmi_need_init = 0;\n",
        "static int hdmi_power = 1;\n"
        "static int hdmi_need_init = 0;\n"
        "static int s_tmds_val = -1; /* last ADV7513 0x41 written by tmds_power(); -1 = unknown */\n",
    )
    text = once(
        text,
        "void tmds_power(int on)\n"
        "{\n"
        "\t// ADV7513 power-down control. 0 = power on, 1 = power down.\n"
        "\tif (hdmi_main_fd >= 0)\n"
        "\t{\n"
        "\t\tuint8_t val = on ? 0x10 : 0x50;\n"
        "\t\tint res = i2c_smbus_write_byte_data(hdmi_main_fd, 0x41, val);\n"
        "\t\tif (res < 0) printf(\"i2c: write error (41 %02X): %d\\n\", val, res);\n"
        "\t}\n"
        "}\n",
        "void tmds_power(int on)\n"
        "{\n"
        "\t// ADV7513 power-down control. 0 = power on, 1 = power down.\n"
        "\tif (hdmi_main_fd >= 0)\n"
        "\t{\n"
        "\t\tuint8_t val = on ? 0x10 : 0x50;\n"
        "\t\tif (s_tmds_val == (int)val) return;\n"
        "\t\tint res = i2c_smbus_write_byte_data(hdmi_main_fd, 0x41, val);\n"
        "\t\tif (res < 0) printf(\"i2c: write error (41 %02X): %d\\n\", val, res);\n"
        "\t\telse\n"
        "\t\t{\n"
        "\t\t\ts_tmds_val = val;\n"
        "\t\t\tprintf(\"[HDMI] TMDS %s (0x41=0x%02X)\\n\", on ? \"on\" : \"off\", val);\n"
        "\t\t}\n"
        "\t}\n"
        "}\n",
    )
    text = once(
        text,
        "\t\t\tbool hpd_high = (current_status & 0x40) != 0; // Bit 6: HPD pin level\n"
        "\t\t\tbool MS_high = (current_status & 0x20) != 0; // Bit 5: Monitor Sense level\n"
        "\n"
        "\t\t\t// The safe window to read EDID is when BOTH 5V power (HPD)\n"
        "\t\t\t// and internal display termination (Monitor Sense) are fully high and stable\n"
        "\t\t\tif (hpd_high && MS_high)\n"
        "\t\t\t{\n"
        "\t\t\t\thdmi_need_init = 1;\n"
        "\t\t\t\tif (hdmi_power)\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\tprintf(\"[HDMI] HPD and Monitor Sense Stable. Power up, re-initializing...\\n\");\n"
        "\t\t\t\t\tvideo_hdmi_power(1);\n"
        "\t\t\t\t}\n"
        "\t\t\t\telse\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\tprintf(\"[HDMI] HPD and Monitor Sense Stable, but HDMI is powered down. Will re-init upon wakeup.\\n\");\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\telse\n"
        "\t\t\t{\n"
        "\t\t\t\tprintf(\"[HDMI] Link lost or re-routing (HPD=%d, MS=%d)\\n\", hpd_high, MS_high);\n"
        "\t\t\t\ttmds_power(0);\n"
        "\t\t\t}\n",
        "\t\t\tbool hpd_high = (current_status & 0x40) != 0; // Bit 6: HPD pin level\n"
        "\t\t\tbool MS_high = (current_status & 0x20) != 0; // Bit 5: Monitor Sense level\n"
        "\n"
        "\t\t\t/* DVD_MAIN_HOOK: TMDS follows Monitor Sense.\n"
        "\t\t\t   SS1 HPD (0x42 bit 6) can stay low while HDMI is visibly working\n"
        "\t\t\t   and MS (bit 5) remains high. Stock treated HPD=0 as unplug and\n"
        "\t\t\t   wrote 0x41=0x50; EDID re-init still requires both signals. */\n"
        "\t\t\tif (MS_high)\n"
        "\t\t\t{\n"
        "\t\t\t\tif (hpd_high)\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\thdmi_need_init = 1;\n"
        "\t\t\t\t\tif (hdmi_power)\n"
        "\t\t\t\t\t{\n"
        "\t\t\t\t\t\tprintf(\"[HDMI] IRQ=0x%02X HPD=1 MS=1 — re-init\\n\", irq_status);\n"
        "\t\t\t\t\t\tvideo_hdmi_power(1);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\telse\n"
        "\t\t\t\t\t{\n"
        "\t\t\t\t\t\tprintf(\"[HDMI] IRQ=0x%02X HPD=1 MS=1 — HDMI off, defer re-init\\n\", irq_status);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t}\n"
        "\t\t\t\telse if (hdmi_power)\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\tint prev = s_tmds_val;\n"
        "\t\t\t\t\ttmds_power(1);\n"
        "\t\t\t\t\tif (prev != 0x10)\n"
        "\t\t\t\t\t\tprintf(\"[HDMI] IRQ=0x%02X HPD=0 MS=1 — TMDS keep/restore\\n\", irq_status);\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\telse\n"
        "\t\t\t{\n"
        "\t\t\t\tint prev = s_tmds_val;\n"
        "\t\t\t\ttmds_power(0);\n"
        "\t\t\t\tif (prev != 0x50)\n"
        "\t\t\t\t\tprintf(\"[HDMI] IRQ=0x%02X HPD=%d MS=0 — TMDS off (sink lost)\\n\", irq_status, hpd_high);\n"
        "\t\t\t}\n",
    )
    return text


def patch_makefile(text: str) -> str:
    if "neon-vfpv3" in text and "PRJ = MiSTer_DVD" in text:
        text = text.replace(
            "BASE    = arm-unknown-linux-gnueabihf",
            "BASE    = arm-none-linux-gnueabihf",
        )
        text = text.replace(
            "BASE = arm-unknown-linux-gnueabihf",
            "BASE = arm-none-linux-gnueabihf",
        )
        if "arm-none-linux-gnueabihf" not in text:
            raise SystemExit(
                "Makefile already patched but BASE is not arm-none-linux-gnueabihf"
            )
        return text
    text = text.replace(
        'MAKEFLAGS += "-j $(shell nproc)"',
        'NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)\n'
        'MAKEFLAGS += "-j $(NPROC)"',
        1,
    )
    # Keep BASE = arm-none-linux-gnueabihf (official Main_MiSTer / GCC 10.2.1).
    # Never rewrite it to arm-unknown-linux-gnueabihf (Homebrew GCC 15 →
    # GLIBCXX_3.4.32, which MiSTer libstdc++ 6.0.28 cannot load).
    if "BASE    = arm-none-linux-gnueabihf" not in text and "BASE = arm-none-linux-gnueabihf" not in text:
        raise SystemExit("Makefile BASE is not arm-none-linux-gnueabihf")
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
        SRC / "video.cpp": patch_video,
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
