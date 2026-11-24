# QNX-Quiver codelab

This walkthrough uses QNX SDP 8.0 x86_64, the supplied QNX guest image, and Ubuntu WSL QEMU 10.2.1. It is a software-only virtual platform. The latest source and host checks are recorded in `../docs/implementation-status.md`; protocol v3 must be used on both sides.

## 1. Build and run local checks

In a QNX SDP command prompt, from the project root:

```sh
make clean all
```

On the development host:

```sh
python -m unittest discover -s tests -v
python host/sim_engine/quiver_sim.py --self-test
python host/sim_engine/quiver_sim.py --duration 2 --latency-spike-us 500
```

The last run prints measured rate, p50/p95/p99/max wake lateness, and deadline misses. These measurements characterize the host scheduler; they are not real-time guarantees.

## 2. Boot QNX and the host plant

In Ubuntu WSL, use one terminal for QEMU and another for the simulator:

```sh
bash qemu/launch_qnx_quiver_wsl.sh
bash qemu/run_quiver_sim_wsl.sh
```

Both commands default to `build/qnx_quiver_shm_v3.bin`. Keep them in WSL: Windows and WSL did not observe coherent live writes through the same DrvFS file. QEMU uses `-snapshot`, so guest disk writes are discarded at exit.

In the QNX guest, verify that `pci-tool -v` lists `1af4:1110`, BAR0 is 256 bytes, and BAR2 is 4 MiB. Start the resource manager:

```sh
build/quiver-pci-rm
build/quiver-client-probe
```

Expected result: `/dev/quiver` is present and the API reports QVB1 v3. The client probe reads info, counters, and a coherent fault snapshot through devctl.

## 3. Run CAN and peripheral examples

Start CAN RX and TX as separate processes, then run the loopback probe:

```sh
build/dev-can-quiver rx &
build/dev-can-quiver tx &
build/quiver-can-probe
build/quiver-can-probe --status
```

The probe sends an extended 64-byte CAN-FD frame and checks the returned payload. For a bus-off transition, set `fault busoff` in the simulator console, query `build/quiver-can-probe --status`, clear the fault, and query again. The status generation should advance for each transition.

Run one IMU ring consumer at a time:

```sh
build/quiver-imu --stale-ms 100 --duration 5
build/quiver-controller
```

The controller consumes the same ring and sends bounded commands; it emits zero after 100 ms without a fresh sample. Send an individual actuator command with `build/quiver-actuator 0.25 100`; the tool waits for its command-ID acknowledgement. `build/quiver-trace /tmp/quiver-trace.csv --duration 5` records fault and telemetry snapshots without consuming ring records.

## 4. Verify removal and recovery in software

No physical PCI card is needed. Boot with the bridge topology:

```sh
QNX_PCI_TOPOLOGY=pci-bridge-hotplug QNX_IFS_IMAGE=/path/to/test-ifs.bin bash qemu/launch_qnx_quiver_wsl.sh
```

Create a throwaway test IFS with `qnx/pci_server_hotplug.cfg` embedded as `/proc/boot/pci_server.cfg`, and make the boot command start `pci-server --aspace-enable --config=/proc/boot/pci_server.cfg` before any PCI client. This config enables bus reconfiguration, so don't apply it by restarting PCI in a working non-snapshot system. Transfer the rebuilt `quiver-pci-rm` and `pci_hotplug_wait.sh` into the guest and start the manager. In one guest shell run `sh /tmp/pci_hotplug_wait.sh removed 30`. In WSL run `python3 tests/qemu_pci_hotplug.py --port 4443`; the port must match `QNX_QMP_PORT` if overridden. The harness holds the device absent for three seconds so the manager's health poll can withdraw `/dev/quiver`. When QMP completes reinsertion, run `sh /tmp/pci_hotplug_wait.sh reinserted 30` in the guest.

The test passes only when QMP receives `DEVICE_DELETED`, `query-pci` shows the function removed and reinserted, and QNX withdraws then republishes `/dev/quiver`. The default guest config did not complete removal in the attempted test: QMP accepted `device_del`, but the device remained enumerated. Load the PCI event handler and hotplug bus configuration modules before interpreting a run as successful. This still cannot prove electrical link loss on real hardware.

## 5. Security and cleanup

The policy fragment compiles, but the supplied guest has no active `/proc/boot/secpol.bin`. Merge it into the product policy, identify the exact PCI-server channel type with `secpolmonitor`, assign service types, and verify allowed and denied operations under enforcement before deployment.

Stop the simulator with `quit` or Ctrl+C; stop guest services with SIGTERM. Close QEMU after each run. The generated shared file contains protocol telemetry only and can be removed after all QEMU and simulator processes have exited.
