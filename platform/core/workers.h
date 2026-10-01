/* A helper thread on the core the game leaves free (core 2; core 0 is the display's, core 1
 * the game's), for splitting per-frame work: post a job, do your own share, wait. */
#ifndef KH_WORKERS_H
#define KH_WORKERS_H

void workers_init(void);
/* Run fn(arg) on the helper; one job at a time. Without a helper it runs here and now. */
void workers_post(void (*fn)(void *), void *arg);
/* Until the posted job is done. */
void workers_wait(void);

#endif
