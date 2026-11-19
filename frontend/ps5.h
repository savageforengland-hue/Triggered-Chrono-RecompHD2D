/* SPDX-FileCopyrightText: 2026 ct-recomp contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: PlayStation 5 homebrew (docs/PS5.md). Built only with the
 * ps5-payload-sdk toolchain (CT_PS5). The program is an ELF payload sent to
 * an ELF loader on a jailbroken console: no command line, no terminal, no
 * Python or compiler, so the frontend finds its files in fixed folders:
 *
 *   <the program's folder>  e.g. /data/homebrew/ct-recomp/ (checked first)
 *   /mnt/usb0/ct-recomp/    ROM (*.sfc), ct_game.so, ct-recomp.log
 *   /mnt/usb1/ct-recomp/    the same, on a second USB drive
 *   /data/ct-recomp/        the same, on internal storage
 *
 * A homebrew launcher may also pass the ROM's path as the one argument.
 *
 * Settings and saves go where SDL_GetPrefPath puts them on the PS5:
 * /user/home/<user id>/ct-recomp/.
 */
#ifndef CT_FRONTEND_PS5_H
#define CT_FRONTEND_PS5_H

#ifdef CT_PS5

/* Send stdout and stderr to <first folder that exists>/ct-recomp.log
   (a payload's output otherwise goes nowhere a player can read) and hide
   the system splash screen. argv0 (may be NULL) names the program's
   folder, searched first. Call first in main. */
void ps5_init(const char *argv0);

/* Fill in what the command line didn't give: the ROM (rom.sfc, else the
   first *.sfc / *.smc in a folder above) and, when want_module is set, the
   game module (ct_game.so beside it). Returns 0, or 1 when there is no ROM
   (the player has been told with a system notification). The strings are
   static. */
int ps5_find_files(const char **rom, const char **module, int want_module);

/* SDL_GetPrefPath's replacement (/data/<org>/<app>/; CMake maps it). */
char *ps5_pref_path(const char *org, const char *app);

/* The title's last opendir/readdir kernel results (title.c; zeros elsewhere). */
void ps5_dir_status(int out[3]);
/* The first 48 bytes the last getdents returned (title.c). */
const unsigned char *ps5_dir_peek(void);
/* A system notification (the toast in the top right corner). */
void ps5_notify(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#endif /* CT_PS5 */
#endif
