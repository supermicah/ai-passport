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
    assert "#define BACKLIGHT_NORMAL_PERCENT 50u" in source
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


def test_radio_exit_stops_wifi_and_active_smoking_can_sleep() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert "leave_radio_screen" in source
    assert 'log_deep_sleep_warning("radio Wi-Fi stop", tools_wifi_stop_all())' in source
    deep_sleep_body = source.split("static bool deep_sleep_allowed(void)", 1)[1].split(
        "static void restore_button_after_sleep_abort", 1
    )[0]
    assert "SMOKING_SCREEN_ACTIVE" not in deep_sleep_body


def test_active_smoking_session_is_persisted_for_sleep() -> None:
    source = MODEL.read_text(encoding="utf-8")
    assert "SMOKING_SLEEP_SESSION_MAGIC" in source
    assert "store_sleep_session(&state, model->smoking_started_at)" in source
    assert "load_sleep_session(&model->persisted, &restored_start)" in source


def main() -> int:
    test_idle_sleep_does_not_periodically_wake_by_default()
    test_default_and_smoking_brightness_are_reduced()
    test_weather_is_not_started_from_idle_tick()
    test_radio_exit_stops_wifi_and_active_smoking_can_sleep()
    test_active_smoking_session_is_persisted_for_sleep()
    print("Tools app power contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
