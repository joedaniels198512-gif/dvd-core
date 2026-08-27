#!/usr/bin/env python3
"""Static CONF_STR / controller / A/V-Sync consistency gate.

Cross-checks fpga/DVD.sv against the ARM player and launcher:
  - CONF_STR parses; O[]/T[]/R[] status bits do not overlap
  - CRT option is Native,Stabilized on status[9] (Native default)
  - A/V Sync is a 41-entry 5 ms wheel on status[17:12]; every OSD label
    matches the player's real av_sync_raw_to_ms() decode (compiled and run)
  - DVD2 set_word layout round-trips through av_sync_raw_from_setword()
  - OSD 0 ms == launcher --video-advance-ms 20 baseline (trim 0, additive)
  - controller bits 0-9 unchanged, bit10 Subtitle, bit11 Audio Next,
    JOY_BTN_MASK 0xFFF in player and launcher, display_buf bit31 untouched
  - CONF_STR v,3 and SET_VER 2 agree between FPGA and player
Exits non-zero on any failure.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DVD_SV = os.path.join(HERE, "..", "DVD.sv")
PLAYER_C = os.path.join(HERE, "..", "..", "player", "tools",
                        "dvd_av_threaded_test.c")
LAUNCHER_C = os.path.join(HERE, "..", "..", "player", "tools",
                          "dvd_launcher.c")

fails = []


def check(cond, msg):
    if cond:
        print("PASS " + msg)
    else:
        print("FAIL " + msg)
        fails.append(msg)


def read(path):
    with open(path, "r") as f:
        return f.read()


sv = read(DVD_SV)
player = read(PLAYER_C)
launcher = read(LAUNCHER_C)

# ---------------------------------------------------------------- CONF_STR
m = re.search(r"localparam CONF_STR = \{(.*?)\};", sv, re.S)
check(m is not None, "CONF_STR block found")
conf = "".join(re.findall(r'"([^"]*)"', m.group(1))) if m else ""
entries = [e for e in conf.split(";") if e]

# Collect status bits from O[...] options and T[...]/R[...] triggers.
opt_bits = {}   # bit -> option name (O options, mutually exclusive)
trig_bits = {}  # bit -> trigger name (T/R may share with each other)
av_entry = None
crt_entry = None
for e in entries:
    body = re.sub(r"^(H\d|h\d|D\d|d\d|P\d)+", "", e)
    mo = re.match(r"([OTR])\[(\d+)(?::(\d+))?\],([^,]+)(?:,(.*))?$", body)
    if not mo:
        continue
    kind, hi, lo, name = mo.group(1), int(mo.group(2)), mo.group(3), mo.group(4)
    lo = int(lo) if lo is not None else hi
    bits = range(min(hi, lo), max(hi, lo) + 1)
    if kind == "O":
        for b in bits:
            check(b not in opt_bits,
                  "status[%d] unique (%s vs %s)" % (b, name,
                                                    opt_bits.get(b, "-"))
                  if b in opt_bits else
                  "status[%d] '%s' has no O[] overlap" % (b, name))
            opt_bits[b] = name
        if name == "A/V Sync":
            av_entry = (min(hi, lo), max(hi, lo), mo.group(5).split(","))
        if name == "CRT":
            crt_entry = (min(hi, lo), max(hi, lo), mo.group(5).split(","))
    else:
        for b in bits:
            trig_bits[b] = name

for b in trig_bits:
    check(b not in opt_bits, "trigger bit %d does not collide with O[]" % b)

check(8 not in opt_bits and 8 not in trig_bits,
      "status[8] free (Buffer A/B debug trap removed)")
check("Buffer,A,B" not in conf, "no 'Buffer,A,B' OSD entry")
check(re.search(r"req_buf\s*\(\s*mb_bit\s*\)", sv) is not None,
      "req_buf = mb_bit (no status[8] OR)")
check("status[8]" not in sv.split("localparam CONF_STR")[1],
      "status[8] unused in RTL below CONF_STR")

check("v,3;" in conf, "CONF_STR version v,3")

# ------------------------------------------------------------------- CRT
check(crt_entry is not None and crt_entry[0] == 9 and crt_entry[1] == 9,
      "CRT option on status[9] only")
check(crt_entry is not None and crt_entry[2] == ["Native", "Stabilized"],
      "CRT values Native,Stabilized (Native = default raw 0)")
check(re.search(r"wire\s+crt_stab\s*=\s*status\[9\]\s*;", sv) is not None,
      "crt_stab = status[9] (0=Native no dup_even, 1=Stabilized)")
check(re.search(r"\.dup_even\s*\(\s*crt_stab\s*\)", sv) is not None,
      "dup_even driven by crt_stab (Duplicate Even RTL unchanged)")

# ------------------------------------------------------------- A/V Sync
check(av_entry is not None and av_entry[0] == 12 and av_entry[1] == 17,
      "A/V Sync on status[17:12] (6 bits)")
av_labels = av_entry[2] if av_entry else []
check(len(av_labels) == 41, "A/V Sync has 41 entries (got %d)" % len(av_labels))


def label_ms(s):
    mo = re.match(r"([+-]?\d+) ms$", s.strip())
    return int(mo.group(1)) if mo else None


expected = [0] + [5 * i for i in range(1, 21)] + \
           [5 * (i - 41) for i in range(21, 41)]
got = [label_ms(x) for x in av_labels]
check(got == expected, "A/V labels follow 0,+5..+100,-100..-5 wheel order")

# Compile the player's real decode functions and run them over every raw.
funcs = []
for fname in ("av_sync_raw_to_ms", "av_sync_raw_from_setword"):
    mo = re.search(r"^static (?:int|unsigned) %s\(.*?\n\}\n" % fname,
                   player, re.S | re.M)
    check(mo is not None, "extracted %s() from player source" % fname)
    if mo:
        funcs.append(mo.group(0))

harness = r"""
#include <stdio.h>
#include <stdint.h>
%s
/* Mirror of the DVD.sv set_word concatenation for the av/crt/tv fields:
 * {MAGIC, VER, seq, tv_osd, crt_stab, av_raw[4:0], av_raw[5], 4'd0,
 *  1'b1, src_std} */
static uint64_t fpga_set_word(unsigned av_raw, unsigned crt, unsigned tv)
{
    uint64_t w = 0;
    w |= (uint64_t)0x44564432u << 32;          /* SET_MAGIC */
    w |= (uint64_t)2 << 24;                    /* SET_VER 2 */
    w |= (uint64_t)(tv & 3u) << 14;
    w |= (uint64_t)(crt & 1u) << 13;
    w |= (uint64_t)(av_raw & 0x1fu) << 8;
    w |= (uint64_t)((av_raw >> 5) & 1u) << 7;
    w |= 1u << 2;                              /* yuv_cap */
    return w;
}
int main(void)
{
    static const int expected[41] = { %s };
    int fails = 0;
    unsigned raw;
    for (raw = 0; raw < 64; raw++) {
        int want = raw <= 40 ? expected[raw] : 0;
        int decoded = av_sync_raw_to_ms(raw);
        unsigned rt = av_sync_raw_from_setword(fpga_set_word(raw, 1, 2));
        if (decoded != want) {
            printf("FAIL raw %%u decode %%d want %%d\n", raw, decoded, want);
            fails++;
        }
        if (rt != raw) {
            printf("FAIL raw %%u set_word round-trip %%u\n", raw, rt);
            fails++;
        }
    }
    /* OSD 0 ms == old +20 ms internal: trim 0 over the launcher's
     * --video-advance-ms 20 baseline, additive combination. */
    if (av_sync_raw_to_ms(0) != 0) { printf("FAIL raw0\n"); fails++; }
    if (20 + av_sync_raw_to_ms(0) != 20) { printf("FAIL base0\n"); fails++; }
    if (20 + av_sync_raw_to_ms(1) != 25) { printf("FAIL base+5\n"); fails++; }
    if (20 + av_sync_raw_to_ms(40) != 15) { printf("FAIL base-5\n"); fails++; }
    return fails ? 1 : 0;
}
""" % ("\n".join(funcs), ", ".join(str(v) for v in expected))

with tempfile.TemporaryDirectory() as td:
    src = os.path.join(td, "avh.c")
    binp = os.path.join(td, "avh")
    with open(src, "w") as f:
        f.write(harness)
    r = subprocess.run(["cc", "-O2", "-Wall", "-o", binp, src],
                       capture_output=True, text=True)
    check(r.returncode == 0, "A/V harness compiles (%s)" % r.stderr.strip()[:120])
    if r.returncode == 0:
        r = subprocess.run([binp], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        check(r.returncode == 0,
              "player decode matches OSD table for all 64 raws; "
              "0ms==+20 internal, +5==+25, -5==+15")

check("return base + (int64_t)p->osd_av_trim_ms * 1000;" in player,
      "player ADDS osd trim to --video-advance-ms baseline")
check('"--video-advance-ms", "20"' in launcher,
      "launcher passes --video-advance-ms 20 baseline")

# set_word source line matches documented layout exactly.
check("wire [63:0] set_word = {SET_MAGIC, SET_VER, set_seq, tv_osd, "
      "crt_stab, av_raw[4:0], av_raw[5], 4'd0, 1'b1, src_std};" in sv,
      "DVD.sv set_word concatenation matches documented v2 layout")
check(re.search(r"SET_VER\s*=\s*8'd2", sv) is not None,
      "FPGA SET_VER == 2")
check(re.search(r"#define SET_VER\s+2\b", player) is not None,
      "player SET_VER == 2")
check(re.search(r"wire \[5:0\] av_raw\s*=\s*status\[17:12\]", sv) is not None,
      "av_raw = status[17:12]")

# ----------------------------------------------------------- Controller
j1 = jn = jp = None
for e in entries:
    if e.startswith("J1,"):
        j1 = e.split(",")[1:]
    if e.startswith("jn,"):
        jn = e.split(",")[1:]
    if e.startswith("jp,"):
        jp = e.split(",")[1:]

base6 = ["Confirm", "Back", "Play/Pause", "DVD Menu",
         "Previous Chapter", "Next Chapter"]
check(j1 is not None and j1[:6] == base6, "J1 bits 4-9 names unchanged")
check(j1 is not None and len(j1) == 8 and j1[6] == "Subtitle",
      "J1[6] = Subtitle -> joystick bit10")
check(j1 is not None and j1[7] == "Audio Next",
      "J1[7] = Audio Next -> joystick bit11")
check(jn == ["A", "B", "Start", "X", "L", "R", "Y", "Select"],
      "jn defaults: Subtitle=Y (WEST), Audio Next=Select "
      "(no free rear trigger: L/R taken by chapters)")
check(jp == jn, "jp matches jn (positional convention)")


def defs(src, name):
    mo = re.search(r"#define %s\s+(0x[0-9A-Fa-f]+|\d+)" % name, src)
    return int(mo.group(1), 0) if mo else None


for src, who in ((player, "player"), (launcher, "launcher")):
    check(defs(src, "JOY_BTN_MASK") == 0xFFF, "%s JOY_BTN_MASK == 0xFFF" % who)
    check(defs(src, "DISP_BUF_BIT") == 31, "%s display_buf bit31" % who)
fixed = {"JOY_BIT_RIGHT": 0, "JOY_BIT_LEFT": 1, "JOY_BIT_DOWN": 2,
         "JOY_BIT_UP": 3, "JOY_BIT_SELECT": 4, "JOY_BIT_BACK": 5,
         "JOY_BIT_PLAYPAUSE": 6, "JOY_BIT_MENU": 7, "JOY_BIT_PREV": 8,
         "JOY_BIT_NEXT": 9, "JOY_BIT_SUBTITLE": 10, "JOY_BIT_AUDIO_NEXT": 11}
for name, bit in fixed.items():
    check(defs(player, name) == bit, "player %s == %d" % (name, bit))
vals = [defs(player, n) for n in fixed]
check(len(set(vals)) == len(vals), "player joystick bits all unique")
check("PREV" not in player[player.find("controller_poll"):
                           player.find("/* Bounded packet queue")] or
      "prev.previous" in player, "no PREV+NEXT chord in controller_poll")
check(re.search(r"now\.previous && now\.next", player) is None and
      re.search(r"chord", player, re.I) is None,
      "no chord logic anywhere in player")

# ------------------------------------------------------------- Mailbox
for name, bit in (("MB_YUV_BIT", 1), ("MB_INTL_BIT", 2), ("MB_TFF_BIT", 3)):
    check(defs(player, name) == bit, "player %s == %d" % (name, bit))
check(re.search(r"mb_intl\s*<=\s*DDRAM_DOUT\[2\]", sv) is not None,
      "FPGA samples mb_intl from mailbox bit2")
check(re.search(r"mb_tff\s*<=\s*DDRAM_DOUT\[3\]", sv) is not None,
      "FPGA samples mb_tff from mailbox bit3")
check(re.search(r"mb_allow_vid\s*=", sv) is not None and
      re.search(r"\.mb_idle\s*\(\s*mb_allow_vid\s*\)", sv) is not None,
      "mb_allow_vid arbiter present and wired")
check(re.search(r"assign HDMI_BOB_DEINT = 1;", sv) is not None,
      "HDMI Bob (HDMI_BOB_DEINT=1) present")

# ------------------------------------------------- Quartus file coverage
# Every module defined under rtl/ that is instantiated by DVD.sv or by
# another rtl/ file must be listed in DVD.qip or files.qip, otherwise
# quartus_map fails with "instantiates undefined entity".
rtl_dir = os.path.join(HERE, "..", "rtl")
qip = read(os.path.join(HERE, "..", "DVD.qip")) + \
      read(os.path.join(HERE, "..", "files.qip"))
# IP blocks ship their own .qip next to their sources (e.g. rtl/pll.qip);
# treat files referenced there as covered too.
for fn in os.listdir(rtl_dir):
    if fn.endswith(".qip"):
        qip += read(os.path.join(rtl_dir, fn))
rtl_srcs = {}
for fn in sorted(os.listdir(rtl_dir)):
    if fn.endswith((".v", ".sv")):
        rtl_srcs[fn] = read(os.path.join(rtl_dir, fn))
all_src = sv + "".join(rtl_srcs.values())
for fn, txt in rtl_srcs.items():
    for mod in re.findall(r"^\s*module\s+(\w+)", txt, re.M):
        if re.search(r"^\s*%s\s+\w+\s*$|^\s*%s\s+\w+\s*\(|\b%s\s+u_\w+" %
                     (mod, mod, mod), all_src, re.M) or \
           re.search(r"\b%s\b\s+\w+\s*\(" % mod, all_src.replace(txt, "")):
            check(("rtl/" + fn) in qip or fn in qip,
                  "rtl/%s (module %s) listed in a .qip" % (fn, mod))

print()
if fails:
    print("check_static FAIL (%d)" % len(fails))
    sys.exit(1)
print("check_static PASS")
