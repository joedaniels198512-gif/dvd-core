# DVD Player native v0.9 diagnostic

Safe/headless diagnostic. No framebuffer and no audio output.

This version deliberately skips `avformat_find_stream_info()` and tests the native stack in stages:
1. MPEG-PS open
2. packet-only demux (`av_read_frame`)
3. video/audio decoder open
4. actual decode

It also installs a SIGILL handler that prints the exact stage and ARM PC/LR addresses if an illegal instruction occurs.

Build on Mac with `./build_mac.sh`. It reuses the conservative FFmpeg libraries built by v0.8.
