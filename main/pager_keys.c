#include "pager_keys.h"

// HID Usage Tables 1.4: Consumer page 0x0C.
#define CC_VOLUME_UP   0xE9
#define CC_VOLUME_DOWN 0xEA

pager_key_t pager_key_for(pager_mode_t mode, pager_dir_t dir)
{
    const bool next = dir == PAGER_NEXT;
    if (mode == PAGER_MODE_CLICK) {
        return (pager_key_t){PAGER_RPT_TOUCH, next ? PAGER_TOUCH_NEXT : PAGER_TOUCH_PREV};
    }
    return (pager_key_t){PAGER_RPT_CONSUMER, next ? CC_VOLUME_DOWN : CC_VOLUME_UP};
}

const char *pager_mode_name(pager_mode_t mode)
{
    static const char *names[PAGER_MODE_COUNT] = {"翻页", "音量键"};
    return mode < PAGER_MODE_COUNT ? names[mode] : "?";
}

const char *pager_mode_hint(pager_mode_t mode)
{
    static const char *hints[PAGER_MODE_COUNT] = {
        "上键向右\n下键向左",
        "上键音量+\n下键音量-",
    };
    return mode < PAGER_MODE_COUNT ? hints[mode] : "";
}

pager_mode_t pager_next_mode(pager_mode_t mode)
{
    return (pager_mode_t)((mode + 1) % PAGER_MODE_COUNT);
}
