#!/usr/bin/env python3
"""QNX-Quiver host plant and BAR2 shared-memory protocol exerciser."""

from __future__ import annotations

import argparse
import ctypes
import mmap
import os
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

BAR2_BYTES = 0x400000
MAGIC = 0x51564231
VERSION = 1
SUPERBLOCK = 0x0000
FAULTS = 0x1000
RING_OFFSETS = (0x2000, 0x82000, 0x102000, 0x182000)
RING_SLOTS = 4095
SLOT_BYTES = 128
SLOT_DATA = 112
CAN_WIRE_HEADER_BYTES = 8
CAN_WIRE_DATA_BYTES = 64
CAN_WIRE_BYTES = CAN_WIRE_HEADER_BYTES + CAN_WIRE_DATA_BYTES
CAN_FLAG_CRC_CORRUPT = 0x01
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
        if initialize:
            try:
                self._initialize_if_empty()
            except Exception:
                self.close()
                raise

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
    def set_faults(self, flags: int | None = None, latency_us: int | None = None) -> None:
        if flags is not None:
            self.atomic_store32(FAULTS, flags)
        if latency_us is not None:
            self.atomic_store32(FAULTS + 4, max(0, min(latency_us, 1_000_000)))

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
        return f"fault_flags=0x{shared.atomic_load32(FAULTS):08x} latency_us={shared.atomic_load32(FAULTS + 4)}"
    return "commands: fault busoff|crc|imu_stuck, latency <us>, clear, status, quit"


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
        finally:
            shared.close()
    print("QNX-Quiver host protocol self-test passed")


def run_simulator(shared: SharedMemory, hz: int, duration: float | None = None) -> None:
    period_ns = 1_000_000_000 // hz
    sequence = 0
    step = 1.0 / hz
    position = 0.0
    velocity = 0.0
    last_imu = (0.0,) * 6
    last_report = time.monotonic()
    cycles_this_report = 0
    command = {"quit": False}

    def read_console() -> None:
        for line in sys.stdin:
            stripped = line.strip().lower()
            if stripped == "quit":
                command["quit"] = True
                return
            print(set_fault_command(shared, line), flush=True)

    if sys.stdin.isatty():
        threading.Thread(target=read_console, daemon=True).start()
        print("Enter fault busoff|crc|imu_stuck, latency <us>, clear, status, or quit.", flush=True)

    deadline = time.monotonic_ns()
    stop_at = None if duration is None else time.monotonic() + duration
    try:
        while not command["quit"] and (stop_at is None or time.monotonic() < stop_at):
            now_ns = time.monotonic_ns()
            flags = shared.atomic_load32(FAULTS)
            latency_us = shared.atomic_load32(FAULTS + 4)
            actuator = shared.ring_pop(3)
            torque = 0.0
            if actuator is not None and actuator[1] == TYPE_ACTUATOR:
                torque = struct.unpack_from("<f", actuator[4])[0]

            accel = max(-8.0, min(8.0, torque)) - 0.15 * velocity
            velocity += accel * step
            position += velocity * step
            imu = (accel, 0.0, 9.81, 0.0, 0.0, velocity)
            if flags & FAULT_IMU_STUCK:
                imu = last_imu
            else:
                last_imu = imu
            if flags & FAULT_LATENCY_SPIKE and latency_us:
                time.sleep(latency_us / 1_000_000.0)
            shared.atomic_store64(SUPERBLOCK + 16, now_ns)
            imu_payload = struct.pack("<6f", *imu)
            shared.ring_push(2, TYPE_IMU, now_ns, imu_payload, sequence=sequence)
            if sequence % max(1, hz // 100) == 0 and not (flags & FAULT_CAN_BUSOFF):
                can_flags = CAN_FLAG_FD
                if flags & FAULT_CAN_CRC_CORRUPT:
                    can_flags |= CAN_FLAG_CRC_CORRUPT
                can_payload = struct.pack("<IBBH", 0x201, 8, can_flags, 0)
                can_payload += struct.pack("<ff", position, velocity) + bytes(56)
                shared.ring_push(0, TYPE_CAN, now_ns, can_payload, flags=can_flags, sequence=sequence)
                shared.atomic_store64(FAULTS + 8, shared.atomic_load64(FAULTS + 8) + 1)
            while (transmitted := shared.ring_pop(1)) is not None:
                tx_sequence, tx_type, tx_flags, tx_timestamp, tx_payload = transmitted
                shared.atomic_store64(FAULTS + 16, shared.atomic_load64(FAULTS + 16) + 1)
                tx_length = tx_payload[4]
                if (tx_type != TYPE_CAN or tx_length > CAN_WIRE_DATA_BYTES or
                        len(tx_payload) < CAN_WIRE_BYTES or
                        not (tx_payload[5] & CAN_FLAG_FD)):
                    shared.atomic_store64(FAULTS + 40,
                                          shared.atomic_load64(FAULTS + 40) + 1)
                    continue
                if not shared.ring_push(0, TYPE_CAN, tx_timestamp, tx_payload,
                                        flags=tx_flags, sequence=tx_sequence):
                    shared.atomic_store64(FAULTS + 40,
                                          shared.atomic_load64(FAULTS + 40) + 1)

            sequence = (sequence + 1) & 0xFFFFFFFF
            cycles_this_report += 1
            deadline += period_ns
            remaining = deadline - time.monotonic_ns()
            if remaining > 0:
                time.sleep(remaining / 1_000_000_000.0)
            else:
                deadline = time.monotonic_ns()

            report_now = time.monotonic()
            if report_now - last_report >= 1.0:
                measured_hz = cycles_this_report / (report_now - last_report)
                print(f"sim measured_hz={measured_hz:.1f} target_hz={hz} "
                      f"pos={position:.3f} vel={velocity:.3f} "
                      f"faults=0x{flags:02x} can_rx={shared.atomic_load64(FAULTS + 16)}",
                      flush=True)
                cycles_this_report = 0
                last_report = report_now
    except KeyboardInterrupt:
        pass
    finally:
        shared.atomic_store64(SUPERBLOCK + 16, 0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shm", type=Path, default=Path(tempfile.gettempdir()) / "qnx_quiver_shm.bin")
    parser.add_argument("--hz", type=int, default=1000)
    parser.add_argument("--duration", type=float, help="stop after this many seconds (useful for smoke checks)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not 1 <= args.hz <= 10000:
        parser.error("--hz must be between 1 and 10000")
    if args.duration is not None and args.duration <= 0:
        parser.error("--duration must be greater than zero")
    shared = SharedMemory(args.shm)
    try:
        print(f"QNX-Quiver simulator mapped {args.shm} ({BAR2_BYTES} bytes) at {args.hz} Hz", flush=True)
        run_simulator(shared, args.hz, args.duration)
    finally:
        shared.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
