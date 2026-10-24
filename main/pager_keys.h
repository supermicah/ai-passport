#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAGER_MODE_CLICK = 0,   // 翻页：触屏向右 / 向左滑动
    PAGER_MODE_VOLUME,      // 音量：Consumer Control 音量 + / -
    PAGER_MODE_COUNT,
} pager_mode_t;

typedef enum { PAGER_PREV = 0, PAGER_NEXT } pager_dir_t;

typedef struct {
    uint8_t report_id;  // PAGER_RPT_TOUCH or PAGER_RPT_CONSUMER
    uint16_t code;      // Mouse side or Consumer usage（0x0C 页）
} pager_key_t;

#define PAGER_RPT_TOUCH    1
#define PAGER_RPT_CONSUMER 2

#define PAGER_TOUCH_PREV 1
#define PAGER_TOUCH_NEXT 2

pager_key_t pager_key_for(pager_mode_t mode, pager_dir_t dir);
const char *pager_mode_name(pager_mode_t mode);
const char *pager_mode_hint(pager_mode_t mode);
pager_mode_t pager_next_mode(pager_mode_t mode);
