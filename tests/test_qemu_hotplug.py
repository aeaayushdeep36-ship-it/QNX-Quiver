import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_pci_hotplug as hotplug


class QemuHotplugInventoryTests(unittest.TestCase):
    def test_root_bus_slot_for_acpi_eject_bitmap(self):
        inventory = [{"bus": 0, "devices": [
            {"slot": 3, "qdev_id": "quiver_ivshmem"},
        ]}]
        self.assertEqual(hotplug.inventory_device_slot(
            inventory, "quiver_ivshmem"), 3)
        self.assertEqual(hotplug.inventory_root_slot(
            inventory, "quiver_ivshmem"), 3)

    def test_bridge_device_uses_root_slot_for_acpi_eject_bitmap(self):
        inventory = [{"bus": 0, "devices": [
            {"slot": 4, "qdev_id": "bridge0", "pci_bridge": {"devices": [
                {"slot": 0, "qdev_id": "quiver_ivshmem"},
            ]}},
        ]}]
        self.assertEqual(hotplug.inventory_device_slot(
            inventory, "quiver_ivshmem"), 4)
        self.assertIsNone(hotplug.inventory_root_slot(
            inventory, "quiver_ivshmem"))

    def test_unknown_device_has_no_eject_slot(self):
        self.assertIsNone(hotplug.inventory_device_slot([], "missing"))


if __name__ == "__main__":
    unittest.main()
