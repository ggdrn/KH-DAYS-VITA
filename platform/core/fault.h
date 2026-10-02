#ifndef KH_FAULT_H
#define KH_FAULT_H

/* Install the kubridge fault handlers (no-op without the plugin). */
void fault_init(void);

/* For a hung game: log where the thread running the overlays' code is (pc, lr, registers). */
void fault_sample_overlays(void);

#endif
