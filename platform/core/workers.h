/* Helper threads for splitting per-frame work into chunks. Core 0 is the display's and core 1
 * the game's; the helper runs on core 2. A job is n chunks pulled from a shared counter, so the
 * caller and the helper balance themselves whatever else runs on their cores. */
#ifndef KH_WORKERS_H
#define KH_WORKERS_H

/* The priority of the port's long-running threads (game, helper, display): a little below the
 * system's default, so that the system's own threads at the default -- the one putting each
 * finished frame on the screen at the VBlank among them -- never wait for a time slice behind
 * them. At the default (0.1.0 and before) a frame now and then missed its VBlank, the screen
 * showed the last one again, with the CPU and the GPU far from busy. */
#define KH_COMPUTE_PRIORITY (0x10000100 + 16)
/* the display thread, above the helper whose 2D it waits for */
#define KH_DISPLAY_PRIORITY (0x10000100 + 8)
/* the side thread (3D polygons sorted while the display thread mixes and uploads vertices):
 * above the helpers and the game, below the display thread */
#define KH_SIDE_PRIORITY (0x10000100 + 12)
/* the second helper on the game's core: below the game's threads, it runs when they wait */
#define KH_SPARE_PRIORITY (0x10000100 + 24)

typedef void (*WorkFn)(int chunk, void *arg);

void workers_init(void);
/* Start fn(0..n-1, arg) on the helper; the caller may do other work, then join. */
void workers_begin(WorkFn fn, int n, void *arg);
/* The same, with the second helper on the game's core joining in: for jobs joined a frame
 * later (the 60 fps 2D), which can wait a few milliseconds for a chunk it was taken from. */
void workers_begin_spare(WorkFn fn, int n, void *arg);
/* One job on the side thread (cores 1 or 2, above the helpers: it does not queue behind a
 * chunk job), alongside whatever job runs. 0 when there is no such thread: the caller does it
 * itself. */
int workers_side_begin(void (*fn)(void *), void *arg);
void workers_side_join(void);
/* Pull the chunks still left without waiting for the ones in flight (the job stays open for
 * workers_join). */
void workers_help(void);
/* Pull the chunks still left, then wait for the ones in flight. */
void workers_join(void);

#endif
