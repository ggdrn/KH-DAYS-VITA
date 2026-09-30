/* File log at ux0:data/khdays/log.txt. The previous run's log is kept as log_prev.txt,
 * because the run that matters is usually the one that crashed. */
#ifndef KH_LOG_H
#define KH_LOG_H

void log_init(const char *dir);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_flush(void);

#define LOG(...) log_printf(__VA_ARGS__)

#endif
