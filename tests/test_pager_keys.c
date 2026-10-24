#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pager_keys.h"

int main(void)
{
    pager_key_t key = pager_key_for(PAGER_MODE_CLICK, PAGER_PREV);
    assert(key.report_id == PAGER_RPT_TOUCH);
    assert(key.code == PAGER_TOUCH_PREV);

    key = pager_key_for(PAGER_MODE_CLICK, PAGER_NEXT);
    assert(key.report_id == PAGER_RPT_TOUCH);
    assert(key.code == PAGER_TOUCH_NEXT);

    key = pager_key_for(PAGER_MODE_VOLUME, PAGER_PREV);
    assert(key.report_id == PAGER_RPT_CONSUMER);
    assert(key.code == 0xE9);

    key = pager_key_for(PAGER_MODE_VOLUME, PAGER_NEXT);
    assert(key.report_id == PAGER_RPT_CONSUMER);
    assert(key.code == 0xEA);

    assert(strcmp(pager_mode_name(PAGER_MODE_CLICK), "翻页") == 0);
    assert(strcmp(pager_mode_name(PAGER_MODE_VOLUME), "音量键") == 0);
    puts("pager key mapping tests: PASS");
    return 0;
}
