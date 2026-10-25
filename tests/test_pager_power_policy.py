"""Static contracts for the pager idle power policy."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/main.c").read_text()


def switch_case_body(source: str, case_name: str) -> str:
    match = re.search(rf"case {case_name}:\n(?P<body>.*?)(?=\n    case |\n    }})", source, re.S)
    if not match:
        raise AssertionError(f"case {case_name} not found")
    return match.group("body")


class PagerPowerPolicyTest(unittest.TestCase):
    def test_page_keys_do_not_extend_shutdown_or_light_screen(self) -> None:
        for case_name in ("EV_PREV", "EV_NEXT"):
            body = switch_case_body(SOURCE, case_name)
            self.assertNotIn("mark_button_activity", body)
            self.assertNotIn("brighten_for_mode_action", body)
            self.assertIn("send_control", body)

    def test_mode_keys_light_screen_and_extend_shutdown(self) -> None:
        for case_name in ("EV_MODE", "EV_FORGET_ASK"):
            body = switch_case_body(SOURCE, case_name)
            self.assertIn("brighten_for_mode_action", body)

        helper = re.search(
            r"static void brighten_for_mode_action\(void\)\n\{(?P<body>.*?)\n\}",
            SOURCE,
            re.S,
        )
        self.assertIsNotNone(helper)
        self.assertIn("mark_button_activity();", helper.group("body"))
        self.assertIn("set_backlight(BACKLIGHT_BRIGHT_PERCENT);", helper.group("body"))

    def test_transient_battery_zero_is_filtered_by_voltage(self) -> None:
        self.assertIn("#define BATTERY_FALSE_ZERO_MIN_MV 3300", SOURCE)
        helper = re.search(
            r"static bool battery_soc_is_false_zero\(int soc, int mv\)\n\{(?P<body>.*?)\n\}",
            SOURCE,
            re.S,
        )
        self.assertIsNotNone(helper)
        self.assertIn("soc == 0", helper.group("body"))
        self.assertIn("mv >= BATTERY_FALSE_ZERO_MIN_MV", helper.group("body"))
        self.assertIn("Battery ignored transient 0%%", SOURCE)


if __name__ == "__main__":
    unittest.main()
