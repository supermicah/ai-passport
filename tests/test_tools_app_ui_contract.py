#!/usr/bin/env python3
"""UI contract checks for the fork-specific tools app."""

from __future__ import annotations

import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_PATH = ROOT / "assets/fonts/smoking_font_18.c"
MAIN_DIR = ROOT / "main"
LINE_HEIGHT = 21
SCREEN_W = 240
SCREEN_H = 320


def c_string_literals(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="ignore")
    values: list[str] = []
    for match in re.finditer(r'"(?:[^"\\]|\\.)*"', text):
        try:
            values.append(ast.literal_eval(match.group(0)))
        except (SyntaxError, ValueError):
            continue
    return values


def chinese_literals() -> list[tuple[Path, str]]:
    values: list[tuple[Path, str]] = []
    for pattern in ("*.c", "*.h"):
        for path in sorted(MAIN_DIR.glob(pattern)):
            for value in c_string_literals(path):
                if any("\u4e00" <= ch <= "\u9fff" for ch in value):
                    values.append((path, value))
    return values


def font_symbols() -> set[str]:
    text = FONT_PATH.read_text(encoding="utf-8", errors="ignore")
    match = re.search(r"--symbols (.*?) --size", text, re.S)
    assert match, "generated tools-app font must record its --symbols inventory"
    return set(match.group(1))


def test_all_fixed_chinese_text_has_generated_glyphs() -> None:
    symbols = font_symbols()
    missing: list[str] = []
    for path, value in chinese_literals():
        missing_chars = sorted(
            {ch for ch in value if "\u4e00" <= ch <= "\u9fff" and ch not in symbols}
        )
        if missing_chars:
            missing.append(f"{path.relative_to(ROOT)}: {value!r} missing {''.join(missing_chars)}")
    assert not missing, "\n".join(missing)


def test_home_feature_order_keeps_settings_last() -> None:
    text = (MAIN_DIR / "main.c").read_text(encoding="utf-8")
    match = re.search(r"feature_order\[\]\s*=\s*\{(?P<body>.*?)\};", text, re.S)
    assert match, "home feature_order[] must be explicit"
    items = re.findall(r"SMOKING_FEATURE_[A-Z_]+", match.group("body"))
    assert items, "home feature_order[] must not be empty"
    assert items[-1] == "SMOKING_FEATURE_SETTINGS"


def boxes_do_not_overlap(
    boxes: list[tuple[str, int, int, int, int]],
    allowed_pairs: set[tuple[str, str]],
) -> list[str]:
    problems: list[str] = []
    for index, box in enumerate(boxes):
        name, x, y, w, h = box
        if x < 0 or y < 0 or x + w > SCREEN_W or y + h > SCREEN_H:
            problems.append(f"{name} outside {SCREEN_W}x{SCREEN_H}")
        for other in boxes[index + 1 :]:
            other_name, ox, oy, ow, oh = other
            disjoint = x + w <= ox or ox + ow <= x or y + h <= oy or oy + oh <= y
            if disjoint:
                continue
            pair = tuple(sorted((name, other_name)))
            if pair not in allowed_pairs:
                problems.append(f"{name} overlaps {other_name}")
    return problems


def child_pairs(boxes: list[tuple[str, int, int, int, int]]) -> set[tuple[str, str]]:
    allowed: set[tuple[str, str]] = set()
    for parent in boxes:
        parent_name, x, y, w, h = parent
        is_container = (
            parent_name.endswith("_panel")
            or parent_name.startswith("row_")
            or (parent_name.startswith("feature") and not parent_name.endswith("_text"))
        )
        if not is_container:
            continue
        for child in boxes:
            child_name, cx, cy, cw, ch = child
            if child_name == parent_name:
                continue
            inside = cx >= x and cy >= y and cx + cw <= x + w and cy + ch <= y + h
            if inside:
                allowed.add(tuple(sorted((parent_name, child_name))))
    return allowed


def test_home_and_settings_static_layout_do_not_overlap() -> None:
    home = [
        ("mark_v", 14, 35, 4, 18),
        ("mark_h", 22, 42, 28, 3),
        ("title", 56, 30, 112, LINE_HEIGHT),
        ("dirty", 168, 32, 58, LINE_HEIGHT),
        ("weather", 14, 58, 212, LINE_HEIGHT),
        ("wifi", 14, 80, 212, LINE_HEIGHT),
        ("stats_panel", 12, 103, 216, 88),
        ("today", 24, 109, 188, LINE_HEIGHT),
        ("streak", 24, 130, 188, LINE_HEIGHT),
        ("progress_track", 24, 151, 68, 8),
        ("progress_fill", 24, 151, 68, 8),
        ("interval", 24, 166, 188, LINE_HEIGHT),
        ("feature0", 12, 196, 216, 23),
        ("feature0_mark", 18, 201, 3, 13),
        ("feature0_index", 28, 197, 66, LINE_HEIGHT),
        ("feature0_text", 100, 197, 86, LINE_HEIGHT),
        ("feature0_arrow", 196, 197, 24, LINE_HEIGHT),
        ("feature1", 12, 220, 216, 23),
        ("feature1_mark", 18, 225, 3, 13),
        ("feature1_index", 28, 221, 66, LINE_HEIGHT),
        ("feature1_text", 100, 221, 86, LINE_HEIGHT),
        ("feature1_arrow", 196, 221, 24, LINE_HEIGHT),
        ("feature2", 12, 244, 216, 23),
        ("feature2_mark", 18, 249, 3, 13),
        ("feature2_index", 28, 245, 66, LINE_HEIGHT),
        ("feature2_text", 100, 245, 86, LINE_HEIGHT),
        ("feature2_arrow", 196, 245, 24, LINE_HEIGHT),
        ("feature3", 12, 268, 216, 23),
        ("feature3_mark", 18, 273, 3, 13),
        ("feature3_index", 28, 269, 66, LINE_HEIGHT),
        ("feature3_text", 100, 269, 86, LINE_HEIGHT),
        ("feature3_arrow", 196, 269, 24, LINE_HEIGHT),
        ("footer", 14, 294, 212, LINE_HEIGHT),
        ("footer_left", 14, 294, 120, LINE_HEIGHT),
        ("footer_right", 132, 294, 94, LINE_HEIGHT),
    ]
    settings = [
        ("mark_v", 14, 35, 4, 18),
        ("mark_h", 22, 42, 28, 3),
        ("title", 56, 31, 80, LINE_HEIGHT),
        ("row_time", 14, 56, 212, 48),
        ("time_mark", 20, 66, 3, 28),
        ("time_title", 28, 61, 70, LINE_HEIGHT),
        ("time_value", 120, 61, 90, LINE_HEIGHT),
        ("time_hint", 28, 82, 182, LINE_HEIGHT),
        ("row_goal", 14, 106, 212, 48),
        ("goal_mark", 20, 116, 3, 28),
        ("goal_title", 28, 111, 70, LINE_HEIGHT),
        ("goal_value", 120, 111, 90, LINE_HEIGHT),
        ("goal_hint", 28, 132, 182, LINE_HEIGHT),
        ("row_today", 14, 156, 212, 48),
        ("today_mark", 20, 166, 3, 28),
        ("today_title", 28, 161, 70, LINE_HEIGHT),
        ("today_value", 120, 161, 90, LINE_HEIGHT),
        ("today_hint", 28, 182, 182, LINE_HEIGHT),
        ("row_wifi", 14, 206, 212, 48),
        ("wifi_mark", 20, 216, 3, 28),
        ("wifi_title", 28, 211, 70, LINE_HEIGHT),
        ("wifi_value", 120, 211, 90, LINE_HEIGHT),
        ("wifi_hint", 28, 232, 182, LINE_HEIGHT),
        ("help1", 20, 264, 200, LINE_HEIGHT),
        ("help2", 20, 288, 200, LINE_HEIGHT),
    ]
    allowed = child_pairs(home) | child_pairs(settings)
    allowed.update(
        {
            tuple(sorted(pair))
            for pair in (
                ("footer", "footer_left"),
                ("footer", "footer_right"),
                ("footer_left", "footer_right"),
                ("progress_track", "progress_fill"),
            )
        }
    )
    problems = boxes_do_not_overlap(home, allowed)
    problems.extend(boxes_do_not_overlap(settings, allowed))
    assert not problems, "\n".join(problems)


def test_battery_uses_icon_without_percent_text() -> None:
    text = (MAIN_DIR / "main.c").read_text(encoding="utf-8")
    body = text.split("static void render_battery_pill", 1)[1].split(
        "static void render_setting_row", 1
    )[0]
    assert "pill(root, x, y, 36, 18" in body
    assert "panel(root, x + 35, y + 5, 3, 8, accent, 0)" in body
    assert '"%d%%"' not in body
    assert "label_one_line" not in body


def test_wifi_setup_uses_softap_web_form_not_wechat_or_blufi() -> None:
    text = (MAIN_DIR / "main.c").read_text(encoding="utf-8")
    wifi = (MAIN_DIR / "tools_wifi.c").read_text(encoding="utf-8")
    header = (MAIN_DIR / "tools_wifi.h").read_text(encoding="utf-8")
    cmake = (MAIN_DIR / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "OK 开AP" in text
    assert "TOOLS_WIFI_SETUP_SSID" in text
    assert "http://192.168.4.1" in header
    assert "AI-Passport-Setup" in header
    assert "esp_netif_create_default_wifi_ap" in wifi
    assert "httpd_start" in wifi
    assert "WIFI_MODE_APSTA" in wifi
    assert "save_credentials_to_tools_data" in wifi
    assert "Nearby Wi-Fi" in wifi
    assert "<select id=\\\"ssid\\\" name=\\\"ssid\\\">" in wifi
    assert "custom_ssid" in wifi
    assert "esp_wifi_scan_start" in wifi
    assert "esp_wifi_scan_get_ap_records" in wifi
    assert "s_save_on_connect" in wifi
    assert "tools_wifi_security.c" not in cmake
    assert "微信" not in text
    assert "小程序" not in text
    assert "BLUFI" not in wifi


def main() -> int:
    test_all_fixed_chinese_text_has_generated_glyphs()
    test_home_feature_order_keeps_settings_last()
    test_home_and_settings_static_layout_do_not_overlap()
    test_battery_uses_icon_without_percent_text()
    test_wifi_setup_uses_softap_web_form_not_wechat_or_blufi()
    print("Tools app UI contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
