"""Validate actual C++ JSON/discovery output and render every Jinja template."""
import json
import subprocess
import sys

from jinja2 import Environment, StrictUndefined


def main():
    executable = sys.argv[1]
    output = subprocess.check_output([executable, "--emit"], text=True)
    rows = [json.loads(line) for line in output.splitlines() if line.strip()]
    states = {row["case"]: row["state"] for row in rows if "case" in row}
    discovery = [row for row in rows if "discovery" in row]
    assert len(discovery) == 93
    assert states["full"]["online"] is True and states["full"]["complete"] is True
    assert states["partial"]["online"] is True and states["partial"]["complete"] is False
    assert states["partial"]["soc"] is None and states["partial"]["charge_allowed"] is None
    assert states["disconnected"]["online"] is False and states["disconnected"]["valid"] == 0
    assert states["identity_only"]["online"] is False
    assert states["invalid_numeric"]["cell_v"] is None and states["invalid_numeric"]["power"] is None
    assert len(states["full"]["cell_v"]) == 24
    assert states["full"]["current"] == -12 and states["full"]["power"] < 0
    assert '"mock"\\test' in states["full"]["model"]
    env = Environment(undefined=StrictUndefined)
    unique, topics = set(), set()
    binary_count = cell_count = temperature_count = 0
    for row in discovery:
        p = row["payload"]
        assert p["unique_id"] not in unique and row["topic"] not in topics
        unique.add(p["unique_id"])
        topics.add(row["topic"])
        assert "command_topic" not in p
        assert p["availability_mode"] == "all" and len(p["availability"]) == 3
        assert p["expire_after"] == 25
        assert p["device"]["configuration_url"] == "http://brain-zero.local/settings/battery"
        template = env.from_string(p["value_template"])
        for state in states.values():
            template.render(value_json=state)
            for availability in p["availability"]:
                if "value_template" in availability:
                    result = env.from_string(availability["value_template"]).render(value_json=state)
                    assert result in ("online", "offline")
        if "/binary_sensor/" in row["topic"]:
            binary_count += 1
            assert p["payload_on"] == "ON" and p["payload_off"] == "OFF"
            key = p["unique_id"].split("_bms_", 1)[1].split("_", 1)[1]
            for value, expected in [(True, "ON"), (False, "OFF"), (None, "None")]:
                assert template.render(value_json={key: value}) == expected
                field_available = env.from_string(p["availability"][2]["value_template"])
                assert field_available.render(value_json={key: value}) == ("offline" if value is None else "online")
        if "_cell_" in p["unique_id"] and p["unique_id"].rsplit("_", 1)[-1].isdigit():
            cell_count += 1
            assert len(p["value_template"]) < 100
            assert template.render(value_json={"cell_v": None}) == "None"
            assert template.render(value_json={"cell_v": []}) == "None"
            assert template.render(value_json={"cell_v": [3.3]}) in ("None", "3.3")
        if p.get("device_class") == "temperature":
            temperature_count += 1
            assert p["unit_of_measurement"] == "°C"
    assert binary_count == 9 and cell_count == 24 and temperature_count == 12
    print("Parsed six battery states and93 unique discovery messages; all availability, binary, cell and UTF-8 unit templates passed")


if __name__ == "__main__":
    main()
