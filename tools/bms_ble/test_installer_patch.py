"""Exercise the actual POSIX awk patch using synthetic, nonprivate YAML."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("install_ha_bridge.sh").read_text()
AWK_PROGRAM = SCRIPT.split("awk '\n", 1)[1].split("' \"$config\"", 1)[0]
AWK = shutil.which("awk") or ("C:/Program Files/Git/usr/bin/awk.exe" if os.name == "nt" else None)
FIXTURE = """esphome:
  name: pacecan
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  reboot_timeout: 5min
api:
  encryption:
    key: !secret api_key
  reboot_timeout: 15min
mqtt:
  broker: !secret broker
  reboot_timeout: 15min
ota:
  - platform: esphome
    password: !secret ota_password
canbus:
  - platform: esp32_can
    tx_pin: GPIO19
    rx_pin: GPIO21
    can_id: 0x0E5
    bit_rate: 500kbps
    on_frame:
      - can_id: 0x355
        then:
          - lambda: |-
              id(state_of_charge).publish_state(x[0]);
external_components:
  - source:
      type: local
      path: components
    components: [battery_link]
"""


@unittest.skipUnless(AWK and Path(AWK).exists(), "POSIX awk is required")
class InstallerPatchTests(unittest.TestCase):
    def run_patch(self, value):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "source.yaml"
            p.write_text(value)
            return subprocess.run([AWK, AWK_PROGRAM, str(p)], text=True, capture_output=True)

    def test_preserves_credentials_receiver_and_ota(self):
        r = self.run_patch(FIXTURE)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.count("reboot_timeout: 0s"), 3)
        self.assertNotIn("reboot_timeout: 5min", r.stdout)
        self.assertNotIn("reboot_timeout: 15min", r.stdout)
        for line in FIXTURE.splitlines():
            if "reboot_timeout:" not in line:
                self.assertIn(line + "\n", r.stdout)
        self.assertIn("    id: pace_can_bus\n", r.stdout)
        self.assertIn("    rx_queue_len: 5\n", r.stdout)
        self.assertIn("    max_frames_per_loop: 50\n", r.stdout)
        self.assertIn("packages:\n  brain_zero_pace_ble: !include pace-can-ble.yaml\n", r.stdout)

    def test_existing_packages_and_expected_id_are_preserved(self):
        value = FIXTURE.replace("    tx_pin:", "    id: pace_can_bus\n    tx_pin:")
        value += "packages:\n  existing: !include existing.yaml\n"
        r = self.run_patch(value)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.count("    id: pace_can_bus"), 1)
        self.assertEqual(r.stdout.count("\npackages:"), 1)
        self.assertIn("  existing: !include existing.yaml", r.stdout)

    def test_three_space_mqtt_and_nested_fields_are_preserved(self):
        value = FIXTURE.replace("mqtt:\n  broker: !secret broker\n  reboot_timeout: 15min\n",
                                "mqtt:\n   idf_send_async: true\n   broker: !secret broker\n   reboot_timeout: 15min\n   username: !secret mqtt_user\n   password: !secret mqtt_password\n")
        value = value.replace("  password: !secret wifi_password\n",
                              "  password: !secret wifi_password\n  ap:\n    ssid: !secret fallback_ssid\n    password: !secret fallback_password\n")
        r = self.run_patch(value)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("mqtt:\n   reboot_timeout: 0s\n   idf_send_async: true\n", r.stdout)
        self.assertNotIn("mqtt:\n  reboot_timeout:", r.stdout)
        for line in value.splitlines():
            if "reboot_timeout:" not in line:
                self.assertIn(line + "\n", r.stdout)
        self.assertEqual(r.stdout.count("reboot_timeout: 0s"), 3)

    def test_rejects_wrong_id_oversize_queue_and_existing_ble(self):
        for value in (
                FIXTURE.replace("    tx_pin:", "    id: different_bus\n    tx_pin:"),
                FIXTURE.replace("    tx_pin:", "    rx_queue_len: 51\n    tx_pin:"),
                FIXTURE + "esp32_ble_server:\n  max_clients: 1\n",
                FIXTURE + "packages:\n  brain_zero_pace_ble: !include pace-can-ble.yaml\n",
                FIXTURE.replace("esp32_can", "mcp2515")):
            r = self.run_patch(value)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("Refused:", r.stderr)

    def test_explicit_none_gets_coexistence_mode_without_altering_credentials(self):
        for spelling in ("none", "NONE", '\"None\"', "'none'"):
            value = FIXTURE.replace("wifi:\n", f"wifi:\n  power_save_mode: {spelling}  # radio policy\n")
            value = value.replace("  ssid: !secret wifi_ssid", "  ssid: none")
            r = self.run_patch(value)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("  power_save_mode: light  # radio policy\n", r.stdout)
            self.assertIn("  ssid: none\n", r.stdout)
            for line in FIXTURE.splitlines():
                if "reboot_timeout:" not in line and "  ssid:" not in line:
                    self.assertIn(line + "\n", r.stdout)

    def test_existing_coexistence_modes_and_nested_values_are_preserved(self):
        for mode in ("light", "HIGH"):
            value = FIXTURE.replace("wifi:\n", f"wifi:\n  power_save_mode: {mode}  # owner choice\n")
            value = value.replace("  password: !secret wifi_password\n", "  password: !secret wifi_password\n  ap:\n    ssid: none\n    password: !secret fallback_password\n")
            r = self.run_patch(value)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(f"  power_save_mode: {mode}  # owner choice\n", r.stdout)
            self.assertIn("    ssid: none\n", r.stdout)
            self.assertIn("    password: !secret fallback_password\n", r.stdout)


if __name__ == "__main__":
    unittest.main()
