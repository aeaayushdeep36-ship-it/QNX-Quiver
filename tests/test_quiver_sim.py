import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host" / "sim_engine"))
import quiver_sim as qv


class QuiverProtocolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="qnx-quiver-test-")
        self.shared = qv.SharedMemory(Path(self.tmp.name) / "bar2.bin")

    def tearDown(self):
        self.shared.close()
        self.tmp.cleanup()

    def test_v3_layout_and_serialized_multi_client_work_sharing(self):
        self.assertEqual(qv.VERSION, 3)
        self.assertEqual(len(self.shared.map), qv.BAR2_BYTES)
        for sequence in range(20):
            self.assertTrue(self.shared.ring_push(2, qv.TYPE_IMU, sequence,
                                                  b"\0" * 24, sequence=sequence))
        # The resource manager serializes devctl requests. Separate clients are
        # deliberately work-shared: every record is consumed once in dispatch order.
        first, second = [], []
        while True:
            slot = self.shared.ring_pop(2)
            if slot is None:
                break
            (first if slot[0] % 2 == 0 else second).append(slot[0])
        merged = first + second
        self.assertEqual(sorted(merged), list(range(20)))
        self.assertEqual(len(set(merged)), 20)

    def test_stalled_snapshot_writer_times_out(self):
        self.shared.atomic_store32(qv.FAULTS + 60, 1)
        with self.assertRaises(TimeoutError):
            self.shared.fault_snapshot()
        self.shared.atomic_store32(qv.FAULTS + 60, 2)

    def test_fault_snapshot_includes_qnx_side_drop_metrics(self):
        self.shared.atomic_store64(qv.FAULTS + 120, 3)
        self.shared.atomic_store64(qv.FAULTS + 128, 4)
        _, _, values = self.shared.fault_snapshot()
        self.assertEqual(values[13:], (3, 4))

    def test_rejects_old_protocol_and_bad_mapping_size(self):
        self.shared.map[4:6] = (2).to_bytes(2, "little")
        with self.assertRaisesRegex(ValueError, "unsupported protocol version"):
            qv.SharedMemory(self.shared.path)
        bad_path = Path(self.tmp.name) / "short.bin"
        bad_path.write_bytes(b"\0" * 32)
        with self.assertRaisesRegex(ValueError, "exactly"):
            qv.SharedMemory(bad_path)

    def test_can_fd_validation(self):
        data = bytes(range(64))
        payload = qv.struct.pack("<IBBH", 0x1ABCDE, 64,
                                 qv.CAN_FLAG_FD | qv.CAN_FLAG_EXTENDED, 0) + data
        self.assertTrue(qv.can_frame_valid(qv.TYPE_CAN, payload))
        bad_dlc = qv.struct.pack("<IBBH", 0x123, 9, qv.CAN_FLAG_FD, 0) + data
        self.assertFalse(qv.can_frame_valid(qv.TYPE_CAN, bad_dlc))
        bad_id = qv.struct.pack("<IBBH", 0x1ABCDE, 8, qv.CAN_FLAG_FD, 0) + data
        self.assertFalse(qv.can_frame_valid(qv.TYPE_CAN, bad_id))

    def test_can_tx_ring_full_reports_backpressure(self):
        payload = qv.struct.pack("<IBBH", 0x1ABCDE, 64,
                                 qv.CAN_FLAG_FD | qv.CAN_FLAG_EXTENDED, 0) + bytes(64)
        for sequence in range(qv.RING_SLOTS - 1):
            self.assertTrue(self.shared.ring_push(1, qv.TYPE_CAN, sequence,
                                                  payload, sequence=sequence))
        self.assertFalse(self.shared.ring_push(1, qv.TYPE_CAN, 999999,
                                               payload, sequence=999999))
        self.assertEqual(self.shared.atomic_load32(qv.RING_OFFSETS[1] + 8), 1)

    def test_actuator_bounds_ack_id_and_expiry_contract(self):
        payload = qv.struct.pack("<IfIIQ", 0, 0.5, 50, 0, 123) + bytes(88)
        self.assertEqual(qv.decode_actuator(qv.TYPE_ACTUATOR, payload),
                         (0.5, 50, 123))
        self.assertIsNone(qv.decode_actuator(qv.TYPE_ACTUATOR,
                         qv.struct.pack("<IfIIQ", 0, 2.0, 50, 0, 123) + bytes(88)))
        self.assertIsNone(qv.decode_actuator(qv.TYPE_ACTUATOR,
                         qv.struct.pack("<IfIIQ", 0, 0.5, 5000, 0, 123) + bytes(88)))

    def test_simulator_acknowledges_and_expires_actuator_output(self):
        errors = []
        def run():
            try:
                qv.run_simulator(self.shared, 500, duration=0.35, interactive=False)
            except Exception as error:
                errors.append(error)
        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        valid = qv.struct.pack("<IfIIQ", 0, 0.5, 50, 0, 1234) + bytes(88)
        self.assertTrue(self.shared.ring_push(3, qv.TYPE_ACTUATOR,
                                              time.monotonic_ns(), valid))
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline:
            _, _, values = self.shared.fault_snapshot()
            if values[11] == 1234:
                break
            time.sleep(0.002)
        self.assertEqual(values[11], 1234)
        output = qv.struct.unpack("<f", qv.struct.pack(
            "<I", self.shared.atomic_load32(qv.FAULTS + 112)))[0]
        self.assertEqual(output, 0.5)
        invalid = qv.struct.pack("<IfIIQ", 0, 2.0, 50, 0, 5678) + bytes(88)
        self.assertTrue(self.shared.ring_push(3, qv.TYPE_ACTUATOR,
                                              time.monotonic_ns(), invalid))
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline:
            _, _, values = self.shared.fault_snapshot()
            if values[12] == 5678:
                break
            time.sleep(0.002)
        self.assertEqual(values[12], 5678)
        worker.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertFalse(errors)
        output = qv.struct.unpack("<f", qv.struct.pack(
            "<I", self.shared.atomic_load32(qv.FAULTS + 112)))[0]
        self.assertEqual(output, 0.0)

    def test_bus_off_holds_qnx_tx_until_recovery(self):
        payload = qv.struct.pack("<IBBH", 0x321, 8, qv.CAN_FLAG_FD, 0) + bytes(64)
        self.assertTrue(self.shared.ring_push(1, qv.TYPE_CAN, 10, payload,
                                              sequence=0xAABB))
        self.shared.set_faults(qv.FAULT_CAN_BUSOFF)
        qv.run_simulator(self.shared, 100, duration=0.03, interactive=False)
        self.assertEqual(self.shared.atomic_load32(qv.RING_OFFSETS[1] + 4), 0)
        self.shared.set_faults(0)
        qv.run_simulator(self.shared, 100, duration=0.03, interactive=False)
        self.assertEqual(self.shared.atomic_load32(qv.RING_OFFSETS[1] + 4), 1)
        returned = None
        while (candidate := self.shared.ring_pop(0)) is not None:
            if candidate[0] == 0xAABB:
                returned = candidate
                break
        self.assertIsNotNone(returned)
        self.assertEqual(returned[4][:qv.CAN_WIRE_BYTES], payload)

    def test_imu_stuck_holds_source_timestamp_until_recovery(self):
        self.shared.set_faults(qv.FAULT_IMU_STUCK)
        qv.run_simulator(self.shared, 100, duration=0.03, interactive=False)
        stuck_timestamps = []
        while (sample := self.shared.ring_pop(2)) is not None:
            stuck_timestamps.append(sample[3])
        self.assertGreater(len(stuck_timestamps), 1)
        self.assertEqual(set(stuck_timestamps), {0})
        self.shared.set_faults(0)
        qv.run_simulator(self.shared, 100, duration=0.03, interactive=False)
        resumed = self.shared.ring_pop(2)
        self.assertIsNotNone(resumed)
        self.assertGreater(resumed[3], 0)


if __name__ == "__main__":
    unittest.main()
