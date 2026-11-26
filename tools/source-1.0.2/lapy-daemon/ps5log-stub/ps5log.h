/* SPDX-License-Identifier: MIT
 * TrinityX360 fix 186: a minimal no-op stub of the ps5log/1 client header, so the MIT/LGPL Lapy
 * owned-root daemon (mpereiraesaa/PS5-Lapy-JB-Daemon) can be built in our CI without the separate
 * logging_server client. The real ps5log sends structured telemetry to a logging server; we do not
 * use that -- the daemon is shipped as a standalone user-loaded helper and our own fix-186 probe
 * reads elevation success from a real /data root write, not from ps5log. These no-ops satisfy the
 * symbols the daemon references (ps5log_init_default, ps5log_printf, ps5log_close, ps5log_raw,
 * PS5LOG_MARK, PS5LOG_ERR). This stub is TrinityX360's own code (MIT); it is not part of Lapy.
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
int ps5log_init_default(const char *title, const char *name) {
  (void)title;
  (void)name;
  return 0; /* report "ready"; the daemon proceeds without real telemetry */
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
