/* File log at ux0:data/khdays/log.txt. The previous run's log is kept as log_prev.txt,
 * because the run that matters is usually the one that crashed. */
#ifndef KH_LOG_H
#define KH_LOG_H

void log_init(const char *dir);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_flush(void);
/* Write and sync without locking: for fault handlers, which may interrupt a logging thread. */
void log_write_raw(const char *buf, int len);

/* The line logged `back` lines ago (0 = the latest), for the on-screen console. */
int log_recent(int back, char *out, int size);

#define LOG(...) log_printf(__VA_ARGS__)

#endif
