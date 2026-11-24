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
    parser.add_argument("--bus", help="QEMU bus for reinsertion (defaults to --bridge)")
    parser.add_argument("--address",
                        help="optional PCI address for reinsertion on --bus")
    parser.add_argument("--memdev", default="qnx_shm",
                        help="memory backend id passed to ivshmem-plain")
    parser.add_argument("--acpi-eject-ack", action="store_true",
                        help="write the i440fx ACPI eject acknowledgment via HMP")
    parser.add_argument("--acpi-eject-port", type=lambda value: int(value, 0),
                        default=0xAE08, help="ACPI PCI eject register I/O port")
    parser.add_argument("--acpi-eject-mask", type=lambda value: int(value, 0),
                        default=0x10,
                        help="32-bit eject bit mask for the device slot")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--hold-removed-seconds", type=float, default=3.0,
                        help="wait for QNX clients to withdraw before reinsertion")
    args = parser.parse_args()
    insert_bus = args.bus or args.bridge
    qmp = QMP(args.host, args.port, args.timeout)
    removed_successfully = False
    try:
        before = qmp.command("query-pci")
        if inventory_find_device(before, args.device) is None:
            raise RuntimeError(f"{args.device} is absent before test")
        qmp.command("device_del", {"id": args.device})
        if args.acpi_eject_ack:
            # This is a host-injected guest ACPI acknowledgment, not evidence
            # that QNX's PCI event handler processed the eject notification.
            qmp.command("human-monitor-command", {"command-line":
                f"o /w 0x{args.acpi_eject_port:x} 0x{args.acpi_eject_mask:x}"})
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
        add_arguments = {
            "driver": "ivshmem-plain",
            "memdev": args.memdev,
            "bus": insert_bus,
            "id": args.device,
        }
        if args.address is not None:
            add_arguments["addr"] = args.address
        qmp.command("device_add", add_arguments)
        added = qmp.command("query-pci")
        device = inventory_find_device(added, args.device)
        if device is None:
            raise RuntimeError("device_add returned but query-pci does not show ivshmem")
        regions = {region.get("bar"): region for region in device.get("regions", [])}
        bar0 = regions.get(0, {})
        bar2 = regions.get(2, {})
        if bar0.get("size") != 0x100 or bar2.get("size") != 0x400000:
            raise RuntimeError("reinserted ivshmem has unexpected BAR geometry")
        if bar0.get("address", -1) < 0 or bar2.get("address", -1) < 0:
            print("UNVERIFIED: QEMU reinserted ivshmem, but BAR0/BAR2 remain unassigned; "
                  "the guest PCI server did not restore its resources.", file=sys.stderr)
            return 2
        if args.acpi_eject_ack:
            print("PASS: QEMU removed and reinserted ivshmem with assigned BARs after "
                  "a host-injected ACPI eject acknowledgment.")
            print("This does not verify that QNX handled the eject notification.")
        else:
            print(f"PASS: QEMU ivshmem removed and reinserted on {insert_bus}.")
        print("Guest-side proof still requires /dev/quiver withdrawal and reacquisition checks.")
        return 0
    finally:
        if removed_successfully:
            try:
                current = qmp.command("query-pci")
                if inventory_find_device(current, args.device) is None:
                    add_arguments = {
                        "driver": "ivshmem-plain",
                        "memdev": args.memdev,
                        "bus": insert_bus,
                        "id": args.device,
                    }
                    if args.address is not None:
                        add_arguments["addr"] = args.address
                    qmp.command("device_add", add_arguments)
                    print("CLEANUP: attempted to restore ivshmem after an incomplete test.",
                          file=sys.stderr)
            except Exception as error:
                print(f"CLEANUP WARNING: could not restore ivshmem: {error}",
                      file=sys.stderr)
        qmp.close()


if __name__ == "__main__":
    raise SystemExit(main())
