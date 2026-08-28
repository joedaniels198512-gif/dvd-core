/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DVD_MAIN_H
#define DVD_MAIN_H

/*
 * Isolated DVD launcher supervisor for the MiSTer_DVD Main binary.
 *
 * Activate only when the running executable is MiSTer_DVD and the loaded
 * core name (CONF_STR current or original) is DVD-Player. Generic "DVD"
 * is a different core and must not match.
 *
 * Teardown is invoked from the start of app_restart() and reboot() so every
 * upstream core/binary transition and power-off path stops DVD processes
 * before this process forks or the SoC resets. Current Main_MiSTer
 * app_restart() double-forks and the original PID dies; do not rely on
 * parent/child ownership surviving a restart.
 */

void dvd_main_on_core_ready(void);
void dvd_main_poll(void);
void dvd_main_stop_all(void);

#endif
