/* Helper threads for splitting per-frame work into chunks. Core 0 is the display's and core 1
 * the game's; the helper runs on core 2. A job is n chunks pulled from a shared counter, so the
 * caller and the helper balance themselves whatever else runs on their cores. */
#ifndef KH_WORKERS_H
#define KH_WORKERS_H

typedef void (*WorkFn)(int chunk, void *arg);

void workers_init(void);
/* Start fn(0..n-1, arg) on the helper; the caller may do other work, then join. */
void workers_begin(WorkFn fn, int n, void *arg);
/* Pull the chunks still left without waiting for the ones in flight (the job stays open for
 * workers_join). */
void workers_help(void);
/* Pull the chunks still left, then wait for the ones in flight. */
void workers_join(void);

#endif
