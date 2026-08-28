# MiSTer_DVD toolchain

Custom Main **must** be built with the same GNU-A toolchain official
Main_MiSTer uses. The SS1 ships glibc 2.31 and libstdc++ 6.0.28, which only
provides **GLIBCXX up to 3.4.28**.

## Required

| Item | Value |
|---|---|
| Toolchain | ARM GNU-A **10.2-2020.11** |
| GCC | **10.2.1** |
| Prefix | **`arm-none-linux-gnueabihf`** |
| Linkage | Dynamic, same `LFLAGS` as upstream (`-lc -lstdc++ …`) |
| Max GLIBCXX | **3.4.28** (SS1 `/lib/libstdc++.so.6`) |

Official Main_MiSTer Makefile (`# using gcc version 10.2.1`):

```
BASE = arm-none-linux-gnueabihf
```

Download (from [MiSTer compile docs](https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/)
and Main_MiSTer `.devcontainer/Dockerfile`):

```
https://developer.arm.com/-/media/Files/downloads/gnu-a/10.2-2020.11/binrel/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf.tar.xz
```

On Apple Silicon, `build_mister_dvd.sh` uses the same GCC 10.2.1 target
compiler hosted for aarch64 Linux, run under Docker `linux/arm64`:

```
`gcc-arm-10.2-2020.11-aarch64-arm-none-linux-gnueabihf.tar.xz`
(SHA-256 `d169f9196e3a6c4248ee79ca85987ebce0e4ea9174c1f8d51af9b28fecf22da1`)

`./build_mister_dvd.sh` caches the tarball under `toolchains/` (gitignored)
and refuses to use any other prefix.

## Forbidden

| Prefix / package | Why |
|---|---|
| `arm-unknown-linux-gnueabihf` (Homebrew GCC 15) | Links `GLIBCXX_3.4.32`. SS1 loader rejects the binary; stock Main `execl` then `reboot(1)`. |
| `armv7-unknown-linux-gnueabihf` | Emits VFPv4 `VFMA`/`VFMS` → SIGILL on Cortex-A9. |

Do **not** fix ABI mismatches by installing a newer libstdc++ on the SS1.

## Gates (enforced by `build_mister_dvd.sh`)

1. `arm-none-linux-gnueabihf-gcc -dumpversion` is `10.2.1`.
2. `objdump` has no `vfma` / `vfms` / `vfnma` / `vfnms`.
3. `readelf -V` requires no `GLIBCXX_3.4.x` with x > 28.

## Build

```
./main-dvd/build_mister_dvd.sh
```

Does not deploy. Does not run Quartus.
