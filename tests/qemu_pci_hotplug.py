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


def inventory_device_slot(pci, device_id):
    """Return the root PCI slot number used by QEMU's ACPI eject bitmap."""
    def on_bus(bus, root_slot=None):
        for device in bus.get("devices", []):
            slot = device.get("slot")
            if device.get("qdev_id") == device_id:
                return slot if root_slot is None else root_slot
            bridge = device.get("pci_bridge")
            if bridge:
                found = on_bus({"devices": bridge.get("devices", [])},
                               slot if root_slot is None else root_slot)
                if found is not None:
                    return found
        return None
    for bus in pci:
        found = on_bus(bus)
        if found is not None:
            return found
    return None


def inventory_root_slot(pci, device_id):
    """Return the slot only when the function is directly on the root bus."""
    for bus in pci:
        for device in bus.get("devices", []):
            if device.get("qdev_id") == device_id:
                return device.get("slot")
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
    parser.add_argument("--memdev", default="qnx_shm_reinsert",
                        help="fresh memory backend id for the reinserted device")
    parser.add_argument("--mem-path", required=True,
                        help="QEMU-host path to the shared 4 MiB BAR2 backing file")
    parser.add_argument("--mem-size", type=lambda value: int(value, 0),
                        default=0x400000,
                        help="shared memory backend size (default: 4 MiB)")
    parser.add_argument("--acpi-eject-ack", action="store_true",
                        help="write the i440fx ACPI eject acknowledgment via HMP")
    parser.add_argument("--acpi-eject-port", type=lambda value: int(value, 0),
                        default=0xAE08, help="ACPI PCI eject register I/O port")
    parser.add_argument("--acpi-eject-mask", type=lambda value: int(value, 0),
                        help="override eject mask; default derives it from QMP PCI slot")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--hold-removed-seconds", type=float, default=3.0,
                        help="wait for QNX clients to withdraw before reinsertion")
    args = parser.parse_args()
    insert_bus = args.bus or args.bridge
    qmp = QMP(args.host, args.port, args.timeout)
    removed_successfully = False
    backend_created = False
    try:
        before = qmp.command("query-pci")
        initial_device = inventory_find_device(before, args.device)
        if initial_device is None:
            raise RuntimeError(f"{args.device} is absent before test")
        if args.mem_size != 0x400000:
            raise RuntimeError("BAR2 backing file must be exactly 4 MiB")
        # Create the fresh backend before removal so a setup error cannot
        # strand the running guest without its original device.
        qmp.command("object-add", {
            "qom-type": "memory-backend-file",
            "id": args.memdev,
            "size": args.mem_size,
            "share": True,
            "mem-path": args.mem_path,
        })
        backend_created = True
        qmp.command("device_del", {"id": args.device})
        if args.acpi_eject_ack:
            # This is a host-injected guest ACPI acknowledgment, not evidence
            # that QNX's PCI event handler processed the eject notification.
            slot = inventory_device_slot(before, args.device)
            if args.acpi_eject_mask is None:
                if slot is None or slot < 0 or slot >= 32:
                    raise RuntimeError("cannot derive ACPI eject bit from PCI inventory")
                eject_mask = 1 << slot
            else:
                eject_mask = args.acpi_eject_mask
            qmp.command("human-monitor-command", {"command-line":
                f"o /w 0x{args.acpi_eject_port:x} 0x{eject_mask:x}"})
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
        # QEMU 10.2.1 can assert if ivshmem reuses the same memory backend for
        # another attachment. The fresh object above maps the same BAR2 file.
        add_arguments = {
            "driver": "ivshmem-plain",
            "memdev": args.memdev,
            "bus": insert_bus,
            "id": args.device,
        }
        if args.address is not None:
            add_arguments["addr"] = args.address
        elif insert_bus == "pci.0":
            root_slot = inventory_root_slot(before, args.device)
            if root_slot is not None:
                add_arguments["addr"] = f"0x{root_slot:x}"
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
        elif backend_created:
            try:
                qmp.command("object-del", {"id": args.memdev})
            except Exception as error:
                print(f"CLEANUP WARNING: could not remove unused memory backend: {error}",
                      file=sys.stderr)
        qmp.close()


if __name__ == "__main__":
    raise SystemExit(main())
