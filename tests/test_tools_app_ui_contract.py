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
        ("title", 12, 32, 120, LINE_HEIGHT),
        ("dirty", 170, 32, 58, LINE_HEIGHT),
        ("weather", 14, 58, 212, LINE_HEIGHT),
        ("wifi", 14, 80, 212, LINE_HEIGHT),
        ("stats_panel", 14, 104, 212, 72),
        ("today", 26, 110, 188, LINE_HEIGHT),
        ("streak", 26, 132, 188, LINE_HEIGHT),
        ("interval", 26, 154, 188, LINE_HEIGHT),
        ("feature0", 14, 180, 212, 24),
        ("feature0_text", 24, 180, 180, LINE_HEIGHT),
        ("feature1", 14, 208, 212, 24),
        ("feature1_text", 24, 208, 180, LINE_HEIGHT),
        ("feature2", 14, 236, 212, 24),
        ("feature2_text", 24, 236, 180, LINE_HEIGHT),
        ("feature3", 14, 264, 212, 24),
        ("feature3_text", 24, 264, 180, LINE_HEIGHT),
        ("footer", 14, 292, 212, LINE_HEIGHT),
        ("footer_left", 14, 292, 120, LINE_HEIGHT),
        ("footer_right", 132, 292, 94, LINE_HEIGHT),
    ]
    settings = [
        ("title", 14, 38, 80, LINE_HEIGHT),
        ("row_time", 14, 68, 212, 58),
        ("time_title", 28, 74, 70, LINE_HEIGHT),
        ("time_value", 120, 74, 90, LINE_HEIGHT),
        ("time_hint", 28, 100, 182, LINE_HEIGHT),
        ("row_goal", 14, 132, 212, 58),
        ("goal_title", 28, 138, 70, LINE_HEIGHT),
        ("goal_value", 120, 138, 90, LINE_HEIGHT),
        ("goal_hint", 28, 164, 182, LINE_HEIGHT),
        ("row_wifi", 14, 196, 212, 58),
        ("wifi_title", 28, 202, 70, LINE_HEIGHT),
        ("wifi_value", 120, 202, 90, LINE_HEIGHT),
        ("wifi_hint", 28, 228, 182, LINE_HEIGHT),
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
            )
        }
    )
    problems = boxes_do_not_overlap(home, allowed)
    problems.extend(boxes_do_not_overlap(settings, allowed))
    assert not problems, "\n".join(problems)


def main() -> int:
    test_all_fixed_chinese_text_has_generated_glyphs()
    test_home_feature_order_keeps_settings_last()
    test_home_and_settings_static_layout_do_not_overlap()
    print("Tools app UI contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
