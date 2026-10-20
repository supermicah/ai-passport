#!/usr/bin/env python3
"""Contract checks for the Poke7 page-turner branch."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


def test_app_starts_page_turner_before_legacy_tools_app() -> None:
    text = (MAIN / "main.c").read_text(encoding="utf-8")
    entry = text.split("void app_main(void)", 1)[1]
    first_body = entry.split("s_boot_rtc_us", 1)[0]
    assert "page_turner_app_main();" in first_body
    assert "return;" in first_body


def test_ble_hid_page_turner_exposes_keyboard_and_consumer_reports() -> None:
    text = (MAIN / "page_turner.c").read_text(encoding="utf-8")
    assert '#define DEVICE_NAME "PassportTurner"' in text
    assert "ESP_HID_TRANSPORT_BLE" in text
    assert "0x09, 0x06" in text  # Keyboard usage.
    assert "0x05, 0x0C" in text  # Consumer usage page.
    assert "0x85, 0x01" in text
    assert "0x85, 0x02" in text
    assert "send_keyboard_usage(next ? 0x4E : 0x4B)" in text
    assert "send_keyboard_usage(next ? 0x4F : 0x50)" in text
    assert "send_consumer_usage(next ? 0x00EA : 0x00E9)" in text


def test_nimble_hid_service_is_enabled_for_ble_backend() -> None:
    text = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    assert "CONFIG_BT_NIMBLE_ENABLED=y" in text
    assert "CONFIG_BT_NIMBLE_HID_SERVICE=y" in text


def test_nimble_enable_uses_real_host_task_not_init_adapter() -> None:
    text = (MAIN / "page_turner.c").read_text(encoding="utf-8")
    assert "static void page_turner_ble_host_task(void *param)" in text
    assert "nimble_port_run();" in text
    assert "esp_nimble_enable(page_turner_ble_host_task)" in text
    assert "esp_nimble_enable(nimble_port_freertos_init)" not in text


def test_page_keys_use_press_event_for_low_latency_turning() -> None:
    text = (MAIN / "page_turner.c").read_text(encoding="utf-8")
    assert "input->event == BSP_BTN_PRESS && input->btn == BSP_BTN_UP" in text
    assert "input->event == BSP_BTN_PRESS && input->btn == BSP_BTN_DOWN" in text
    assert "input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK" in text
    assert "ESP_LOGI(TAG, \"button btn=%d event=%d mv=%d connected=%d mode=%s\"" in text
    assert "if (input->event != BSP_BTN_CLICK) return;" not in text


def test_volume_mode_is_default_for_ereader_apps() -> None:
    text = (MAIN / "page_turner.c").read_text(encoding="utf-8")
    assert "static turner_mode_t s_mode = TURNER_MODE_VOLUME;" in text
    assert "uint8_t mode = TURNER_MODE_VOLUME;" in text
    assert 'return "VOL UP / DOWN";' in text
    assert 'mode == TURNER_MODE_VOLUME ? "VOL UP"' in text
    assert 'mode == TURNER_MODE_VOLUME ? "VOL DOWN"' in text


def test_page_turner_uses_low_power_screen_and_report_cadence() -> None:
    text = (MAIN / "page_turner.c").read_text(encoding="utf-8")
    assert "#define BACKLIGHT_ACTIVE_PERCENT 12" in text
    assert "#define BACKLIGHT_DIM_PERCENT 3" in text
    assert "#define SCREEN_OFF_IDLE_MS (25 * 1000)" in text
    assert "#define UI_BATTERY_REFRESH_MS (60 * 1000)" in text
    assert "#define HID_BATTERY_REFRESH_MS (5 * 60 * 1000)" in text
    assert "#define HID_KEY_HOLD_MS 80" in text
    assert "set_screen_backlight(0);" in text
    assert "bsp_display_backlight(35)" not in text
    assert "pdMS_TO_TICKS(1000))" not in text


def test_pairing_advertising_is_not_high_duty_fast_interval() -> None:
    text = (MAIN / "page_turner.c").read_text(encoding="utf-8")
    assert "BLE_GAP_ADV_ITVL_MS(250)" in text
    assert "BLE_GAP_ADV_ITVL_MS(400)" in text
    assert "BLE_GAP_ADV_ITVL_MS(30)" not in text
    assert "BLE_GAP_ADV_ITVL_MS(50)" not in text


def test_ui_uses_ascii_to_avoid_font_subset_dependency() -> None:
    text = (MAIN / "page_turner.c").read_text(encoding="utf-8")
    non_ascii = sorted({ch for ch in text if ord(ch) > 127})
    assert not non_ascii, f"page_turner.c should stay ASCII-only, found {non_ascii!r}"


if __name__ == "__main__":
    test_app_starts_page_turner_before_legacy_tools_app()
    test_ble_hid_page_turner_exposes_keyboard_and_consumer_reports()
    test_nimble_hid_service_is_enabled_for_ble_backend()
    test_nimble_enable_uses_real_host_task_not_init_adapter()
    test_page_keys_use_press_event_for_low_latency_turning()
    test_volume_mode_is_default_for_ereader_apps()
    test_page_turner_uses_low_power_screen_and_report_cadence()
    test_pairing_advertising_is_not_high_duty_fast_interval()
    test_ui_uses_ascii_to_avoid_font_subset_dependency()
    print("Page turner contract: PASS")
