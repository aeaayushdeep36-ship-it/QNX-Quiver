#!/usr/bin/env python3
"""Exercise QEMU ivshmem removal/reinsert and verify PCI inventory changes."""

import argparse
import json
import select
import socket
import sys
import time


class QMP:
    def __init__(self, host: str, port: int, timeout: float):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.socket.settimeout(None)
        self.command_timeout = timeout
        self.file = self.socket.makefile("rwb", buffering=0)
        self.events = []
        greeting = self._read()
        if "QMP" not in greeting:
            raise RuntimeError(f"unexpected QMP greeting: {greeting}")
        self.command("qmp_capabilities")

    def _read(self, timeout=None):
        if timeout is not None:
            readable, _, _ = select.select([self.socket], [], [], timeout)
            if not readable:
                raise TimeoutError("QMP response timed out")
        line = self.file.readline()
        if not line:
            raise RuntimeError("QMP connection closed")
        return json.loads(line)

    def command(self, name, arguments=None):
        request = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.file.write((json.dumps(request) + "\r\n").encode())
        while True:
            response = self._read(self.command_timeout)
            if "event" in response:
                self.events.append(response)
                continue
            if "return" in response:
                return response["return"]
            if "error" in response:
                raise RuntimeError(f"QMP {name} failed: {response['error']}")

    def wait_event(self, event, device_id, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, message in enumerate(self.events):
                if (message.get("event") == event and
                        message.get("data", {}).get("device") == device_id):
                    del self.events[index]
                    return True
            try:
                message = self._read(max(0.0, deadline - time.monotonic()))
            except TimeoutError:
                break
            if (message.get("event") == event and
                    message.get("data", {}).get("device") == device_id):
                return True
            if "event" in message:
                self.events.append(message)
        return False

    def close(self):
        self.file.close()
        self.socket.close()


def inventory_find_device(pci, device_id):
    def on_bus(bus):
        for device in bus.get("devices", []):
            if device.get("qdev_id") == device_id:
                return device
            bridge = device.get("pci_bridge")
            if bridge:
                found = on_bus({"devices": bridge.get("devices", [])})
                if found is not None:
                    return found
        return None
    for bus in pci:
        found = on_bus(bus)
        if found is not None:
            return found
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4443)
    parser.add_argument("--device", default="quiver_ivshmem")
    parser.add_argument("--bridge", default="quiver_bridge")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--hold-removed-seconds", type=float, default=3.0,
                        help="wait for QNX clients to withdraw before reinsertion")
    args = parser.parse_args()
    qmp = QMP(args.host, args.port, args.timeout)
    removed_successfully = False
    try:
        before = qmp.command("query-pci")
        if inventory_find_device(before, args.device) is None:
            raise RuntimeError(f"{args.device} is absent before test")
        qmp.command("device_del", {"id": args.device})
        if not qmp.wait_event("DEVICE_DELETED", args.device, args.timeout):
            after = qmp.command("query-pci")
            if inventory_find_device(after, args.device) is not None:
                print("UNVERIFIED: QMP accepted device_del, but ivshmem remains in query-pci; "
                      "the QNX PCI hotplug event path did not complete removal.", file=sys.stderr)
                return 2
            raise RuntimeError("device disappeared without a matching DEVICE_DELETED event")
        removed = qmp.command("query-pci")
        if inventory_find_device(removed, args.device) is not None:
            raise RuntimeError("DEVICE_DELETED was reported but the device remains enumerated")
        removed_successfully = True
        if args.hold_removed_seconds > 0:
            time.sleep(args.hold_removed_seconds)
        qmp.command("device_add", {
            "driver": "ivshmem-plain",
            "memdev": "qnx_shm",
            "bus": args.bridge,
            "id": args.device,
        })
        added = qmp.command("query-pci")
        device = inventory_find_device(added, args.device)
        if device is None:
            raise RuntimeError("device_add returned but query-pci does not show ivshmem")
        if (device.get("bus") != 1 or
                not any(region.get("bar") == 2 and region.get("size") == 0x400000
                        for region in device.get("regions", []))):
            raise RuntimeError("reinserted ivshmem has unexpected bus or BAR2 geometry")
        print("PASS: QEMU ivshmem removed and reinserted behind the PCI bridge.")
        print("Guest-side proof still requires /dev/quiver withdrawal and reacquisition checks.")
        return 0
    finally:
        if removed_successfully:
            try:
                current = qmp.command("query-pci")
                if inventory_find_device(current, args.device) is None:
                    qmp.command("device_add", {
                        "driver": "ivshmem-plain",
                        "memdev": "qnx_shm",
                        "bus": args.bridge,
                        "id": args.device,
                    })
                    print("CLEANUP: attempted to restore ivshmem after an incomplete test.",
                          file=sys.stderr)
            except Exception as error:
                print(f"CLEANUP WARNING: could not restore ivshmem: {error}",
                      file=sys.stderr)
        qmp.close()


if __name__ == "__main__":
    raise SystemExit(main())
