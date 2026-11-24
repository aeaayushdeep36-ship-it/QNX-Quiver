# QNX-Quiver codelab

This walkthrough targets QNX SDP 8.0 x86_64 with Ubuntu WSL QEMU 10.2.1. It uses a disposable QNX test IFS and a snapshot disk, so guest filesystem changes are discarded when QEMU exits. Protocol v3 is required on both sides.

## 1. Build and validate

From a QNX SDP command prompt at the project root:

```sh
make clean all
```

Build a disposable IFS containing the v3 client and service utilities from PowerShell:

```powershell
.\qemu\build_hotplug_test_ifs.ps1 -IncludeServices -OutputIfs .\build\ifs-v3-services-test.bin
```

For the supervisor exercise, build a second disposable image that leaves manager startup to the supervisor:

```powershell
.\qemu\build_hotplug_test_ifs.ps1 -IncludeServices -SupervisorMode -OutputIfs .\build\ifs-v3-supervisor-test.bin
```

Run the host checks:

```sh
python -m unittest discover -s tests -v
python host/sim_engine/quiver_sim.py --self-test
python host/sim_engine/quiver_sim.py --duration 2 --latency-spike-us 500
```

Timing output characterizes the host scheduler; it is not a real-time guarantee.

## 2. Boot the guest and simulator

In separate Ubuntu WSL terminals, start the QNX guest and host plant:

```sh
QNX_IFS_IMAGE="/mnt/d/Freelance/QNX Quiver/build/ifs-v3-services-test.bin" \
  QUIVER_HEADLESS=1 QNX_SERIAL_BACKEND=stdio \
  bash qemu/launch_qnx_quiver_wsl.sh
```

```sh
bash qemu/run_quiver_sim_wsl.sh
```

Keep both processes in WSL. They map the same v3 BAR2 file. QEMU uses `-snapshot`, so the VM disk is not modified by the session. The test IFS starts `pci-server` and the Quiver manager; its serial shell can run the embedded probes under `/proc/boot`.

Run the protocol and ownership probes:

```sh
/proc/boot/quiver-client-probe
/proc/boot/quiver-pci-db-probe
```

Expect QVB1 v3 geometry/counters and an exclusive-attach error from the PCI database probe because the manager owns the device.

## 3. Run the CAN and peripheral checks

Start RX and TX separately, then test CAN-FD echo and endpoint state:

```sh
/proc/boot/dev-can-quiver rx &
/proc/boot/dev-can-quiver tx &
/proc/boot/quiver-can-probe
/proc/boot/quiver-can-probe --status
```

The probe must echo the extended ID and all 64 data bytes. To exercise bus-off, set the fault through another WSL shell while QEMU and the simulator are running:

```sh
python3 - <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "/mnt/d/Freelance/QNX Quiver/host/sim_engine")
import quiver_sim
shared = quiver_sim.SharedMemory(Path("/mnt/d/Freelance/QNX Quiver/build/qnx_quiver_shm_v3.bin"))
shared.set_faults(quiver_sim.FAULT_CAN_BUSOFF)
shared.close()
PY
```

Query `/proc/boot/quiver-can-probe --status` and expect bus-off with an increased transition generation. Repeat the snippet with `shared.set_faults(0)` and verify active state and another generation increment. For the controller stale-data check below, change the fault argument to `shared.set_faults(quiver_sim.FAULT_IMU_STUCK)`.

Run peripheral examples in the guest:

```sh
/proc/boot/quiver-imu --duration 2
/proc/boot/quiver-actuator 0.25 100
/proc/boot/quiver-actuator 2 100   # expected local bounds rejection
/proc/boot/quiver-trace /tmp/quiver-trace.csv --duration 1
```

Verify actuator acknowledgement and expiry to zero in `/proc/boot/quiver-client-probe`, and confirm the trace CSV has a header and records. To check controller stale-data handling, start `/proc/boot/quiver-controller` in the guest, set `FAULT_IMU_STUCK` with the WSL snippet, and verify the controller logs stale input and sends zero. Clear all faults with `shared.set_faults(0)` and stop the controller with SIGTERM.

## 4. Verify supervisor restart and shutdown

Boot the separate `ifs-v3-supervisor-test.bin` snapshot and start the host simulator. In the guest, start `/proc/boot/quiver-supervisor /proc/boot`. Confirm `/dev/quiver`, `/dev/can0`, and the supervised service processes appear. Record the trace process PID, send it SIGTERM, and confirm a replacement PID appears. Then send SIGTERM to the supervisor and verify the manager and service processes exit and `/dev/quiver` is absent.

## 5. Check virtual PCI removal and reinsertion

Actual PCI hot-unplug does not need physical hardware. The current QNX 8.0 + QEMU 10.2.1 setup has not passed a full cycle. For a root-bus diagnostic, use a guest with the hotplug PCI server config, leave the test manager online, and run in WSL:

```sh
python3 tests/qemu_pci_hotplug.py --port 4443 --bus pci.0 \
  --memdev qnx_shm_reinsert \
  --mem-path "/mnt/d/Freelance/QNX Quiver/build/qnx_quiver_shm_v3.bin" \
  --acpi-eject-ack
```

The harness derives the ACPI eject bit from live inventory and creates a fresh shared memory backend before removal. The injected ACPI acknowledgment is host-generated diagnostic behavior, not proof of QNX's native event handling. Current evidence is that QEMU removes the device, QNX withdraws `/dev/quiver`, but reinserted BAR0/BAR2 remain unassigned and the manager stays offline. Treat `UNVERIFIED` as a failed acceptance gate. A complete pass requires QNX-observed removal, successful BAR reconfiguration after insertion, and `/dev/quiver` republication.

## 6. Security and cleanup

The policy fragment compiles but is not active in the supplied guest. Product integration must merge it into the target image policy, determine the PCI-server process type, apply distinct service types, and test allowed and denied API access under enforcement. Physical CAN arbitration and electrical behavior are outside this software-only test.

Stop the simulator with Ctrl+C. Exit QEMU with Ctrl+A then X, or Ctrl+C in its PTY. Check that both WSL processes have exited before reusing or removing the shared backing file.
