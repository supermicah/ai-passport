#include <assert.h>
#include <string.h>

#include "smoking_model.h"

#define DAY(n) ((uint32_t)(n) * 86400u)

static void test_smoking_record_flow(void)
{
    smoking_model_t model;
    smoking_model_init(&model);

    smoking_model_start(&model, DAY(10) + 3600);
    assert(model.persisted.screen == SMOKING_SCREEN_ACTIVE);
    smoking_model_finish(&model, DAY(10) + 3660);
    assert(model.persisted.screen == SMOKING_SCREEN_COUNT_SELECT);
    smoking_model_count_up(&model);
    assert(model.pending_cigarettes == 2);
    smoking_model_save_pending(&model, DAY(10) + 3660);

    assert(model.persisted.screen == SMOKING_SCREEN_HOME);
    assert(model.persisted.completed_count == 1);
    assert(model.persisted.last.valid == 1);
    assert(model.persisted.last.day == 10);
    assert(model.persisted.last.start_sec == 3600);
    assert(model.persisted.last.duration_sec == 60);
    assert(model.persisted.last.cigarettes == 2);
}

static void test_stats_and_interval(void)
{
    smoking_model_t model;
    smoking_model_init(&model);

    smoking_model_start(&model, DAY(4) + 86390);
    smoking_model_finish(&model, DAY(4) + 86410);
    smoking_model_save_pending(&model, DAY(5) + 10);
    smoking_model_start(&model, DAY(5) + 120);
    smoking_model_finish(&model, DAY(5) + 125);
    smoking_model_count_up(&model);
    smoking_model_save_pending(&model, DAY(5) + 125);

    smoking_totals_t today = smoking_model_today(&model, 5);
    assert(today.cigarettes == 2);
    assert(today.duration_sec == 5);
    smoking_totals_t week = smoking_model_last_7_days(&model, 5);
    assert(week.cigarettes == 3);
    assert(week.duration_sec == 25);
    assert(smoking_model_last_interval_sec(&model, DAY(6) + 125) == 86400);
}

static void add_record(smoking_model_t *model, uint32_t day, uint8_t cigarettes)
{
    smoking_model_start(model, DAY(day) + 100);
    smoking_model_finish(model, DAY(day) + 110);
    if (cigarettes >= 2) {
        smoking_model_count_up(model);
    }
    smoking_model_save_pending(model, DAY(day) + 110);
}

static void test_history_replaces_oldest(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    for (uint32_t day = 1; day <= 8; day++) {
        smoking_model_start(&model, DAY(day) + 1);
        smoking_model_finish(&model, DAY(day) + 2);
        smoking_model_save_pending(&model, DAY(day) + 2);
    }
    assert(smoking_model_today(&model, 1).cigarettes == 0);
    assert(smoking_model_today(&model, 8).cigarettes == 1);
    assert(smoking_model_last_7_days(&model, 8).cigarettes == 7);
}

static void test_restore_sanitizes_runtime_and_version(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    smoking_persist_state_t state = model.persisted;
    state.screen = SMOKING_SCREEN_ACTIVE;
    state.selected_feature = 99;

    smoking_model_restore(&model, &state);
    assert(model.persisted.screen == SMOKING_SCREEN_HOME);
    assert(model.persisted.selected_feature == SMOKING_FEATURE_SMOKE);
    assert(model.smoking_started_at == 0);

    state = model.persisted;
    state.version = 99;
    state.selected_feature = 99;
    smoking_model_restore(&model, &state);
    assert(model.persisted.version == SMOKING_MODEL_VERSION);
    assert(model.persisted.selected_feature == SMOKING_FEATURE_SMOKE);

    state = model.persisted;
    state.daily_goal = 0;
    smoking_model_restore(&model, &state);
    assert(smoking_model_daily_goal(&model) == SMOKING_DEFAULT_DAILY_GOAL);

    state = model.persisted;
    state.daily_goal = SMOKING_MAX_DAILY_GOAL + 1;
    smoking_model_restore(&model, &state);
    assert(smoking_model_daily_goal(&model) == SMOKING_DEFAULT_DAILY_GOAL);

    state = model.persisted;
    state.screen = 3;
    state.selected_feature = 2;
    smoking_model_restore(&model, &state);
    assert(model.persisted.screen == SMOKING_SCREEN_HOME);
    assert(model.persisted.selected_feature == SMOKING_FEATURE_SETTINGS);
}

static void test_idle_policy_and_feature_cycle(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SMOKE);
    smoking_model_home_prev(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SETTINGS);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SMOKE);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_FOCUS);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SETTINGS);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SMOKE);

    assert(smoking_model_idle_backlight_allowed(&model));
    smoking_model_start(&model, 1);
    assert(smoking_model_idle_backlight_allowed(&model));
    smoking_model_finish(&model, 2);
    assert(smoking_model_idle_backlight_allowed(&model));

    smoking_model_return_home(&model);
    model.persisted.selected_feature = SMOKING_FEATURE_SETTINGS;
    smoking_model_enter_selected(&model, 10);
    assert(model.persisted.screen == SMOKING_SCREEN_SETTINGS);
    assert(smoking_model_idle_backlight_allowed(&model));

    smoking_model_return_home(&model);
    model.persisted.selected_feature = SMOKING_FEATURE_FOCUS;
    smoking_model_enter_selected(&model, 10);
    assert(model.persisted.screen == SMOKING_SCREEN_FOCUS);
    assert(smoking_model_idle_backlight_allowed(&model));
}

static void test_settings_menu_and_time_edit(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    model.persisted.selected_feature = SMOKING_FEATURE_SETTINGS;
    smoking_model_enter_selected(&model, DAY(2) + 13u * 3600u + 24u * 60u + 59u);

    assert(model.persisted.screen == SMOKING_SCREEN_SETTINGS);
    assert(model.selected_setting == SMOKING_SETTING_TIME);
    assert(!model.editing_time);
    assert(model.edit_hour == 13);
    assert(model.edit_minute == 24);
    assert(!model.edit_minute_selected);

    smoking_model_settings_down(&model);
    assert(model.selected_setting == SMOKING_SETTING_GOAL);
    assert(!model.editing_time);
    smoking_model_settings_down(&model);
    assert(model.selected_setting == SMOKING_SETTING_WIFI);
    smoking_model_settings_down(&model);
    assert(model.selected_setting == SMOKING_SETTING_TIME);

    smoking_model_settings_click(&model);
    assert(model.editing_time);
    assert(!model.edit_minute_selected);
    smoking_model_settings_down(&model);
    assert(model.edit_hour == 14);
    smoking_model_settings_click(&model);
    assert(model.edit_minute_selected);
    smoking_model_settings_up(&model);
    assert(model.edit_minute == 23);
}

static void test_goal_settings_and_streaks(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    assert(smoking_model_daily_goal(&model) == SMOKING_DEFAULT_DAILY_GOAL);

    model.persisted.selected_feature = SMOKING_FEATURE_SETTINGS;
    smoking_model_enter_selected(&model, DAY(9));
    smoking_model_settings_down(&model);
    assert(model.selected_setting == SMOKING_SETTING_GOAL);
    smoking_model_settings_click(&model);
    assert(model.editing_goal);
    smoking_model_settings_up(&model);
    assert(smoking_model_daily_goal(&model) == SMOKING_DEFAULT_DAILY_GOAL - 1);
    smoking_model_settings_down(&model);
    smoking_model_settings_down(&model);
    assert(smoking_model_daily_goal(&model) == SMOKING_DEFAULT_DAILY_GOAL);
    assert(model.dirty);
    smoking_model_settings_click(&model);
    assert(!model.editing_goal);

    smoking_model_settings_down(&model);
    assert(model.selected_setting == SMOKING_SETTING_WIFI);
    smoking_model_settings_click(&model);
    assert(!model.editing_time);
    assert(!model.editing_goal);

    model.persisted.daily_goal = 2;
    add_record(&model, 10, 1);
    add_record(&model, 11, 2);
    add_record(&model, 11, 2);
    add_record(&model, 12, 1);
    add_record(&model, 13, 2);

    assert(smoking_model_goal_streak(&model, 13) == 2);
    assert(smoking_model_goal_best_streak(&model, 13) == 2);
    assert(smoking_model_goal_streak(&model, 12) == 1);
    assert(smoking_model_goal_best_streak(&model, 12) == 1);
}

static void test_focus_timer_flow(void)
{
    smoking_model_t model;
    smoking_model_init(&model);

    smoking_model_enter_focus(&model);
    assert(model.persisted.screen == SMOKING_SCREEN_FOCUS);
    assert(model.focus_mode == SMOKING_FOCUS_WORK);
    assert(!model.focus_running);
    assert(smoking_model_focus_remaining(&model, 100) == SMOKING_FOCUS_WORK_SEC);
    assert(strcmp(smoking_model_focus_mode_name(&model), "专注") == 0);

    smoking_model_focus_click(&model, 100);
    assert(model.focus_running);
    assert(smoking_model_focus_remaining(&model, 160) == SMOKING_FOCUS_WORK_SEC - 60);
    smoking_model_focus_click(&model, 160);
    assert(!model.focus_running);
    assert(smoking_model_focus_remaining(&model, 300) == SMOKING_FOCUS_WORK_SEC - 60);

    smoking_model_focus_toggle_mode(&model);
    assert(model.focus_mode == SMOKING_FOCUS_BREAK);
    assert(smoking_model_focus_remaining(&model, 300) == SMOKING_FOCUS_BREAK_SEC);
    assert(strcmp(smoking_model_focus_mode_name(&model), "休息") == 0);

    smoking_model_focus_click(&model, 400);
    assert(!smoking_model_focus_tick(&model, 400 + SMOKING_FOCUS_BREAK_SEC - 1));
    assert(smoking_model_focus_tick(&model, 400 + SMOKING_FOCUS_BREAK_SEC));
    assert(!model.focus_running);
    assert(model.focus_mode == SMOKING_FOCUS_WORK);
    assert(smoking_model_focus_remaining(&model, 800) == SMOKING_FOCUS_WORK_SEC);
}

int main(void)
{
    test_smoking_record_flow();
    test_stats_and_interval();
    test_history_replaces_oldest();
    test_restore_sanitizes_runtime_and_version();
    test_idle_policy_and_feature_cycle();
    test_settings_menu_and_time_edit();
    test_goal_settings_and_streaks();
    test_focus_timer_flow();
    return 0;
}
