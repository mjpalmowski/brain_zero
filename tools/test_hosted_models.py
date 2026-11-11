"""Check automatic model detection across the firmware catalogue and voltage guards."""

import base64
import json
import subprocess
import unittest

from build_hosted_models import OUTPUT, catalogue, generated_models
from export_compatibility import BATTERY_VOLTAGES


class HostedModelsTest(unittest.TestCase):
    def detect(self, profiles):
        driver = r"""
import {readFileSync} from 'node:fs';
const source = JSON.parse(readFileSync(0, 'utf8'));
const {detectModel} = await import('data:text/javascript;base64,' + source.module);
const result = source.profiles.map(profile => {
  try { return {ok: true, name: detectModel(profile)}; }
  catch { return {ok: false}; }
});
process.stdout.write(JSON.stringify(result));
"""
        result = subprocess.run(["node", "--input-type=module", "-e", driver],
                                input=json.dumps({"module": base64.b64encode(OUTPUT.read_bytes()).decode(),
                                                  "profiles": profiles}),
                                text=True, capture_output=True, check=True, timeout=30)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout)

    def test_generated_catalogue_matches_firmware_and_sanitizer_supported_voltages(self):
        self.assertEqual(OUTPUT.read_text(encoding="utf-8"), generated_models())
        models = catalogue()
        self.assertEqual(set(models), set().union(*BATTERY_VOLTAGES.values()))
        for voltage, prefixes in BATTERY_VOLTAGES.items():
            for prefix in prefixes:
                self.assertEqual(models[prefix]["voltage"], voltage)

    def test_every_prefix_detects_exact_family_for_different_firmware_revisions(self):
        cases = [(prefix, model, revision) for prefix, model in catalogue().items()
                 for revision in (0, 556, 563, 999)]
        # schema_firmware is authoritative even when a legacy unit firmware word differs.
        profiles = [{"schema_firmware": prefix * 1000 + revision,
                     "unit_firmware": 1234567, "model": 47, "nominal_voltage": model["voltage"]}
                    for prefix, model, revision in cases]
        actual = self.detect(profiles)
        self.assertEqual(actual, [{"ok": True, "name": model["name"]} for _, model, _ in cases])

    def test_unknown_firmware_and_mismatched_voltages_cannot_select_a_model(self):
        invalid = [None, {}, *[{"schema_firmware": value, "nominal_voltage": 48}
                              for value in (None, True, "2623563", 2623563.5, 563, 999999, 10000000, 2700563)]]
        for prefix, model in catalogue().items():
            invalid.extend({"schema_firmware": prefix * 1000 + 563, "nominal_voltage": voltage}
                           for voltage in (None, "48", 12, 24, 48) if voltage != model["voltage"])
        self.assertEqual(self.detect(invalid), [{"ok": False}] * len(invalid))


if __name__ == "__main__":
    unittest.main()
