#include "smoking_model.h"

#include <string.h>

#define SECONDS_PER_DAY 86400u
#define SMOKING_LEGACY_SCREEN_SUDOKU 3u
#define SMOKING_LEGACY_FEATURE_SETTINGS 2u
#define SMOKING_SLEEP_SESSION_MAGIC 0x31504C53u

static const smoking_feature_t FEATURE_ORDER[] = {
    SMOKING_FEATURE_SMOKE,
    SMOKING_FEATURE_FOCUS,
    SMOKING_FEATURE_RADIO,
    SMOKING_FEATURE_SETTINGS,
};

static int32_t day_from_sec(uint32_t now_sec)
{
    return (int32_t)(now_sec / SECONDS_PER_DAY);
}

static uint32_t sec_of_day(uint32_t now_sec)
{
    return now_sec % SECONDS_PER_DAY;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static bool load_sleep_session(const smoking_persist_state_t *state, uint32_t *started_at)
{
    if (read_le32(&state->reserved_game[0]) != SMOKING_SLEEP_SESSION_MAGIC) {
        return false;
    }
    uint32_t value = read_le32(&state->reserved_game[4]);
    if (!value) return false;
    if (started_at) *started_at = value;
    return true;
}

static void store_sleep_session(smoking_persist_state_t *state, uint32_t started_at)
{
    write_le32(&state->reserved_game[0], SMOKING_SLEEP_SESSION_MAGIC);
    write_le32(&state->reserved_game[4], started_at);
}

static void clear_sleep_session(smoking_persist_state_t *state)
{
    memset(state->reserved_game, 0, sizeof(state->reserved_game));
}

static bool valid_screen(uint8_t screen)
{
    return screen == SMOKING_SCREEN_HOME ||
           screen == SMOKING_SCREEN_ACTIVE ||
           screen == SMOKING_SCREEN_COUNT_SELECT ||
           screen == SMOKING_SCREEN_SETTINGS ||
           screen == SMOKING_SCREEN_FOCUS ||
           screen == SMOKING_SCREEN_RADIO;
}

static uint16_t valid_daily_goal(uint16_t goal)
{
    if (goal < SMOKING_MIN_DAILY_GOAL || goal > SMOKING_MAX_DAILY_GOAL) {
        return SMOKING_DEFAULT_DAILY_GOAL;
    }
    return goal;
}

static void sanitize_selected_feature(uint8_t *feature)
{
    if (*feature == SMOKING_LEGACY_FEATURE_SETTINGS) {
        *feature = SMOKING_FEATURE_SETTINGS;
    } else if (*feature >= SMOKING_FEATURE_COUNT) {
        *feature = SMOKING_FEATURE_SMOKE;
    }
}

static size_t feature_order_index(smoking_feature_t feature)
{
    for (size_t i = 0; i < sizeof(FEATURE_ORDER) / sizeof(FEATURE_ORDER[0]); i++) {
        if (FEATURE_ORDER[i] == feature) return i;
    }
    return 0;
}

void smoking_model_init(smoking_model_t *model)
{
    memset(model, 0, sizeof(*model));
    model->persisted.version = SMOKING_MODEL_VERSION;
    model->persisted.screen = SMOKING_SCREEN_HOME;
    model->persisted.selected_feature = SMOKING_FEATURE_SMOKE;
    model->persisted.daily_goal = SMOKING_DEFAULT_DAILY_GOAL;
    model->pending_cigarettes = 1;
    model->focus_mode = SMOKING_FOCUS_WORK;
    model->focus_remaining_sec = SMOKING_FOCUS_WORK_SEC;
}

void smoking_model_restore(smoking_model_t *model, const smoking_persist_state_t *state)
{
    smoking_model_init(model);
    if (!state || state->version != SMOKING_MODEL_VERSION) return;

    model->persisted = *state;
    uint32_t restored_start = 0;
    bool restore_active = model->persisted.screen == SMOKING_SCREEN_ACTIVE &&
                          load_sleep_session(&model->persisted, &restored_start);
    if (!valid_screen(model->persisted.screen) ||
        model->persisted.screen == SMOKING_SCREEN_COUNT_SELECT ||
        (model->persisted.screen == SMOKING_SCREEN_ACTIVE && !restore_active) ||
        model->persisted.screen == SMOKING_LEGACY_SCREEN_SUDOKU) {
        model->persisted.screen = SMOKING_SCREEN_HOME;
    }
    sanitize_selected_feature(&model->persisted.selected_feature);
    model->persisted.daily_goal = valid_daily_goal(model->persisted.daily_goal);
    for (size_t i = 0; i < SMOKING_HISTORY_DAYS; i++) {
        if (model->persisted.days[i].cigarettes > 200 ||
            model->persisted.days[i].duration_sec > SECONDS_PER_DAY) {
            memset(&model->persisted.days[i], 0, sizeof(model->persisted.days[i]));
        }
    }
    if (model->persisted.last.cigarettes > SMOKING_MAX_SESSION_CIGARETTES ||
        model->persisted.last.duration_sec > SECONDS_PER_DAY) {
        memset(&model->persisted.last, 0, sizeof(model->persisted.last));
    }
    model->smoking_started_at = restore_active ? restored_start : 0;
    model->pending_duration_sec = 0;
    model->pending_cigarettes = 1;
    model->editing_goal = false;
    model->focus_mode = SMOKING_FOCUS_WORK;
    model->focus_running = false;
    model->focus_started_at = 0;
    model->focus_remaining_sec = SMOKING_FOCUS_WORK_SEC;
    model->dirty = false;
}

smoking_persist_state_t smoking_model_persist_snapshot(const smoking_model_t *model)
{
    smoking_persist_state_t state = model->persisted;
    state.version = SMOKING_MODEL_VERSION;
    state.daily_goal = valid_daily_goal(state.daily_goal);
    if (model->persisted.screen == SMOKING_SCREEN_ACTIVE && model->smoking_started_at) {
        store_sleep_session(&state, model->smoking_started_at);
    } else {
        state.screen = SMOKING_SCREEN_HOME;
        clear_sleep_session(&state);
    }
    sanitize_selected_feature(&state.selected_feature);
    return state;
}

const char *smoking_feature_name(smoking_feature_t feature)
{
    static const char *NAMES[] = {
        "抽烟", "专注", "设置", "收音机",
    };
    return feature < SMOKING_FEATURE_COUNT ? NAMES[feature] : "";
}

smoking_feature_t smoking_model_selected_feature(const smoking_model_t *model)
{
    return (smoking_feature_t)model->persisted.selected_feature;
}

bool smoking_model_active_session(const smoking_model_t *model)
{
    return model->persisted.screen == SMOKING_SCREEN_ACTIVE &&
           model->smoking_started_at > 0;
}

void smoking_model_home_next(smoking_model_t *model)
{
    size_t count = sizeof(FEATURE_ORDER) / sizeof(FEATURE_ORDER[0]);
    size_t index = feature_order_index((smoking_feature_t)model->persisted.selected_feature);
    model->persisted.selected_feature = (uint8_t)FEATURE_ORDER[(index + 1u) % count];
}

void smoking_model_home_prev(smoking_model_t *model)
{
    size_t count = sizeof(FEATURE_ORDER) / sizeof(FEATURE_ORDER[0]);
    size_t index = feature_order_index((smoking_feature_t)model->persisted.selected_feature);
    model->persisted.selected_feature =
        (uint8_t)FEATURE_ORDER[(index + count - 1u) % count];
}

void smoking_model_enter_selected(smoking_model_t *model, uint32_t now_sec)
{
    switch ((smoking_feature_t)model->persisted.selected_feature) {
    case SMOKING_FEATURE_SMOKE:
        smoking_model_start(model, now_sec);
        break;
    case SMOKING_FEATURE_FOCUS:
        smoking_model_enter_focus(model);
        break;
    case SMOKING_FEATURE_SETTINGS:
        smoking_model_enter_time_settings(model, now_sec);
        break;
    case SMOKING_FEATURE_RADIO:
        smoking_model_enter_radio(model);
        break;
    default:
        model->persisted.selected_feature = SMOKING_FEATURE_SMOKE;
        model->persisted.screen = SMOKING_SCREEN_HOME;
        break;
    }
}

void smoking_model_enter_time_settings(smoking_model_t *model, uint32_t now_sec)
{
    model->persisted.screen = SMOKING_SCREEN_SETTINGS;
    model->edit_hour = (int)((now_sec % SECONDS_PER_DAY) / 3600u);
    model->edit_minute = (int)((now_sec % 3600u) / 60u);
    model->edit_minute_selected = false;
    model->edit_today_day = day_from_sec(now_sec);
    model->selected_setting = SMOKING_SETTING_TIME;
    model->editing_time = false;
    model->editing_goal = false;
    model->editing_today = false;
}

void smoking_model_return_home(smoking_model_t *model)
{
    model->persisted.screen = SMOKING_SCREEN_HOME;
    model->smoking_started_at = 0;
    model->pending_duration_sec = 0;
    model->pending_cigarettes = 1;
    model->editing_time = false;
    model->editing_goal = false;
    model->editing_today = false;
    model->focus_running = false;
}

void smoking_model_start(smoking_model_t *model, uint32_t now_sec)
{
    model->persisted.screen = SMOKING_SCREEN_ACTIVE;
    model->smoking_started_at = now_sec;
    model->pending_duration_sec = 0;
    model->pending_cigarettes = 1;
    model->dirty = true;
}

void smoking_model_finish(smoking_model_t *model, uint32_t now_sec)
{
    if (model->persisted.screen != SMOKING_SCREEN_ACTIVE) return;
    uint32_t duration = now_sec > model->smoking_started_at ?
        now_sec - model->smoking_started_at : 1u;
    if (duration == 0) duration = 1;
    model->pending_duration_sec = duration;
    model->pending_cigarettes = 1;
    model->persisted.screen = SMOKING_SCREEN_COUNT_SELECT;
    model->dirty = true;
}

void smoking_model_count_up(smoking_model_t *model)
{
    if (model->pending_cigarettes < SMOKING_MAX_SESSION_CIGARETTES) {
        model->pending_cigarettes++;
    }
}

void smoking_model_count_down(smoking_model_t *model)
{
    if (model->pending_cigarettes > 1) model->pending_cigarettes--;
}

static smoking_day_stat_t *find_day_slot(smoking_model_t *model, int32_t day)
{
    smoking_day_stat_t *oldest = &model->persisted.days[0];
    for (size_t i = 0; i < SMOKING_HISTORY_DAYS; i++) {
        smoking_day_stat_t *slot = &model->persisted.days[i];
        if (slot->day == day) return slot;
        if (slot->cigarettes == 0 && slot->duration_sec == 0) return slot;
        if (slot->day < oldest->day) oldest = slot;
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->day = day;
    return oldest;
}

void smoking_model_save_pending(smoking_model_t *model, uint32_t now_sec)
{
    (void)now_sec;
    if (model->persisted.screen != SMOKING_SCREEN_COUNT_SELECT) return;
    uint32_t duration = model->pending_duration_sec ? model->pending_duration_sec : 1u;
    uint8_t cigarettes = model->pending_cigarettes;
    if (cigarettes < 1) cigarettes = 1;
    if (cigarettes > SMOKING_MAX_SESSION_CIGARETTES) {
        cigarettes = SMOKING_MAX_SESSION_CIGARETTES;
    }

    int32_t day = day_from_sec(model->smoking_started_at);
    smoking_day_stat_t *slot = find_day_slot(model, day);
    slot->day = day;
    slot->cigarettes += cigarettes;
    slot->duration_sec += duration;

    model->persisted.last.day = day;
    model->persisted.last.start_sec = sec_of_day(model->smoking_started_at);
    model->persisted.last.duration_sec = duration;
    model->persisted.last.cigarettes = cigarettes;
    model->persisted.last.valid = 1;
    clear_sleep_session(&model->persisted);
    model->persisted.completed_count++;
    model->dirty = true;
    smoking_model_return_home(model);
}

smoking_totals_t smoking_model_today(const smoking_model_t *model, int32_t today)
{
    smoking_totals_t totals = {0};
    for (size_t i = 0; i < SMOKING_HISTORY_DAYS; i++) {
        if (model->persisted.days[i].day == today) {
            totals.cigarettes += model->persisted.days[i].cigarettes;
            totals.duration_sec += model->persisted.days[i].duration_sec;
        }
    }
    return totals;
}

smoking_totals_t smoking_model_last_7_days(const smoking_model_t *model, int32_t today)
{
    smoking_totals_t totals = {0};
    for (size_t i = 0; i < SMOKING_HISTORY_DAYS; i++) {
        int32_t delta = today - model->persisted.days[i].day;
        if (delta >= 0 && delta < (int32_t)SMOKING_HISTORY_DAYS) {
            totals.cigarettes += model->persisted.days[i].cigarettes;
            totals.duration_sec += model->persisted.days[i].duration_sec;
        }
    }
    return totals;
}

uint32_t smoking_model_last_interval_sec(const smoking_model_t *model, uint32_t now_sec)
{
    const smoking_record_t *last = &model->persisted.last;
    if (!last->valid) return 0;
    uint32_t end_sec = (uint32_t)last->day * SECONDS_PER_DAY +
                       last->start_sec + last->duration_sec;
    return now_sec > end_sec ? now_sec - end_sec : 0;
}

uint16_t smoking_model_daily_goal(const smoking_model_t *model)
{
    return valid_daily_goal(model->persisted.daily_goal);
}

uint32_t smoking_model_today_cigarettes(const smoking_model_t *model, int32_t today)
{
    return smoking_model_today(model, today).cigarettes;
}

static bool has_any_record(const smoking_model_t *model, int32_t *oldest_day)
{
    bool found = false;
    int32_t oldest = 0;
    for (size_t i = 0; i < SMOKING_HISTORY_DAYS; i++) {
        const smoking_day_stat_t *slot = &model->persisted.days[i];
        if (slot->cigarettes == 0 && slot->duration_sec == 0) continue;
        if (!found || slot->day < oldest) {
            oldest = slot->day;
        }
        found = true;
    }
    if (oldest_day) *oldest_day = oldest;
    return found;
}

static uint32_t cigarettes_on_day(const smoking_model_t *model, int32_t day)
{
    return smoking_model_today(model, day).cigarettes;
}

uint8_t smoking_model_goal_streak(const smoking_model_t *model, int32_t today)
{
    int32_t oldest = 0;
    if (!has_any_record(model, &oldest)) return 0;

    uint16_t goal = smoking_model_daily_goal(model);
    uint8_t streak = 0;
    int32_t earliest = today - (int32_t)SMOKING_HISTORY_DAYS + 1;
    if (earliest < oldest) earliest = oldest;
    for (int32_t day = today; day >= earliest; day--) {
        if (cigarettes_on_day(model, day) > goal) break;
        streak++;
    }
    return streak;
}

uint8_t smoking_model_goal_best_streak(const smoking_model_t *model, int32_t today)
{
    int32_t oldest = 0;
    if (!has_any_record(model, &oldest)) return 0;

    uint16_t goal = smoking_model_daily_goal(model);
    uint8_t best = 0;
    uint8_t current = 0;
    int32_t earliest = today - (int32_t)SMOKING_HISTORY_DAYS + 1;
    if (earliest < oldest) earliest = oldest;
    for (int32_t day = earliest; day <= today; day++) {
        if (cigarettes_on_day(model, day) <= goal) {
            current++;
            if (current > best) best = current;
        } else {
            current = 0;
        }
    }
    return best;
}

bool smoking_model_idle_backlight_allowed(const smoking_model_t *model)
{
    switch ((smoking_screen_t)model->persisted.screen) {
    case SMOKING_SCREEN_HOME:
    case SMOKING_SCREEN_ACTIVE:
    case SMOKING_SCREEN_COUNT_SELECT:
    case SMOKING_SCREEN_SETTINGS:
    case SMOKING_SCREEN_FOCUS:
    case SMOKING_SCREEN_RADIO:
        return true;
    default:
        return false;
    }
}

void smoking_model_time_up(smoking_model_t *model)
{
    if (!model->editing_time) {
        smoking_model_settings_up(model);
        return;
    }
    if (model->edit_minute_selected) {
        model->edit_minute = (model->edit_minute + 59) % 60;
    } else {
        model->edit_hour = (model->edit_hour + 23) % 24;
    }
}

void smoking_model_time_down(smoking_model_t *model)
{
    if (!model->editing_time) {
        smoking_model_settings_down(model);
        return;
    }
    if (model->edit_minute_selected) {
        model->edit_minute = (model->edit_minute + 1) % 60;
    } else {
        model->edit_hour = (model->edit_hour + 1) % 24;
    }
}

void smoking_model_time_click(smoking_model_t *model)
{
    if (!model->editing_time) {
        smoking_model_settings_click(model);
        return;
    }
    model->edit_minute_selected = !model->edit_minute_selected;
}

void smoking_model_settings_up(smoking_model_t *model)
{
    if (model->editing_today) {
        smoking_day_stat_t *slot = find_day_slot(model, model->edit_today_day);
        if (slot->cigarettes > 0) {
            slot->cigarettes--;
            model->dirty = true;
        }
        return;
    }
    if (model->editing_goal) {
        if (model->persisted.daily_goal > SMOKING_MIN_DAILY_GOAL) {
            model->persisted.daily_goal--;
            model->dirty = true;
        }
        return;
    }
    if (model->editing_time) {
        smoking_model_time_up(model);
        return;
    }
    model->selected_setting =
        (uint8_t)((model->selected_setting + SMOKING_SETTING_COUNT - 1u) %
                  SMOKING_SETTING_COUNT);
}

void smoking_model_settings_down(smoking_model_t *model)
{
    if (model->editing_today) {
        smoking_day_stat_t *slot = find_day_slot(model, model->edit_today_day);
        if (slot->cigarettes < SMOKING_MAX_DAILY_CIGARETTES) {
            slot->cigarettes++;
            model->dirty = true;
        }
        return;
    }
    if (model->editing_goal) {
        if (model->persisted.daily_goal < SMOKING_MAX_DAILY_GOAL) {
            model->persisted.daily_goal++;
            model->dirty = true;
        }
        return;
    }
    if (model->editing_time) {
        smoking_model_time_down(model);
        return;
    }
    model->selected_setting =
        (uint8_t)((model->selected_setting + 1u) % SMOKING_SETTING_COUNT);
}

void smoking_model_settings_click(smoking_model_t *model)
{
    if (model->editing_time) {
        smoking_model_time_click(model);
        return;
    }
    if (model->editing_goal) {
        model->editing_goal = false;
        return;
    }
    if (model->editing_today) {
        model->editing_today = false;
        return;
    }
    if (model->selected_setting == SMOKING_SETTING_TIME) {
        model->editing_time = true;
        model->edit_minute_selected = false;
    } else if (model->selected_setting == SMOKING_SETTING_GOAL) {
        model->persisted.daily_goal = smoking_model_daily_goal(model);
        model->editing_goal = true;
    } else if (model->selected_setting == SMOKING_SETTING_TODAY) {
        smoking_day_stat_t *slot = find_day_slot(model, model->edit_today_day);
        slot->day = model->edit_today_day;
        model->editing_today = true;
    }
}

static uint32_t focus_default_remaining(const smoking_model_t *model)
{
    return model->focus_mode == SMOKING_FOCUS_BREAK ?
        SMOKING_FOCUS_BREAK_SEC : SMOKING_FOCUS_WORK_SEC;
}

void smoking_model_enter_focus(smoking_model_t *model)
{
    model->persisted.screen = SMOKING_SCREEN_FOCUS;
    model->focus_mode = SMOKING_FOCUS_WORK;
    model->focus_running = false;
    model->focus_started_at = 0;
    model->focus_remaining_sec = SMOKING_FOCUS_WORK_SEC;
}

uint32_t smoking_model_focus_remaining(const smoking_model_t *model, uint32_t now_sec)
{
    if (!model->focus_running) return model->focus_remaining_sec;
    uint32_t elapsed = now_sec > model->focus_started_at ?
        now_sec - model->focus_started_at : 0;
    return elapsed >= model->focus_remaining_sec ? 0 :
        model->focus_remaining_sec - elapsed;
}

void smoking_model_focus_click(smoking_model_t *model, uint32_t now_sec)
{
    if (model->persisted.screen != SMOKING_SCREEN_FOCUS) return;
    if (model->focus_running) {
        model->focus_remaining_sec = smoking_model_focus_remaining(model, now_sec);
        model->focus_running = false;
        return;
    }
    if (model->focus_remaining_sec == 0) {
        model->focus_remaining_sec = focus_default_remaining(model);
    }
    model->focus_started_at = now_sec;
    model->focus_running = true;
}

void smoking_model_focus_reset(smoking_model_t *model)
{
    model->focus_running = false;
    model->focus_started_at = 0;
    model->focus_remaining_sec = focus_default_remaining(model);
}

void smoking_model_focus_toggle_mode(smoking_model_t *model)
{
    if (model->focus_running) return;
    model->focus_mode = model->focus_mode == SMOKING_FOCUS_WORK ?
        SMOKING_FOCUS_BREAK : SMOKING_FOCUS_WORK;
    model->focus_remaining_sec = focus_default_remaining(model);
}

bool smoking_model_focus_tick(smoking_model_t *model, uint32_t now_sec)
{
    if (model->persisted.screen != SMOKING_SCREEN_FOCUS || !model->focus_running) {
        return false;
    }
    if (smoking_model_focus_remaining(model, now_sec) > 0) return false;

    model->focus_running = false;
    model->focus_mode = model->focus_mode == SMOKING_FOCUS_WORK ?
        SMOKING_FOCUS_BREAK : SMOKING_FOCUS_WORK;
    model->focus_started_at = 0;
    model->focus_remaining_sec = focus_default_remaining(model);
    return true;
}

const char *smoking_model_focus_mode_name(const smoking_model_t *model)
{
    return model->focus_mode == SMOKING_FOCUS_BREAK ? "休息" : "专注";
}

void smoking_model_enter_radio(smoking_model_t *model)
{
    model->persisted.screen = SMOKING_SCREEN_RADIO;
}
