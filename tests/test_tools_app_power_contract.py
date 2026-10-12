#!/usr/bin/env python3
"""Static power-policy checks for the fork-specific tools app."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main" / "main.c"
MODEL = ROOT / "main" / "smoking_model.c"


def test_idle_sleep_does_not_periodically_wake_by_default() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "#define IDLE_SLEEP_TIMER_WAKE_SEC 0u" in source
    assert "#if IDLE_SLEEP_TIMER_WAKE_SEC > 0" in source


def test_default_and_smoking_brightness_are_reduced() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "#define BACKLIGHT_NORMAL_PERCENT 35u" in source
    assert "#define BACKLIGHT_SMOKING_PERCENT 25u" in source
    assert "#define BACKLIGHT_SMOKING_DIM_PERCENT 8u" in source
    assert "bsp_display_backlight(100)" not in source


def test_weather_is_not_started_from_idle_tick() -> None:
    source = MAIN.read_text(encoding="utf-8")
    tick_body = source.split("if (tools_wifi_take_time_synced())", 1)[1].split(
        "tools_wifi_status_t wifi = tools_wifi_status();", 1
    )[0]
    assert "tools_weather_init" not in tick_body
    assert "tools_weather_request_refresh" not in tick_body


def test_radio_exit_stops_wifi_and_active_smoking_stays_awake() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "leave_radio_screen" in source
    assert 'log_deep_sleep_warning("radio Wi-Fi stop", tools_wifi_stop_all())' in source
    deep_sleep_body = source.split("static bool deep_sleep_allowed(void)", 1)[1].split(
        "static void restore_button_after_sleep_abort", 1
    )[0]
    assert "SMOKING_SCREEN_ACTIVE" in deep_sleep_body


def test_screen_off_does_not_steal_navigation_buttons() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "SCREEN_OFF_BUTTON BSP_BTN_UP" not in source
    assert "SCREEN_OFF_BUTTON BSP_BTN_DOWN" not in source
    process_body = source.split("static void process_input", 1)[1].split(
        "static void input_task", 1
    )[0]
    home_body = process_body.split("if (screen == SMOKING_SCREEN_HOME)", 1)[1].split(
        "} else if (screen == SMOKING_SCREEN_ACTIVE)", 1
    )[0]
    assert "input->btn == BSP_BTN_UP" in home_body
    assert "smoking_model_home_prev(&s_model)" in home_body
    assert "input->btn == BSP_BTN_DOWN" in home_body
    assert "smoking_model_home_next(&s_model)" in home_body


def test_home_ok_long_press_turns_off_screen() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "static bool s_manual_screen_off;" in source
    process_body = source.split("static void process_input", 1)[1].split(
        "static void input_task", 1
    )[0]
    home_body = process_body.split("if (screen == SMOKING_SCREEN_HOME)", 1)[1].split(
        "} else if (screen == SMOKING_SCREEN_ACTIVE)", 1
    )[0]
    long_branch = home_body.split("input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK", 1)[1].split(
        "} else if", 1
    )[0]
    assert "s_manual_screen_off = true" in long_branch
    assert "set_backlight_percent(0)" in long_branch
    assert "request_render()" in long_branch
    assert "smoking_model_enter_time_settings" not in long_branch


def test_manual_screen_off_ignores_long_press_tail_until_new_press() -> None:
    source = MAIN.read_text(encoding="utf-8")
    active_body = source.split("static void apply_active_backlight", 1)[1].split(
        "static void set_common_text", 1
    )[0]
    assert "s_manual_screen_off" in active_body
    assert "if (!s_backlight_on || s_manual_screen_off) return;" in active_body

    process_body = source.split("static void process_input", 1)[1].split(
        "static void input_task", 1
    )[0]
    off_body = process_body.split("if (!s_backlight_on)", 1)[1].split(
        "if (s_ignore_wake_tail)", 1
    )[0]
    assert "s_manual_screen_off && input->event != BSP_BTN_PRESS" in off_body
    assert "return;" in off_body
    assert "s_manual_screen_off = false" in off_body
    assert "set_backlight_percent(desired_backlight_percent(now))" in off_body


def test_wake_click_tail_is_consumed_before_home_ok_action() -> None:
    source = MAIN.read_text(encoding="utf-8")
    process_body = source.split("static void process_input", 1)[1].split(
        "smoking_screen_t screen", 1
    )[0]
    ignore_body = process_body.split("if (s_ignore_wake_tail)", 1)[1]
    assert "input->event == BSP_BTN_CLICK" in ignore_body
    assert "input->event == BSP_BTN_DOUBLE" in ignore_body
    assert "input->event == BSP_BTN_LONG" in ignore_body
    assert """s_ignore_wake_tail = false;
            s_last_input_sec = now;
            request_render();
            return;""" in ignore_body
    assert """s_last_input_sec = now;
        request_render();
        return;""" in ignore_body


def test_active_smoking_session_is_persisted_for_sleep() -> None:
    source = MODEL.read_text(encoding="utf-8")
    assert "SMOKING_SLEEP_SESSION_MAGIC" in source
    assert "store_sleep_session(&state, model->smoking_started_at)" in source
    assert "load_sleep_session(&model->persisted, &restored_start)" in source


def main() -> int:
    test_idle_sleep_does_not_periodically_wake_by_default()
    test_default_and_smoking_brightness_are_reduced()
    test_weather_is_not_started_from_idle_tick()
    test_radio_exit_stops_wifi_and_active_smoking_stays_awake()
    test_screen_off_does_not_steal_navigation_buttons()
    test_home_ok_long_press_turns_off_screen()
    test_manual_screen_off_ignores_long_press_tail_until_new_press()
    test_wake_click_tail_is_consumed_before_home_ok_action()
    test_active_smoking_session_is_persisted_for_sleep()
    print("Tools app power contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
