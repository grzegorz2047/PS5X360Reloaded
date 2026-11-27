/* SPDX-License-Identifier: MIT
 * TrinityX360 fix 186 ps5log/1 client stub. Normally a no-op (the daemon ships standalone and our
 * probe reads elevation success from a real /data root write, not from ps5log).
 *
 * Fix 186h (issue #80): a DIAGNOSTIC build. 186g proved the Lapy owned-root helper reaches its APPLY
 * step and returns status 7 (APPLY_FAILED) -- the handshake and the title check both pass -- but the
 * no-op stub hid WHICH kernel sub-step fails. This build appends the helper's own stage/error
 * telemetry (ps5log_printf / ps5log_close) to a file, so we can read the exact failing stage over
 * FTP. The helper runs as root via elfldr, so it can write under the title's /data folder. Still
 * TrinityX360's own code (MIT); not part of Lapy. Revert to the plain no-op for release.
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
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/* Diagnostic sink (fix 186h). Readable over FTP; the helper runs as root so the write succeeds. */
#define PS5LOG_STUB_FILE "/data/homebrew/PPSA83600/lapy-helper.log"

static void ps5log_stub_write(const char *prefix, const char *text) {
  char buf[640];
  int n = snprintf(buf, sizeof(buf), "%s%s\n", prefix ? prefix : "", text ? text : "");
  if (n < 0) return;
  if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
  int fd = open(PS5LOG_STUB_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd >= 0) {
    (void)!write(fd, buf, (size_t)n);
    (void)close(fd);
  }
}

int ps5log_init_default(const char *title, const char *name) {
  ps5log_stub_write("init ", name ? name : (title ? title : ""));
  return 0; /* report "ready"; the daemon proceeds */
}
void ps5log_printf(int level, const char *format, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, format);
  int n = vsnprintf(msg, sizeof(msg), format, ap);
  va_end(ap);
  if (n < 0) msg[0] = '\0';
  ps5log_stub_write(level == PS5LOG_ERR ? "ERR  " : "mark ", msg);
}
void ps5log_close(const char *message) { ps5log_stub_write("close ", message ? message : ""); }
int ps5log_raw(const char *data, size_t length) {
  if (data && length) {
    int fd = open(PS5LOG_STUB_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
      (void)!write(fd, data, length);
      (void)close(fd);
    }
  }
  return 0;
}
#endif /* PS5LOG_IMPLEMENTATION */

#endif /* PS5LOG_H_STUB */
