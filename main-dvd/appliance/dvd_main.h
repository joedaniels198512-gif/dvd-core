/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DVD_MAIN_H
#define DVD_MAIN_H

/*
 * DVD Player supervisor for the public MiSTer_DVD Main binary.
 *
 * Activate when the running executable is MiSTer_DVD (or the A2.2 rollback
 * name MiSTer_DVD_Appliance) and the loaded core name is DVD-Player
 * (or DVD-Player-Appliance for the A2.2 RBF).
 *
 * This binary must never start /media/fat/DVD/dev/dvd_launcher.
 * It starts /media/fat/DVD/bin/dvd_player only.
 *
 * Teardown is invoked from the start of app_restart() and reboot() so every
 * upstream core/binary transition and power-off path stops DVD Player
 * processes before this process forks or the SoC resets.
 */

void dvd_main_on_core_ready(void);
void dvd_main_poll(void);
void dvd_main_stop_all(void);

/*
 * OSD F0 "Play ISO..." intercept. Call from MENU_FILE_SELECT2 confirm
 * (the actual SelectFile result) and from MENU_GENERIC_FILE_SELECTED
 * (safety net before user_io_file_tx). Returns 1 if this core handled
 * the selection. Other binaries and other selector indexes return 0.
 */
int dvd_appliance_handle_iso_select(const char *sel_path, int ioctl_index);

#endif
