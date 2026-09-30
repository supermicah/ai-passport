#include "smoking_model.h"

#include <string.h>

#define SECONDS_PER_DAY 86400u

static const uint8_t SUDOKU_SOLUTIONS[SMOKING_SUDOKU_LEVELS][SMOKING_SUDOKU_CELLS] = {
    {
        1, 2, 3, 4, 5,
        2, 3, 4, 5, 1,
        3, 4, 5, 1, 2,
        4, 5, 1, 2, 3,
        5, 1, 2, 3, 4,
    },
    {
        2, 4, 1, 3, 5,
        3, 5, 2, 4, 1,
        4, 1, 3, 5, 2,
        5, 2, 4, 1, 3,
        1, 3, 5, 2, 4,
    },
    {
        5, 3, 1, 4, 2,
        1, 4, 2, 5, 3,
        2, 5, 3, 1, 4,
        3, 1, 4, 2, 5,
        4, 2, 5, 3, 1,
    },
};

static const uint8_t SUDOKU_GIVEN[SMOKING_SUDOKU_LEVELS][SMOKING_SUDOKU_CELLS] = {
    {
        1, 0, 3, 0, 5,
        0, 3, 0, 5, 0,
        3, 0, 5, 0, 2,
        0, 5, 0, 2, 0,
        5, 0, 2, 0, 4,
    },
    {
        2, 0, 0, 3, 5,
        0, 5, 2, 0, 0,
        4, 0, 3, 0, 2,
        0, 2, 0, 1, 0,
        1, 0, 5, 0, 4,
    },
    {
        5, 0, 1, 0, 2,
        0, 4, 0, 5, 0,
        2, 0, 3, 0, 4,
        0, 1, 0, 2, 0,
        4, 0, 5, 0, 1,
    },
};

static int32_t day_from_sec(uint32_t now_sec)
{
    return (int32_t)(now_sec / SECONDS_PER_DAY);
}

static uint32_t sec_of_day(uint32_t now_sec)
{
    return now_sec % SECONDS_PER_DAY;
}

static bool valid_screen(uint8_t screen)
{
    return screen <= SMOKING_SCREEN_SETTINGS;
}

static void reset_sudoku_to_level(smoking_sudoku_state_t *sudoku, uint8_t level)
{
    memset(sudoku, 0, sizeof(*sudoku));
    sudoku->level = level % SMOKING_SUDOKU_LEVELS;
    sudoku->mode = SMOKING_SUDOKU_SELECT_CELL;
    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        sudoku->values[i] = SUDOKU_GIVEN[sudoku->level][i];
    }
}

void smoking_model_init(smoking_model_t *model)
{
    memset(model, 0, sizeof(*model));
    model->persisted.version = SMOKING_MODEL_VERSION;
    model->persisted.screen = SMOKING_SCREEN_HOME;
    model->persisted.selected_feature = SMOKING_FEATURE_SMOKE;
    model->pending_cigarettes = 1;
    reset_sudoku_to_level(&model->persisted.sudoku, 0);
}

void smoking_model_restore(smoking_model_t *model, const smoking_persist_state_t *state)
{
    smoking_model_init(model);
    if (!state || state->version != SMOKING_MODEL_VERSION) return;

    model->persisted = *state;
    if (!valid_screen(model->persisted.screen) ||
        model->persisted.screen == SMOKING_SCREEN_ACTIVE ||
        model->persisted.screen == SMOKING_SCREEN_COUNT_SELECT) {
        model->persisted.screen = SMOKING_SCREEN_HOME;
    }
    if (model->persisted.selected_feature >= SMOKING_FEATURE_COUNT) {
        model->persisted.selected_feature = SMOKING_FEATURE_SMOKE;
    }
    for (size_t i = 0; i < SMOKING_HISTORY_DAYS; i++) {
        if (model->persisted.days[i].cigarettes > 200 ||
            model->persisted.days[i].duration_sec > SECONDS_PER_DAY) {
            memset(&model->persisted.days[i], 0, sizeof(model->persisted.days[i]));
        }
    }
    if (model->persisted.last.cigarettes > 2 ||
        model->persisted.last.duration_sec > SECONDS_PER_DAY) {
        memset(&model->persisted.last, 0, sizeof(model->persisted.last));
    }
    if (model->persisted.sudoku.level >= SMOKING_SUDOKU_LEVELS ||
        model->persisted.sudoku.selected_empty >= SMOKING_SUDOKU_CELLS ||
        model->persisted.sudoku.mode > SMOKING_SUDOKU_SELECT_CELL) {
        reset_sudoku_to_level(&model->persisted.sudoku, 0);
    }
    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        if (model->persisted.sudoku.values[i] > SMOKING_SUDOKU_SIZE) {
            reset_sudoku_to_level(&model->persisted.sudoku, model->persisted.sudoku.level);
            break;
        }
        if (SUDOKU_GIVEN[model->persisted.sudoku.level][i]) {
            model->persisted.sudoku.values[i] =
                SUDOKU_GIVEN[model->persisted.sudoku.level][i];
        }
    }
    model->smoking_started_at = 0;
    model->pending_duration_sec = 0;
    model->pending_cigarettes = 1;
    model->dirty = false;
}

smoking_persist_state_t smoking_model_persist_snapshot(const smoking_model_t *model)
{
    smoking_persist_state_t state = model->persisted;
    state.version = SMOKING_MODEL_VERSION;
    state.screen = SMOKING_SCREEN_HOME;
    if (state.selected_feature >= SMOKING_FEATURE_COUNT) {
        state.selected_feature = SMOKING_FEATURE_SMOKE;
    }
    return state;
}

const char *smoking_feature_name(smoking_feature_t feature)
{
    static const char *NAMES[] = {
        "抽烟", "数独", "设置",
    };
    return feature < SMOKING_FEATURE_COUNT ? NAMES[feature] : "";
}

smoking_feature_t smoking_model_selected_feature(const smoking_model_t *model)
{
    return (smoking_feature_t)model->persisted.selected_feature;
}

void smoking_model_home_next(smoking_model_t *model)
{
    model->persisted.selected_feature =
        (uint8_t)((model->persisted.selected_feature + 1u) % SMOKING_FEATURE_COUNT);
}

void smoking_model_home_prev(smoking_model_t *model)
{
    model->persisted.selected_feature =
        (uint8_t)((model->persisted.selected_feature + SMOKING_FEATURE_COUNT - 1u) %
                  SMOKING_FEATURE_COUNT);
}

void smoking_model_enter_selected(smoking_model_t *model, uint32_t now_sec)
{
    switch ((smoking_feature_t)model->persisted.selected_feature) {
    case SMOKING_FEATURE_SMOKE:
        smoking_model_start(model, now_sec);
        break;
    case SMOKING_FEATURE_SUDOKU:
        model->persisted.screen = SMOKING_SCREEN_SUDOKU;
        break;
    case SMOKING_FEATURE_SETTINGS:
        smoking_model_enter_time_settings(model, now_sec);
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
    model->selected_setting = SMOKING_SETTING_TIME;
    model->editing_time = false;
}

void smoking_model_return_home(smoking_model_t *model)
{
    model->persisted.screen = SMOKING_SCREEN_HOME;
    model->smoking_started_at = 0;
    model->pending_duration_sec = 0;
    model->pending_cigarettes = 1;
    model->editing_time = false;
}

void smoking_model_start(smoking_model_t *model, uint32_t now_sec)
{
    model->persisted.screen = SMOKING_SCREEN_ACTIVE;
    model->smoking_started_at = now_sec;
    model->pending_duration_sec = 0;
    model->pending_cigarettes = 1;
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
}

void smoking_model_count_up(smoking_model_t *model)
{
    if (model->pending_cigarettes < 2) model->pending_cigarettes++;
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
    if (cigarettes > 2) cigarettes = 2;

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

static size_t empty_count(uint8_t level)
{
    size_t count = 0;
    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        if (!SUDOKU_GIVEN[level][i]) count++;
    }
    return count;
}

static size_t empty_cell_at(uint8_t level, uint8_t empty_index)
{
    size_t found = 0;
    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        if (SUDOKU_GIVEN[level][i]) continue;
        if (found == empty_index) return i;
        found++;
    }
    return 0;
}

void smoking_model_sudoku_reset(smoking_model_t *model)
{
    reset_sudoku_to_level(&model->persisted.sudoku, model->persisted.sudoku.level);
}

void smoking_model_sudoku_click(smoking_model_t *model)
{
    smoking_sudoku_state_t *sudoku = &model->persisted.sudoku;
    size_t cell = empty_cell_at(sudoku->level, sudoku->selected_empty);
    if (sudoku->values[cell] == 0) {
        sudoku->values[cell] = 1;
        sudoku->mode = SMOKING_SUDOKU_EDIT_DIGIT;
    } else {
        sudoku->mode = sudoku->mode == SMOKING_SUDOKU_EDIT_DIGIT ?
            SMOKING_SUDOKU_SELECT_CELL : SMOKING_SUDOKU_EDIT_DIGIT;
    }
    model->dirty = true;
}

void smoking_model_sudoku_up(smoking_model_t *model)
{
    smoking_sudoku_state_t *sudoku = &model->persisted.sudoku;
    size_t count = empty_count(sudoku->level);
    if (sudoku->mode == SMOKING_SUDOKU_SELECT_CELL) {
        sudoku->selected_empty = (uint8_t)((sudoku->selected_empty + count - 1u) % count);
    } else {
        size_t cell = empty_cell_at(sudoku->level, sudoku->selected_empty);
        uint8_t value = sudoku->values[cell] ? sudoku->values[cell] : 1;
        sudoku->values[cell] = value == 1 ? SMOKING_SUDOKU_SIZE : (uint8_t)(value - 1u);
    }
    model->dirty = true;
}

void smoking_model_sudoku_down(smoking_model_t *model)
{
    smoking_sudoku_state_t *sudoku = &model->persisted.sudoku;
    size_t count = empty_count(sudoku->level);
    if (sudoku->mode == SMOKING_SUDOKU_SELECT_CELL) {
        sudoku->selected_empty = (uint8_t)((sudoku->selected_empty + 1u) % count);
    } else {
        size_t cell = empty_cell_at(sudoku->level, sudoku->selected_empty);
        uint8_t value = sudoku->values[cell] ? sudoku->values[cell] : 0;
        sudoku->values[cell] = (uint8_t)((value % SMOKING_SUDOKU_SIZE) + 1u);
    }
    model->dirty = true;
    if (smoking_model_sudoku_complete(model)) {
        reset_sudoku_to_level(&model->persisted.sudoku,
                              (uint8_t)(sudoku->level + 1u));
    }
}

uint8_t smoking_model_sudoku_cell_value(const smoking_model_t *model, size_t cell)
{
    if (cell >= SMOKING_SUDOKU_CELLS) return 0;
    return model->persisted.sudoku.values[cell];
}

bool smoking_model_sudoku_cell_fixed(const smoking_model_t *model, size_t cell)
{
    if (cell >= SMOKING_SUDOKU_CELLS) return false;
    return SUDOKU_GIVEN[model->persisted.sudoku.level][cell] != 0;
}

bool smoking_model_sudoku_cell_error(const smoking_model_t *model, size_t cell)
{
    if (cell >= SMOKING_SUDOKU_CELLS) return false;
    uint8_t value = model->persisted.sudoku.values[cell];
    return value != 0 && value != SUDOKU_SOLUTIONS[model->persisted.sudoku.level][cell];
}

bool smoking_model_sudoku_complete(const smoking_model_t *model)
{
    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        if (model->persisted.sudoku.values[i] !=
            SUDOKU_SOLUTIONS[model->persisted.sudoku.level][i]) {
            return false;
        }
    }
    return true;
}

bool smoking_model_idle_backlight_allowed(const smoking_model_t *model)
{
    switch ((smoking_screen_t)model->persisted.screen) {
    case SMOKING_SCREEN_HOME:
    case SMOKING_SCREEN_COUNT_SELECT:
    case SMOKING_SCREEN_SUDOKU:
    case SMOKING_SCREEN_SETTINGS:
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
    if (model->selected_setting == SMOKING_SETTING_TIME) {
        model->editing_time = true;
        model->edit_minute_selected = false;
    }
}
