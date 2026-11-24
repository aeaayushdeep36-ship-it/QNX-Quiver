#!/usr/bin/env python3
"""QNX-Quiver host plant and BAR2 shared-memory protocol exerciser."""

from __future__ import annotations

import argparse
import ctypes
import mmap
import math
import os
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

BAR2_BYTES = 0x400000
MAGIC = 0x51564231
VERSION = 3
SUPERBLOCK = 0x0000
FAULTS = 0x1000
RING_OFFSETS = (0x2000, 0x82000, 0x102000, 0x182000)
RING_SLOTS = 4095
SLOT_BYTES = 128
SLOT_DATA = 112
CAN_WIRE_HEADER_BYTES = 8
CAN_WIRE_DATA_BYTES = 64
CAN_WIRE_BYTES = CAN_WIRE_HEADER_BYTES + CAN_WIRE_DATA_BYTES
ACTUATOR_WIRE_BYTES = 24
ACTUATOR_MAX_TTL_MS = 1000
CAN_FLAG_CRC_CORRUPT = 0x01
CAN_FLAG_REMOTE = 0x02
CAN_FLAG_REMOTE = 0x02
CAN_FLAG_EXTENDED = 0x04
CAN_FLAG_FD = 0x08
FAULT_CAN_BUSOFF = 0x01
FAULT_CAN_CRC_CORRUPT = 0x02
FAULT_IMU_STUCK = 0x04
FAULT_LATENCY_SPIKE = 0x08
TYPE_CAN = 1
TYPE_IMU = 2
TYPE_ACTUATOR = 3
FLAG_CRC_CORRUPT = 1

if os.name != "nt":
    import ctypes.util

    _atomic_path = ctypes.util.find_library("atomic")
    _atomic_lib = ctypes.CDLL(_atomic_path) if _atomic_path else None
    if _atomic_lib is not None:
        _atomic_load4 = _atomic_lib.__atomic_load_4
        _atomic_load4.argtypes = (ctypes.c_void_p, ctypes.c_int)
        _atomic_load4.restype = ctypes.c_uint32
        _atomic_store4 = _atomic_lib.__atomic_store_4
        _atomic_store4.argtypes = (ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int)
        _atomic_store4.restype = None
        _atomic_load8 = _atomic_lib.__atomic_load_8
        _atomic_load8.argtypes = (ctypes.c_void_p, ctypes.c_int)
        _atomic_load8.restype = ctypes.c_uint64
        _atomic_store8 = _atomic_lib.__atomic_store_8
        _atomic_store8.argtypes = (ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int)
        _atomic_store8.restype = None
    else:
        _atomic_load4 = _atomic_store4 = _atomic_load8 = _atomic_store8 = None

class SharedMemory:
    def __init__(self, path: Path, initialize: bool = True) -> None:
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        exists = path.exists()
        self.file = path.open("r+b" if exists else "w+b")
        size = os.fstat(self.file.fileno()).st_size
        if not exists:
            self.file.truncate(BAR2_BYTES)
        elif size != BAR2_BYTES:
            self.file.close()
            raise ValueError(f"shared file must be exactly {BAR2_BYTES} bytes, got {size}")
        self.map = mmap.mmap(self.file.fileno(), BAR2_BYTES, access=mmap.ACCESS_WRITE)
        self.base = ctypes.addressof(ctypes.c_char.from_buffer(self.map))
        self._fault_update_lock = threading.Lock()
        if initialize:
            try:
                self._initialize_if_empty()
            except Exception:
                self.close()
                raise
        host_seq_offset = FAULTS + 56
        host_seq = self.atomic_load32(host_seq_offset)
        if host_seq & 1:
            self.atomic_store32(host_seq_offset, host_seq + 1)

    def close(self) -> None:
        self.map.close()
        self.file.close()

    def _initialize_if_empty(self) -> None:
        magic = struct.unpack_from("<I", self.map, SUPERBLOCK)[0]
        if magic == MAGIC:
            version = struct.unpack_from("<H", self.map, SUPERBLOCK + 4)[0]
            if version != VERSION:
                raise ValueError(f"unsupported protocol version {version}")
            return
        if magic != 0:
            raise ValueError(f"unknown shared-memory magic 0x{magic:08x}; refusing to overwrite")
        self.map[:] = b"\0" * BAR2_BYTES
        struct.pack_into("<IHHII", self.map, 0, MAGIC, VERSION, 64, BAR2_BYTES, 1)
        for offset in RING_OFFSETS:
            struct.pack_into("<II", self.map, offset + 12, RING_SLOTS, SLOT_BYTES)
        self.atomic_store32(SUPERBLOCK + 32, 1)
        self.map.flush()

    def _address(self, offset: int):
        return ctypes.c_void_p(self.base + offset)

    def atomic_load32(self, offset: int) -> int:
        if offset & 3:
            raise ValueError("32-bit atomic field is unaligned")
        if os.name == "nt":
            return int(ctypes.c_uint32.from_address(self.base + offset).value)
        if _atomic_load4 is None:
            raise RuntimeError("32-bit shared atomics require libatomic on this platform")
        return int(_atomic_load4(self._address(offset), 5))

    def atomic_store32(self, offset: int, value: int) -> None:
        if offset & 3:
            raise ValueError("32-bit atomic field is unaligned")
        value &= 0xFFFFFFFF
        if os.name == "nt":
            ctypes.c_uint32.from_address(self.base + offset).value = value
            return
        if _atomic_store4 is None:
            raise RuntimeError("32-bit shared atomics require libatomic on this platform")
        _atomic_store4(self._address(offset), value, 5)

    def atomic_load64(self, offset: int) -> int:
        if offset & 7:
            raise ValueError("64-bit atomic field is unaligned")
        if os.name == "nt":
            return int(ctypes.c_uint64.from_address(self.base + offset).value)
        if _atomic_load8 is None:
            raise RuntimeError("64-bit shared atomics require libatomic on this platform")
        return int(_atomic_load8(self._address(offset), 5))

    def atomic_store64(self, offset: int, value: int) -> None:
        if offset & 7:
            raise ValueError("64-bit atomic field is unaligned")
        value &= 0xFFFFFFFFFFFFFFFF
        if os.name == "nt":
            ctypes.c_uint64.from_address(self.base + offset).value = value
            return
        if _atomic_store8 is None:
            raise RuntimeError("64-bit shared atomics require libatomic on this platform")
        _atomic_store8(self._address(offset), value, 5)

    def fault_update(self, update) -> None:
        sequence_offset = FAULTS + 56
        with self._fault_update_lock:
            sequence = self.atomic_load32(sequence_offset)
            if sequence & 1:
                sequence += 1
            self.atomic_store32(sequence_offset, sequence + 1)
            try:
                update()
            finally:
                self.atomic_store32(sequence_offset, sequence + 2)

    def fault_add64(self, offset: int, amount: int) -> None:
        def update() -> None:
            self.atomic_store64(offset, self.atomic_load64(offset) + amount)
        self.fault_update(update)

    def fault_snapshot(self) -> tuple[tuple[int, int], int, tuple[int, ...]]:
        host_sequence_offset = FAULTS + 56
        qnx_sequence_offset = FAULTS + 60
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            host_before = self.atomic_load32(host_sequence_offset)
            qnx_before = self.atomic_load32(qnx_sequence_offset)
            if (host_before | qnx_before) & 1:
                time.sleep(0)
                continue
            flags = self.atomic_load32(FAULTS)
            latency = self.atomic_load32(FAULTS + 4)
            counters = tuple(self.atomic_load64(FAULTS + offset)
                             for offset in (8, 16, 24, 32, 40, 48, 64,
                                            72, 80, 88, 96, 104, 120, 128))
            host_after = self.atomic_load32(host_sequence_offset)
            qnx_after = self.atomic_load32(qnx_sequence_offset)
            if (host_before == host_after and qnx_before == qnx_after and
                    not ((host_after | qnx_after) & 1)):
                return (host_after, qnx_after), flags, (latency, *counters)
            time.sleep(0)
        raise TimeoutError("fault snapshot writer did not complete within 1 second")

    def set_faults(self, flags: int | None = None, latency_us: int | None = None) -> None:
        def update() -> None:
            if flags is not None:
                self.atomic_store32(FAULTS, flags)
            if latency_us is not None:
                self.atomic_store32(FAULTS + 4, max(0, min(latency_us, 1_000_000)))
        self.fault_update(update)

    def ring_dropped_add(self, ring_index: int, amount: int = 1) -> None:
        base = RING_OFFSETS[ring_index]
        self.atomic_store32(base + 8, self.atomic_load32(base + 8) + amount)

    def record_actuator_result(self, accepted: bool, command_id: int,
                               value: float = 0.0) -> None:
        def update() -> None:
            if accepted:
                offset = FAULTS + 72
                id_offset = FAULTS + 96
            else:
                offset = FAULTS + 80
                id_offset = FAULTS + 104
            self.atomic_store64(offset, self.atomic_load64(offset) + 1)
            self.atomic_store64(id_offset, command_id)
            if accepted:
                output_bits = struct.unpack("<I", struct.pack("<f", value))[0]
                self.atomic_store32(FAULTS + 112, output_bits)
        self.fault_update(update)

    def set_actuator_output(self, value: float) -> None:
        bits = struct.unpack("<I", struct.pack("<f", value))[0]
        self.fault_update(lambda: self.atomic_store32(FAULTS + 112, bits))

    def ring_push(self, ring_index: int, slot_type: int, timestamp_ns: int, payload: bytes,
                  flags: int = 0, sequence: int = 0) -> bool:
        base = RING_OFFSETS[ring_index]
        head = self.atomic_load32(base)
        tail = self.atomic_load32(base + 4)
        next_head = (head + 1) % RING_SLOTS
        if next_head == tail:
            self.atomic_store32(base + 8, self.atomic_load32(base + 8) + 1)
            return False
        slot_offset = base + 64 + head * SLOT_BYTES
        struct.pack_into("<IHHQ", self.map, slot_offset, sequence, slot_type, flags, timestamp_ns)
        self.map[slot_offset + 16:slot_offset + SLOT_BYTES] = bytes(SLOT_DATA)
        self.map[slot_offset + 16:slot_offset + 16 + min(len(payload), SLOT_DATA)] = payload[:SLOT_DATA]
        self.atomic_store32(base, next_head)
        return True

    def ring_pop(self, ring_index: int):
        base = RING_OFFSETS[ring_index]
        head = self.atomic_load32(base)
        tail = self.atomic_load32(base + 4)
        if tail == head:
            return None
        slot_offset = base + 64 + tail * SLOT_BYTES
        sequence, slot_type, flags, timestamp_ns = struct.unpack_from("<IHHQ", self.map, slot_offset)
        payload = bytes(self.map[slot_offset + 16:slot_offset + SLOT_BYTES])
        self.atomic_store32(base + 4, (tail + 1) % RING_SLOTS)
        return sequence, slot_type, flags, timestamp_ns, payload


def set_fault_command(shared: SharedMemory, command: str) -> str:
    current = shared.atomic_load32(FAULTS)
    tokens = command.strip().lower().split()
    if not tokens:
        return "commands: fault busoff|crc|imu_stuck, latency <us>, clear, status, quit"
    if tokens[0] == "fault" and len(tokens) == 2:
        bits = {"busoff": FAULT_CAN_BUSOFF, "crc": FAULT_CAN_CRC_CORRUPT,
                "imu_stuck": FAULT_IMU_STUCK}
        if tokens[1] not in bits:
            return "unknown fault; use busoff, crc, or imu_stuck"
        shared.set_faults(current | bits[tokens[1]])
        return f"fault enabled: {tokens[1]}"
    if tokens[0] == "latency" and len(tokens) == 2:
        try:
            delay = int(tokens[1])
        except ValueError:
            return "latency requires an integer number of microseconds"
        shared.set_faults(current | FAULT_LATENCY_SPIKE, latency_us=delay)
        return f"latency spike set to {max(0, min(delay, 1_000_000))} us"
    if tokens[0] == "clear":
        shared.set_faults(0, latency_us=0)
        return "all faults cleared"
    if tokens[0] == "status":
        generation, flags, values = shared.fault_snapshot()
        output_bits = shared.atomic_load32(FAULTS + 112)
        actuator_output = struct.unpack("<f", struct.pack("<I", output_bits))[0]
        return (f"fault_flags=0x{flags:08x} latency_us={values[0]} "
                f"snapshot={generation} actuator_output={actuator_output:.4f} "
                f"actuator_ack={values[8]} actuator_rejected={values[9]} "
                f"qnx_can_dropped={values[7]} qnx_imu_dropped={values[13]} "
                f"qnx_actuator_dropped={values[14]}")
    return "commands: fault busoff|crc|imu_stuck, latency <us>, clear, status, quit"


def can_frame_valid(slot_type: int, payload: bytes) -> bool:
    if slot_type != TYPE_CAN or len(payload) < CAN_WIRE_BYTES:
        return False
    can_id, length, flags, reserved = struct.unpack_from("<IBBH", payload)
    legal_dlc = (length <= 8 or length in (12, 16, 20, 24, 32, 48, 64))
    return (can_id <= 0x1FFFFFFF and legal_dlc and reserved == 0 and
            (flags & ~(CAN_FLAG_CRC_CORRUPT | CAN_FLAG_REMOTE |
                       CAN_FLAG_EXTENDED | CAN_FLAG_FD)) == 0 and
            bool(flags & CAN_FLAG_FD) and not (flags & CAN_FLAG_REMOTE) and
            (bool(flags & CAN_FLAG_EXTENDED) or can_id <= 0x7FF))


def decode_actuator(slot_type: int, payload: bytes):
    if slot_type != TYPE_ACTUATOR or len(payload) < ACTUATOR_WIRE_BYTES:
        return None
    axis, value, ttl_ms, reserved, command_id = struct.unpack_from("<IfIIQ", payload)
    if (axis != 0 or reserved != 0 or not math.isfinite(value) or
            abs(value) > 1.0 or not 0 < ttl_ms <= ACTUATOR_MAX_TTL_MS or
            command_id == 0):
        return None
    return value, ttl_ms, command_id


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="qnx-quiver-") as directory:
        shared = SharedMemory(Path(directory) / "bar2.bin")
        try:
            assert struct.unpack_from("<I", shared.map, 0)[0] == MAGIC
            assert struct.unpack_from("<I", shared.map, 8)[0] == BAR2_BYTES
            item = b"protocol-self-test"
            for index in range(RING_SLOTS - 1):
                assert shared.ring_push(2, TYPE_IMU, 123456 + index, item, sequence=index)
            assert not shared.ring_push(2, TYPE_IMU, 999999, item, sequence=999999)
            assert shared.atomic_load32(RING_OFFSETS[2] + 8) == 1
            for index in range(RING_SLOTS - 1):
                value = shared.ring_pop(2)
                assert value is not None and value[0] == index
            assert shared.ring_push(2, TYPE_IMU, 222222, item, sequence=7)
            value = shared.ring_pop(2)
            assert value is not None
            assert value[:4] == (7, TYPE_IMU, 0, 222222)
            assert value[4][:len(item)] == item
            can_data = bytes(range(CAN_WIRE_DATA_BYTES))
            can_payload = struct.pack("<IBBH", 0x1ABCDE, CAN_WIRE_DATA_BYTES,
                                      CAN_FLAG_FD | CAN_FLAG_EXTENDED, 0) + can_data
            assert len(can_payload) == CAN_WIRE_BYTES
            assert shared.ring_push(1, TYPE_CAN, 333333, can_payload,
                                    flags=0, sequence=9)
            can_value = shared.ring_pop(1)
            assert can_value is not None
            assert can_value[:4] == (9, TYPE_CAN, 0, 333333)
            assert can_value[4][:CAN_WIRE_BYTES] == can_payload
            set_fault_command(shared, "fault busoff")
            assert shared.atomic_load32(FAULTS) == FAULT_CAN_BUSOFF
            set_fault_command(shared, "clear")
            assert shared.atomic_load32(FAULTS) == 0
            workers = [threading.Thread(
                target=lambda: [shared.fault_add64(FAULTS + 8, 1)
                                for _ in range(250)]) for _ in range(4)]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join()
            generations, _, values = shared.fault_snapshot()
            assert generations[0] % 2 == 0 and generations[1] % 2 == 0
            assert values[1] == 1000
            shared.atomic_store32(FAULTS + 60, 1)
            try:
                shared.fault_snapshot()
                raise AssertionError("stalled sequence writer must time out")
            except TimeoutError:
                pass
            finally:
                shared.atomic_store32(FAULTS + 60, 2)
        finally:
            shared.close()
    print("QNX-Quiver host protocol self-test passed")


def run_simulator(shared: SharedMemory, hz: int, duration: float | None = None,
                  interactive: bool | None = None) -> None:
    period_ns = 1_000_000_000 // hz
    sequence = 0
    step = 1.0 / hz
    position = 0.0
    velocity = 0.0
    last_imu = (0.0,) * 6
    last_imu_timestamp_ns = 0
    actuator_value = 0.0
    actuator_deadline_ns = 0
    last_report = time.monotonic()
    cycles_this_report = 0
    wake_lateness_us: list[float] = []
    missed_deadlines = 0
    command = {"quit": False}

    def read_console() -> None:
        for line in sys.stdin:
            stripped = line.strip().lower()
            if stripped == "quit":
                command["quit"] = True
                return
            print(set_fault_command(shared, line), flush=True)

    if interactive is None:
        interactive = sys.stdin.isatty()
    if interactive:
        threading.Thread(target=read_console, daemon=True).start()
        print("Enter fault busoff|crc|imu_stuck, latency <us>, clear, status, or quit.", flush=True)

    deadline = time.monotonic_ns()
    stop_at = None if duration is None else time.monotonic() + duration
    try:
        while not command["quit"] and (stop_at is None or time.monotonic() < stop_at):
            now_ns = time.monotonic_ns()
            _, flags, fault_values = shared.fault_snapshot()
            latency_us = fault_values[0]
            actuator = shared.ring_pop(3)
            if actuator is not None:
                _, actuator_type, _, _, actuator_payload = actuator
                decoded = decode_actuator(actuator_type, actuator_payload)
                if decoded is not None:
                    value, ttl_ms, command_id = decoded
                    actuator_value = value
                    actuator_deadline_ns = now_ns + ttl_ms * 1_000_000
                    shared.record_actuator_result(True, command_id, value)
                else:
                    rejected_id = (struct.unpack_from("<Q", actuator_payload, 16)[0]
                                   if actuator_type == TYPE_ACTUATOR and
                                   len(actuator_payload) >= ACTUATOR_WIRE_BYTES else 0)
                    shared.record_actuator_result(False, rejected_id)
            if now_ns >= actuator_deadline_ns:
                if actuator_value != 0.0:
                    shared.set_actuator_output(0.0)
                actuator_value = 0.0
            torque = actuator_value

            accel = max(-8.0, min(8.0, torque)) - 0.15 * velocity
            velocity += accel * step
            position += velocity * step
            imu = (accel, 0.0, 9.81, 0.0, 0.0, velocity)
            if flags & FAULT_IMU_STUCK:
                imu = last_imu
                imu_timestamp_ns = last_imu_timestamp_ns
            else:
                last_imu = imu
                last_imu_timestamp_ns = now_ns
                imu_timestamp_ns = now_ns
            if flags & FAULT_LATENCY_SPIKE and latency_us:
                time.sleep(latency_us / 1_000_000.0)
            shared.atomic_store64(SUPERBLOCK + 16, now_ns)
            imu_payload = struct.pack("<6f", *imu)
            if not shared.ring_push(2, TYPE_IMU, imu_timestamp_ns, imu_payload,
                                    sequence=sequence):
                shared.fault_add64(FAULTS + 88, 1)
            if sequence % max(1, hz // 100) == 0 and not (flags & FAULT_CAN_BUSOFF):
                can_flags = CAN_FLAG_FD
                if flags & FAULT_CAN_CRC_CORRUPT:
                    can_flags |= CAN_FLAG_CRC_CORRUPT
                can_payload = struct.pack("<IBBH", 0x201, 8, can_flags, 0)
                can_payload += struct.pack("<ff", position, velocity) + bytes(56)
                shared.ring_push(0, TYPE_CAN, now_ns, can_payload, flags=can_flags, sequence=sequence)
                shared.fault_add64(FAULTS + 8, 1)
            if not (flags & FAULT_CAN_BUSOFF):
                while (transmitted := shared.ring_pop(1)) is not None:
                    tx_sequence, tx_type, tx_flags, tx_timestamp, tx_payload = transmitted
                    shared.fault_add64(FAULTS + 16, 1)
                    if not can_frame_valid(tx_type, tx_payload):
                        shared.fault_add64(FAULTS + 40, 1)
                        continue
                    if not shared.ring_push(0, TYPE_CAN, tx_timestamp, tx_payload,
                                            flags=tx_flags, sequence=tx_sequence):
                        shared.fault_add64(FAULTS + 40, 1)

            sequence = (sequence + 1) & 0xFFFFFFFF
            cycles_this_report += 1
            deadline += period_ns
            remaining = deadline - time.monotonic_ns()
            if remaining > 0:
                time.sleep(remaining / 1_000_000_000.0)
                lateness_ns = max(0, time.monotonic_ns() - deadline)
            else:
                missed_deadlines += 1
                lateness_ns = -remaining
                deadline = time.monotonic_ns()
            wake_lateness_us.append(lateness_ns / 1000.0)

            report_now = time.monotonic()
            if report_now - last_report >= 1.0:
                measured_hz = cycles_this_report / (report_now - last_report)
                _, _, fault_values = shared.fault_snapshot()
                samples = sorted(wake_lateness_us)
                def percentile(fraction: float) -> float:
                    return samples[min(len(samples) - 1,
                                       int((len(samples) - 1) * fraction))]
                print(f"sim measured_hz={measured_hz:.1f} target_hz={hz} "
                      f"pos={position:.3f} vel={velocity:.3f} "
                      f"faults=0x{flags:02x} can_rx={fault_values[2]} "
                      f"wake_late_us[p50/p95/p99/max]={percentile(0.50):.1f}/"
                      f"{percentile(0.95):.1f}/{percentile(0.99):.1f}/"
                      f"{samples[-1]:.1f} deadline_misses={missed_deadlines}",
                      flush=True)
                cycles_this_report = 0
                wake_lateness_us.clear()
                missed_deadlines = 0
                last_report = report_now
    except KeyboardInterrupt:
        pass
    finally:
        shared.atomic_store64(SUPERBLOCK + 16, 0)
        shared.set_actuator_output(0.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shm", type=Path, default=Path(tempfile.gettempdir()) / "qnx_quiver_shm_v3.bin")
    parser.add_argument("--hz", type=int, default=1000)
    parser.add_argument("--duration", type=float, help="stop after this many seconds (useful for smoke checks)")
    parser.add_argument("--latency-spike-us", type=int, default=0,
                        help="apply a repeatable host delay each simulator cycle")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not 1 <= args.hz <= 10000:
        parser.error("--hz must be between 1 and 10000")
    if args.duration is not None and args.duration <= 0:
        parser.error("--duration must be greater than zero")
    if not 0 <= args.latency_spike_us <= 1_000_000:
        parser.error("--latency-spike-us must be between 0 and 1000000")
    shared = SharedMemory(args.shm)
    try:
        if args.latency_spike_us:
            shared.set_faults(FAULT_LATENCY_SPIKE,
                              latency_us=args.latency_spike_us)
        print(f"QNX-Quiver simulator mapped {args.shm} ({BAR2_BYTES} bytes) at {args.hz} Hz", flush=True)
        run_simulator(shared, args.hz, args.duration)
    finally:
        shared.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
