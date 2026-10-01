/* ux0:data/khdays/config.ini: key = value lines, '#' or ';' comments. Written with the defaults
 * when missing, so the user has something to edit. */
#ifndef KH_CONFIG_H
#define KH_CONFIG_H

typedef struct {
    int render_scale; /* 3D internal resolution: 1-3 times the DS's 256x192 (default 2) */
} KhConfig;

extern KhConfig kh_config;

void config_load(void);

#endif
