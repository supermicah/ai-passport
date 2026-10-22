#!/usr/bin/env python3
"""Static power-policy checks for the fork-specific tools app."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main" / "main.c"
MODEL = ROOT / "main" / "smoking_model.c"


def test_idle_sleep_does_not_periodically_wake_by_default() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "#define IDLE_BACKLIGHT_SEC 20u" in source
    assert "#define IDLE_DEEP_SLEEP_SEC (3u * 60u)" in source
    assert "#define SCREEN_OFF_DEEP_SLEEP_SEC (3u * 60u)" in source
    assert "#define IDLE_SLEEP_TIMER_WAKE_SEC 0u" in source
    assert "#if IDLE_SLEEP_TIMER_WAKE_SEC > 0" in source


def test_default_and_smoking_brightness_are_reduced() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "#define BACKLIGHT_NORMAL_PERCENT 15u" in source
    assert "#define BACKLIGHT_RADIO_PERCENT 25u" in source
    assert "#define BACKLIGHT_SMOKING_PERCENT 12u" in source
    assert "#define BACKLIGHT_SMOKING_DIM_PERCENT 2u" in source
    assert "#define BACKLIGHT_ACTIVE_DIM_SEC 10u" in source
    assert "bsp_display_backlight(100)" not in source


def test_network_refreshes_weather_only_when_wifi_is_ready() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "bool weather_network_ready = false;" in source
    tick_body = source.split("static void input_task", 1)[1].split(
        "static void audio_task", 1
    )[0]
    assert "if (tools_wifi_take_time_synced())" in tick_body
    assert "tools_weather_request_refresh();" in tick_body
    assert "bool network_ready = wifi_ready_for_radio(wifi);" in tick_body
    assert "if (network_ready && !weather_network_ready)" in tick_body
    assert "tools_weather_init()" in tick_body
    assert "tools_wifi_reconnect" not in tick_body


def test_radio_exit_stops_wifi_and_active_smoking_can_sleep_safely() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "leave_radio_screen" in source
    leave_body = source.split("static void leave_radio_screen(void)", 1)[1].split(
        "static void request_render", 1
    )[0]
    assert "tools_radio_stop()" in leave_body
    assert "tools_wifi_stop_all()" in leave_body
    deep_sleep_body = source.split("static bool deep_sleep_allowed(void)", 1)[1].split(
        "static void restore_button_after_sleep_abort", 1
    )[0]
    assert "SMOKING_SCREEN_ACTIVE" not in deep_sleep_body
    assert "focus_running" in deep_sleep_body
    assert "wifi.provisioning" in deep_sleep_body


def test_screen_off_enters_deep_sleep_quickly() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "static uint32_t deep_sleep_idle_sec(void)" in source
    helper_body = source.split("static uint32_t deep_sleep_idle_sec(void)", 1)[1].split(
        "static void restore_button_after_sleep_abort", 1
    )[0]
    assert "if (s_manual_screen_off) return SCREEN_OFF_DEEP_SLEEP_SEC;" in helper_body
    assert "if (!s_backlight_on) return IDLE_BACKLIGHT_SEC + SCREEN_OFF_DEEP_SLEEP_SEC;" in helper_body
    assert "return IDLE_DEEP_SLEEP_SEC;" in helper_body
    assert "now - s_last_input_sec >= deep_sleep_idle_sec()" in source
    sleep_body = source.split("static void enter_idle_deep_sleep(void)", 1)[1].split(
        "static bool count_select_ready_for_input", 1
    )[0]
    assert "uint32_t idle_sec = deep_sleep_idle_sec();" in sleep_body
    assert "空闲 %u 秒，进入 deep sleep" in sleep_body


def test_saved_wifi_does_not_reconnect_on_boot_by_default() -> None:
    source = MAIN.read_text(encoding="utf-8")
    app_main_body = source.split("void app_main(void)", 1)[1]
    store_ready_body = app_main_body.split("if (store_partition_err == ESP_OK)", 1)[1].split(
        "s_last_input_sec = now_sec();", 1
    )[0]
    assert "restore_clock_if_available()" in store_ready_body
    assert "tools_wifi_reconnect()" not in store_ready_body
    assert "启动 Wi-Fi 失败" not in store_ready_body


def test_wifi_settings_reconnects_saved_credentials_on_demand() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "static void ensure_wifi_settings_connection(void)" in source
    helper_body = source.split("static void ensure_wifi_settings_connection(void)", 1)[1].split(
        "static void clear_wifi_credentials", 1
    )[0]
    assert "tools_wifi_reconnect()" in helper_body
    assert "tools_wifi_start_provisioning" not in helper_body
    assert "status.state == TOOLS_WIFI_CONNECTING" in helper_body
    process_body = source.split("static void process_input", 1)[1].split(
        "static void input_task", 1
    )[0]
    assert "smoking_model_settings_up(&s_model);" in process_body
    assert "smoking_model_settings_down(&s_model);" in process_body
    assert "ensure_wifi_settings_connection();" in process_body


def test_deep_sleep_clock_uses_boot_rtc_to_avoid_wake_time_overshoot() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "static uint64_t s_boot_rtc_us;" in source
    assert "s_boot_rtc_us = esp_rtc_get_time_us();" in source
    restore_body = source.split("static void restore_clock_if_available(void)", 1)[1].split(
        "static void save_clock_if_due", 1
    )[0]
    assert "uint64_t rtc_now = s_boot_rtc_us ? s_boot_rtc_us : esp_rtc_get_time_us();" in restore_body


def test_long_sleep_triggers_bounded_one_shot_clock_sync() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "#define CLOCK_DEEP_SLEEP_RESYNC_SEC (6u * 60u * 60u)" in source
    assert "#define CLOCK_RESYNC_TIMEOUT_US (90LL * 1000LL * 1000LL)" in source
    assert "static bool s_clock_resync_pending;" in source
    assert "static bool s_clock_resync_wifi_active;" in source

    restore_body = source.split("static void restore_clock_if_available(void)", 1)[1].split(
        "static void finish_clock_resync", 1
    )[0]
    assert "elapsed >= CLOCK_DEEP_SLEEP_RESYNC_SEC" in restore_body
    assert "s_clock_resync_pending = true;" in restore_body

    resync_body = source.split("static void service_clock_resync_if_needed(void)", 1)[1].split(
        "static void save_clock_if_due", 1
    )[0]
    assert "if (!s_clock_resync_pending && !s_clock_resync_wifi_active) return;" in resync_body
    assert "if (status.provisioning) return;" in resync_body
    assert "elapsed_us >= CLOCK_RESYNC_TIMEOUT_US" in resync_body
    assert "finish_clock_resync(true);" in resync_body
    assert "tools_wifi_reconnect()" in resync_body

    tick_body = source.split("static void input_task", 1)[1].split(
        "static void audio_task", 1
    )[0]
    assert "bool was_clock_resync = s_clock_resync_wifi_active;" in tick_body
    assert "if (was_clock_resync)" in tick_body
    assert "finish_clock_resync(true);" in tick_body
    assert "service_clock_resync_if_needed();" in tick_body


def test_wake_zero_battery_is_filtered_temporarily() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "#define BATTERY_WAKE_ZERO_FILTER_SEC 20u" in source
    assert "static RTC_DATA_ATTR int s_deep_sleep_battery_soc = -1;" in source
    assert "static int filtered_battery_soc(void)" in source
    filter_body = source.split("static int filtered_battery_soc(void)", 1)[1].split(
        "static int32_t today_day", 1
    )[0]
    assert "soc == 0 && s_deep_sleep_battery_soc > 0" in filter_body
    assert "now_sec() < s_battery_zero_filter_until_sec" in filter_body
    assert "return s_deep_sleep_battery_soc;" in filter_body
    assert "int soc = filtered_battery_soc();" in source


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
    test_network_refreshes_weather_only_when_wifi_is_ready()
    test_radio_exit_stops_wifi_and_active_smoking_can_sleep_safely()
    test_screen_off_enters_deep_sleep_quickly()
    test_saved_wifi_does_not_reconnect_on_boot_by_default()
    test_wifi_settings_reconnects_saved_credentials_on_demand()
    test_deep_sleep_clock_uses_boot_rtc_to_avoid_wake_time_overshoot()
    test_long_sleep_triggers_bounded_one_shot_clock_sync()
    test_wake_zero_battery_is_filtered_temporarily()
    test_screen_off_does_not_steal_navigation_buttons()
    test_home_ok_long_press_turns_off_screen()
    test_manual_screen_off_ignores_long_press_tail_until_new_press()
    test_wake_click_tail_is_consumed_before_home_ok_action()
    test_active_smoking_session_is_persisted_for_sleep()
    print("Tools app power contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
