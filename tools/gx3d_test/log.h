/* Host stand-in for platform/core/log.h. */
#include <stdio.h>
#define LOG(...) (printf(__VA_ARGS__), printf("\n"))
