#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
RTL="$HERE/../rtl"
PASS=0
FAIL=0

cc -O2 -Wall -Wextra -o "$HERE/yuv601_ref" "$HERE/yuv601_ref.c"
if "$HERE/yuv601_ref"; then
  echo "PASS C yuv601_ref"
  PASS=$((PASS+1))
else
  echo "FAIL C yuv601_ref"
  FAIL=$((FAIL+1))
fi

if ! command -v iverilog >/dev/null 2>&1; then
  echo "ERROR: iverilog is required for the FPGA YUV simulation."
  exit 1
fi

run_tb() {
  local name="$1"
  shift
  local vvp="$HERE/${name}.vvp"
  iverilog -g2012 -o "$vvp" "$@"
  if vvp "$vvp"; then
    echo "PASS $name"
    PASS=$((PASS+1))
  else
    echo "FAIL $name"
    FAIL=$((FAIL+1))
  fi
}

run_tb yuv601_tb "$HERE/yuv601_tb.v" "$RTL/yuv601_rgb.v"
run_tb yuv_plane_addr_tb "$HERE/yuv_plane_addr_tb.v" "$RTL/yuv_plane_addr.v"
run_tb fb_yuv_reader_tb "$HERE/fb_yuv_reader_tb.v" \
  "$RTL/yuv601_rgb.v" "$RTL/yuv_plane_addr.v" "$RTL/fb_line_reader.v"

echo "Simulation $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
