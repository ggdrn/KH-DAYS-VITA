/* CPU time per thread group, for the log: which of the port's threads keeps which core busy
 * (a system overlay only shows the cores). */
#ifndef KH_THREADSTAT_H
#define KH_THREADSTAT_H

#include <psp2/types.h>
#include <stdint.h>

/* group: a static name; threads of the same name are summed */
void threadstat_add(const char *group, SceUID thid);
/* the calling thread's CPU time so far, us (a system call: a few a frame at most) */
uint64_t threadstat_self_us(void);
/* one line: each group's share of one core over the time since the last call */
void threadstat_log(void);

#endif
