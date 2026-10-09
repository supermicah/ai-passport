#!/usr/bin/env python3
"""Static checks for tools-app data that must survive normal full-image flashes."""

from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PARTITIONS = ROOT / "partitions.csv"
STORE = ROOT / "main" / "smoking_store.c"
WIFI = ROOT / "main" / "tools_wifi.c"
RADIO = ROOT / "main" / "tools_radio.c"


def parse_size(value: str) -> int:
    return int(value.strip(), 0)


def load_partitions() -> dict[str, dict[str, str]]:
    rows: dict[str, dict[str, str]] = {}
    with PARTITIONS.open(encoding="utf-8", newline="") as handle:
        for raw in csv.reader(line for line in handle if not line.lstrip().startswith("#")):
            if not raw or not raw[0].strip():
                continue
            name, kind, subtype, offset, size, *rest = [item.strip() for item in raw]
            rows[name] = {
                "kind": kind,
                "subtype": subtype,
                "offset": offset,
                "size": size,
            }
    return rows


def test_tools_data_partition_is_reserved_after_factory_app() -> None:
    partitions = load_partitions()
    assert "factory" in partitions
    assert "tools_data" in partitions

    factory = partitions["factory"]
    tools_data = partitions["tools_data"]
    factory_end = parse_size(factory["offset"]) + parse_size(factory["size"])

    assert factory["kind"] == "app"
    assert tools_data["kind"] == "data"
    assert tools_data["subtype"] == "nvs"
    assert parse_size(tools_data["offset"]) == factory_end
    assert parse_size(tools_data["offset"]) >= 0x700000
    assert parse_size(tools_data["offset"]) + parse_size(tools_data["size"]) == 0x800000


def test_smoking_store_uses_dedicated_partition_with_legacy_migration() -> None:
    source = STORE.read_text(encoding="utf-8")
    assert '#define STORE_PARTITION TOOLS_STORAGE_PARTITION' in source
    assert '#define TOOLS_STORAGE_PARTITION "tools_data"' in (
        ROOT / "main" / "tools_storage.h"
    ).read_text(encoding="utf-8")
    assert "nvs_flash_init_partition(STORE_PARTITION)" in source
    assert "nvs_open_from_partition(STORE_PARTITION" in source
    assert "open_legacy_store" in source
    assert "STORE_POWER_KEY" in source
    assert "smoking_store_append_power_sample" in source


def test_wifi_and_radio_preferences_use_dedicated_partition() -> None:
    wifi = WIFI.read_text(encoding="utf-8")
    radio = RADIO.read_text(encoding="utf-8")

    assert "load_tools_data_credentials" in wifi
    assert "save_credentials_to_tools_data" in wifi
    assert "clear_tools_data_credentials" in wifi
    assert "nvs_open_from_partition(TOOLS_STORAGE_PARTITION" in wifi
    assert "nvs_open_from_partition(TOOLS_STORAGE_PARTITION" in radio
    assert 'nvs_open("radio"' in radio


def main() -> None:
    test_tools_data_partition_is_reserved_after_factory_app()
    test_smoking_store_uses_dedicated_partition_with_legacy_migration()
    test_wifi_and_radio_preferences_use_dedicated_partition()
    print("Tools app storage contract: PASS")


if __name__ == "__main__":
    main()
