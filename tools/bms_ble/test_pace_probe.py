"""Contract/freshness tests; no Bluetooth or Home Assistant access."""
import struct
import unittest

from pace_probe import Receiver


def meta(ticks=1000, boot=123, rx=42, dropped=0):
    return b"BZCB\x01\x01" + struct.pack("<IIIH", boot, ticks, rx, dropped)


def frame(can_id=0x356, seq=1, ticks=1050, payload=struct.pack("<Hhh", 5250, -115, 292)):
    return bytes((1, len(payload))) + struct.pack("<HII", can_id, seq, ticks) + payload.ljust(8, b"\x00")


class ContractTests(unittest.TestCase):
    def setup_receiver(self):
        r = Receiver()
        self.assertTrue(r.metadata(meta(), 10000, 10020))
        return r

    def test_signed_current_and_units(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(), 10070))
        values = r.snapshot(10070)["fresh_values"]
        self.assertEqual(values["voltage_v"], 52.5)
        self.assertEqual(values["current_a"], -11.5)
        self.assertEqual(values["average_temperature_c"], 29.2)
        self.assertEqual(r.snapshot(10070)["ages_s"]["0x356"], 0.272)

    def test_unknown_and_writable_metadata_rejected(self):
        r = Receiver()
        for bad in (meta()[:-1], b"ABCD" + meta()[4:], meta()[:5] + b"\x00" + meta()[6:], meta(boot=0)):
            self.assertFalse(r.metadata(bad, 10000, 10010))
        self.assertFalse(r.receive(frame(), 10070))

    def test_rtt_and_clock_expiry(self):
        r = Receiver()
        self.assertFalse(r.metadata(meta(), 10000, 10251))
        self.assertTrue(r.metadata(meta(), 10000, 10250))
        self.assertFalse(r.receive(frame(ticks=1000), 70001))

    def test_original_source_age_rejects_delayed_record(self):
        r = self.setup_receiver()
        self.assertFalse(r.receive(frame(ticks=1000), 20001))
        self.assertEqual(r.last_error, "source_age")
        self.assertFalse(r.receive(frame(ticks=2000), 10020))

    def test_per_id_freshness(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(0x351, 1, 1000, struct.pack("<HHH", 577, 1450, 1550)), 10020))
        self.assertTrue(r.receive(frame(0x35C, 2, 1000, b"\xc0"), 10020))
        self.assertTrue(r.receive(frame(seq=3, ticks=10700), 19720))
        fields = r.snapshot(20271)["fresh_values"]
        self.assertIn("voltage_v", fields)
        self.assertNotIn("charge_voltage_limit_v", fields)
        self.assertNotIn("charge_enabled", fields)

    def test_sequence_duplicate_old_and_wrap(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(seq=0xFFFFFFFE), 10070))
        self.assertTrue(r.receive(frame(seq=0xFFFFFFFF), 10070))
        self.assertTrue(r.receive(frame(seq=0), 10070))
        self.assertFalse(r.receive(frame(seq=0), 10070))
        self.assertFalse(r.receive(frame(seq=0xFFFFFFFE), 10070))

    def test_source_clock_wrap_and_boot_reset(self):
        r = Receiver()
        self.assertTrue(r.metadata(meta(ticks=0xFFFFFFF0), 10000, 10010))
        self.assertTrue(r.receive(frame(ticks=0x00000010), 10042))
        self.assertTrue(r.metadata(meta(ticks=100, boot=456), 20000, 20010))
        self.assertEqual(r.latest, {})
        self.assertTrue(r.receive(frame(seq=1, ticks=100), 20020))

    def test_id_dlc_and_wire_shape(self):
        for bad in (frame(can_id=0x305), frame(payload=b"\x00"), frame()[:-1], frame() + b"\x00",
                    b"\x02" + frame()[1:], frame()[:1] + b"\x09" + frame()[2:]):
            r = self.setup_receiver()
            self.assertFalse(r.receive(bad, 10070))

    def test_invalid_can_values_clear_only_affected_fields(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(0x35C, 1, 1050, b"\xc0"), 10070))
        self.assertTrue(r.receive(frame(seq=2), 10070))
        self.assertFalse(r.receive(frame(0x35C, 3, 1050, b"\xc1"), 10070))
        fields = r.snapshot(10070)["fresh_values"]
        self.assertNotIn("charge_enabled", fields)
        self.assertIn("voltage_v", fields)

    def test_bad_clock_metadata_invalidates_previous_values(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(), 10070))
        self.assertFalse(r.metadata(meta(ticks=5000), 11000, 11020))
        self.assertFalse(r.snapshot(11020)["clock_verified"])
        self.assertEqual(r.snapshot(11020)["fresh_values"], {})

    def test_nonzero_padding_is_rejected(self):
        r = self.setup_receiver()
        value = frame()[:-1] + b"\x01"
        self.assertFalse(r.receive(value, 10070))
        self.assertEqual(r.last_error, "nonzero_padding")

    def test_short_known_frame_invalidates_that_group(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(0x35C, 1, 1050, b"\xc0"), 10070))
        self.assertFalse(r.receive(frame(0x35C, 2, 1050, b""), 10070))
        self.assertNotIn("charge_enabled", r.snapshot(10070)["fresh_values"])

    def test_freshness_boundaries_and_anchor_end(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(), 10070))
        sample_time = r.latest[0x356][0] / 1000
        self.assertIn("voltage_v", r.snapshot(sample_time + 9999.9)["fresh_values"])
        self.assertNotIn("voltage_v", r.snapshot(sample_time + 10000)["fresh_values"])
        self.assertTrue(r.metadata(meta(ticks=1100), 10100, 10350))
        # A 250ms metadata RTT is accepted; its anchor TTL starts at read end.
        self.assertTrue(r.receive(frame(seq=2, ticks=61100), 70200))
        self.assertTrue(r.snapshot(70349.9)["clock_verified"])
        self.assertFalse(r.snapshot(70350)["clock_verified"])

    def test_extrema_and_permissions(self):
        r = self.setup_receiver()
        self.assertTrue(r.receive(frame(0x373, 1, 1050, struct.pack("<HHHH", 3262, 3276, 301, 303)), 10070))
        v = r.snapshot(10070)["fresh_values"]
        self.assertEqual(v["delta_cell_v"], 0.014)
        self.assertEqual(v["minimum_cell_temperature_c"], 27.85)
        self.assertTrue(r.receive(frame(0x35C, 2, 1050, b"\xf8"), 10070))
        self.assertTrue(r.snapshot(10070)["fresh_values"]["charge_enabled"])


if __name__ == "__main__":
    unittest.main()
