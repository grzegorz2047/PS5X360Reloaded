/* SPDX-License-Identifier: MIT
 * TrinityX360 fix 186 ps5log/1 client stub: a no-op.
 *
 * The Lapy owned-root helper (lapy.elf) logs its stages through ps5log/1. TrinityX360 builds the
 * helper standalone and does not use that telemetry in release -- the elevation's success is read
 * from the app's own euid and a real /data write (xbox360ps5::elevate_probe), not from ps5log. So
 * this stub satisfies the daemon's ps5log/1 symbols and does nothing, keeping the helper quiet.
 *
 * (Fix 186h briefly made this a diagnostic sink that appended the helper's stages to
 * /data/homebrew/PPSA83600/lapy-helper.log while the cred-only APPLY path was being brought up;
 * that is reverted here for release.) TrinityX360's own code, MIT; not part of Lapy.
 */
#ifndef PS5LOG_H_STUB
#define PS5LOG_H_STUB

#include <stddef.h>

#define PS5LOG_MARK 0
#define PS5LOG_ERR 1

#ifdef __cplusplus
extern "C" {
#endif

int ps5log_init_default(const char *title, const char *name);
void ps5log_printf(int level, const char *format, ...);
void ps5log_close(const char *message);
int ps5log_raw(const char *data, size_t length);

#ifdef __cplusplus
}
#endif

#ifdef PS5LOG_IMPLEMENTATION
/* No-op: accept the calls, keep nothing. The daemon treats a zero return from init as "ready". */
int ps5log_init_default(const char *title, const char *name) {
  (void)title;
  (void)name;
  return 0;
}
void ps5log_printf(int level, const char *format, ...) {
  (void)level;
  (void)format;
}
void ps5log_close(const char *message) { (void)message; }
int ps5log_raw(const char *data, size_t length) {
  (void)data;
  (void)length;
  return 0;
}
#endif /* PS5LOG_IMPLEMENTATION */

#endif /* PS5LOG_H_STUB */
