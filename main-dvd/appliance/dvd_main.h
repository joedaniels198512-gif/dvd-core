/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DVD_MAIN_H
#define DVD_MAIN_H

/*
 * Isolated Appliance supervisor for the MiSTer_DVD_Appliance Main binary.
 *
 * Activate only when the running executable is MiSTer_DVD_Appliance and the
 * loaded core name (CONF_STR current or original) is DVD-Player-Appliance.
 * The launcher core "DVD-Player" and generic "DVD" must not match.
 *
 * This binary must never start /media/fat/DVD/dev/dvd_launcher.
 * It starts /media/fat/DVD_Appliance/bin/dvd_appliance only.
 *
 * Teardown is invoked from the start of app_restart() and reboot() so every
 * upstream core/binary transition and power-off path stops Appliance
 * processes before this process forks or the SoC resets.
 */

void dvd_main_on_core_ready(void);
void dvd_main_poll(void);
void dvd_main_stop_all(void);

/*
 * OSD F0 "Play ISO..." intercept. Returns 1 if this core handled the
 * selection (path sent to dvd_appliance). Must be called BEFORE
 * user_io_file_tx() so the ISO is never copied into FPGA memory.
 * Non-Appliance binaries and other selector indexes return 0.
 */
int dvd_appliance_handle_iso_select(const char *sel_path, int ioctl_index);

#endif
