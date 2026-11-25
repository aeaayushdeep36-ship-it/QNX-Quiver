# QNX-Quiver Handoff

Updated: 25 November 2026

This document is for the next engineer taking over QNX-Quiver. It records the local toolchain and VM locations, how to build and enter the QNX guest, what has evidence behind it, and what is still open.

## 1. Project summary

QNX-Quiver is a QNX SDP 8.0 x86_64 virtual embedded platform. QEMU exposes a shared-memory PCI device (`ivshmem-plain`) whose BAR2 contains the QVB1 ring protocol. A QNX PCI resource manager owns and maps BAR2 privately and exposes a validated client API at `/dev/quiver`. A host-side plant simulator exchanges CAN FD, IMU, and actuator records through the same shared file.

The project is not a direct BAR-mapping library for applications. The PCI resource manager is the only component that owns/maps BAR2; QNX clients use the `/dev/quiver` devctl interface.

## 2. Important locations and software

| Item | Location / version |
|---|---|
| Project folder | `D:\Freelance\QNX Quiver` |
| Project folder in WSL Ubuntu | `/mnt/d/Freelance/QNX Quiver` |
| QNX SDP 8.0 | `C:\Users\Lenovo\qnx800` |
| QNX host tools | `C:\Users\Lenovo\qnx800\host\win64\x86_64` |
| QNX target tree | `C:\Users\Lenovo\qnx800\target\qnx` |
| QNX environment batch file | `C:\Users\Lenovo\qnx800\qnxsdp-env.bat` |
| Supplied QNX VM | `C:\Users\Lenovo\qnxvm` |
| Original guest IFS | `C:\Users\Lenovo\qnxvm\output\ifs.bin` |
| Guest virtual disk | `C:\Users\Lenovo\qnxvm\output\disk-qemu.vmdk` |
| Supported QEMU for ivshmem | Ubuntu WSL QEMU 10.2.1; executable `qemu-system-x86_64` |
| Windows QEMU | 11.1.0 installed, but this build lacks `ivshmem-plain`; do not use it for the Quiver guest |
| Git remote | `https://github.com/aeaayushdeep36-ship-it/QNX-Quiver.git` |

The QNX environment targets x86_64 with `qcc -Vgcc_ntox86_64`. Build flags include `-Wall -Wextra -Werror`.

## 3. Start QEMU and access QNX

### Open the supported QEMU environment

From Windows PowerShell:

```powershell
wsl.exe -d Ubuntu
```

In the Ubuntu shell:

```sh
cd "/mnt/d/Freelance/QNX Quiver"
qemu-system-x86_64 --version
```

Confirm it reports QEMU 10.2.1. The launch script checks for `ivshmem-plain` and uses the QNX VM disk in snapshot mode, so guest writes do not persist to the original disk.

### Build a disposable QNX test IFS

From Windows PowerShell in the project folder:

```powershell
cd 'D:\Freelance\QNX Quiver'
.\qemu\build_hotplug_test_ifs.ps1 -IncludeServices -OutputIfs build\ifs-quiver-services.bin
```

For a test image where the supervisor owns the PCI manager lifecycle:

```powershell
.\qemu\build_hotplug_test_ifs.ps1 -IncludeServices -SupervisorMode -OutputIfs build\ifs-quiver-supervisor.bin
```

The builder uses QNX SDP 8.0 tools from the installed `qnx800` tree and the original IFS as its base. It copies the result to the requested output path. Keep this output separate from `C:\Users\Lenovo\qnxvm\output\ifs.bin`.

### Launch the guest

In WSL Ubuntu, set the paths explicitly when using a project-built IFS:

```sh
cd "/mnt/d/Freelance/QNX Quiver"
export QNX_VM_ROOT=/mnt/c/Users/Lenovo/qnxvm
export QNX_DISK_IMAGE=/mnt/c/Users/Lenovo/qnxvm/output/disk-qemu.vmdk
export QNX_IFS_IMAGE="/mnt/d/Freelance/QNX Quiver/build/ifs-quiver-services.bin"
export QNX_QUIVER_SHM="/mnt/d/Freelance/QNX Quiver/build/qnx_quiver_shm_v3.bin"
export QNX_PCI_TOPOLOGY=pci-bridge-hotplug
bash qemu/launch_qnx_quiver_wsl.sh
```

The `pci-bridge-hotplug` profile retains the known-booting i440fx/EIDE disk topology and places ivshmem behind a PCI bridge. The QNX console is the QEMU serial terminal in this mode. Exit QEMU with `Ctrl+A`, then `X`.

The launcher defaults to the original IFS if `QNX_IFS_IMAGE` is not set. The project-built IFS is required for the Quiver manager, services, supervisor, or PCI diagnostic utilities.

### Start the host simulator

Use a second WSL Ubuntu terminal and the exact same shared-memory file path as QEMU:

```sh
cd "/mnt/d/Freelance/QNX Quiver"
export QNX_QUIVER_SHM="/mnt/d/Freelance/QNX Quiver/build/qnx_quiver_shm_v3.bin"
bash qemu/run_quiver_sim_wsl.sh
```

Both QEMU and the simulator must run inside WSL. Windows and WSL views of the same DrvFS backing file did not share live writes reliably. The file must be exactly 4 MiB and initialized for protocol v3; the simulator rejects an unknown nonzero protocol image rather than overwriting it.

### QNX network access and credentials

The launcher forwards host port `2223` to guest port 22 by default; override with `QNX_SSH_PORT`. If the guest networking and `sshd` are up, connect from WSL with an authorized QNX guest account, for example:

```sh
ssh -p 2223 <authorized-user>@127.0.0.1
```

No VM-specific SSH private key or password is included in this project handoff. Do not assume the unrelated keys in the Windows `.ssh` directory are valid for this VM. If SSH is unavailable, use the QEMU serial console to work in the guest. The guest's `vtnet0` startup has failed in some prior serial output, so serial access is the reliable fallback until networking is confirmed.

The launcher exposes QMP on `127.0.0.1:4443` by default (`QNX_QMP_PORT`). Use the QMP test harness rather than issuing guessed device commands. When `QUIVER_HEADLESS=1`, the HMP monitor is also exposed on `127.0.0.1:4442`; serial output is written to `build/qemu_serial.log` by default. QMP acceptance is not proof of QNX guest recovery.

## 4. Build and test commands

### QNX target build

Use the QNX command prompt/environment, then from the project root:

```sh
make clean all
```

The IFS builder can also invoke the installed QNX tools directly. If building manually in PowerShell, set:

```powershell
$env:QNX_HOST = 'C:\Users\Lenovo\qnx800\host\win64\x86_64'
$env:QNX_TARGET = 'C:\Users\Lenovo\qnx800\target\qnx'
$env:PROCESSOR = 'x86_64'
```

### Host protocol tests

With Python 3 available:

```sh
python -m unittest discover -s tests -v
python host/sim_engine/quiver_sim.py --self-test
```

At handoff, 13 host unit tests passed and the simulator self-test passed. These do not replace QNX guest tests.

### QNX service checks

In the normal services IFS, `quiver-pci-rm` is inserted into startup and publishes `/dev/quiver` after the PCI server and device are ready. To run services manually in the guest:

```sh
/proc/boot/dev-can-quiver rx &
/proc/boot/dev-can-quiver tx &
/proc/boot/quiver-can-probe
/proc/boot/quiver-can-probe --status
```

CAN RX and TX must be separate processes with this guest's libcan behavior. Run one IMU ring consumer at a time: `quiver-imu` or `quiver-controller`. `quiver-supervisor` starts the manager and dependent services when using a `-SupervisorMode` image.

Useful guest diagnostics include:

```sh
ls -l /dev/quiver /dev/can0
/proc/boot/quiver-client-probe
/proc/boot/quiver-pci-db-probe
pci-tool -D 0x1110 --strip
slog2info | grep -i -E 'pci|quiver|buscfg|attach|bar'
pidin ar
```

Use `/proc/boot/...` paths for IFS components. The PCI resource manager owns BAR2; clients should test through `/dev/quiver`, never map BAR2 directly.

## 5. PCI hotplug and recovery findings

### Verified

- The one-shot PCI fault was traced to missing `ThreadCtl(_NTO_TCTL_IO, 0)` before libpci I/O/mapping calls. BAR reads use `pci_device_read_ba()`, not `pci_device_cfg_rd32()` at BAR config offset `0x10`.
- The resident PCI manager verifies QVB1 v3, owns BAR2, publishes `/dev/quiver`, and withdraws it on loss. It checks PCI server identity on a timer. On PCI-server restart, it re-executes itself to reinitialize libpci state and republishes the node.
- PCI-server restart recovery passed on the controlled Q35 bridge setup; `/dev/quiver`, client probes, and supervised services recovered.
- QNX CAN FD loopback, including extended IDs and 64-byte payloads, plus IMU, actuator, trace, controller fail-safe, supervisor restart/shutdown, and host protocol tests have prior disposable guest evidence. See `docs/implementation-status.md` for exact limits.

### Not verified / known limits

- Native guest-visible PCI hot-unplug and reinsert with BAR reassignment has **not** passed. In the tested layouts, QEMU can accept `device_del` without completing removal, or QNX sees a newly inserted device in QEMU but leaves it out of its PCI database with BARs unassigned.
- QNX's event handler reported unsupported link events on the tested bridge. Q35 root-port experiments previously faulted the supplied PCI server. A `pcie-root-port` launcher option is available as an experimental candidate, not a validated fix.
- Restarting `pci-server` with ivshmem present does re-enumerate and assign the BARs on the verified Q35 bridge setup. This is the supported software recovery path currently evidenced; it is not native hotplug.
- A prior i440fx PCI-server restart caused a QEMU 10.2.1 assertion. Prefer the tested Q35 bridge topology for restart-recovery experiments.
- Physical hardware is not inherently required for virtual hotplug testing. A topology with QNX-supported event delivery and a stable BAR-reassignment path is still needed.

### QMP lifecycle test

From WSL, with the guest running and QMP on port 4443:

```sh
cd "/mnt/d/Freelance/QNX Quiver"
python3 tests/qemu_pci_hotplug.py \
  --port 4443 \
  --bus quiver_bridge \
  --memdev qnx_shm_reinsert \
  --mem-path '/mnt/d/Freelance/QNX Quiver/build/qnx_quiver_shm_v3.bin'
```

For a direct root-bus diagnostic, use `--bus pci.0`. `--acpi-eject-ack` injects an eject acknowledgment from the host and is diagnostic only; it does not prove QNX processed the event. The harness discovers the slot from live QMP inventory, validates BAR sizes/addresses, and reports incomplete cycles as `UNVERIFIED`. Use a new memory-backend ID for each reinsert while mapping the same shared file; QEMU 10.2.1 can assert if an attached backend object is reused.

The guest helpers `/proc/boot/pci_hotplug_cycle.sh` and `/proc/boot/pci_hotplug_wait.sh` check `/dev/quiver` withdrawal and return. Acceptance requires the complete guest-visible cycle plus a client operation succeeding afterward.

## 6. Production security policy

`qnx/security/qnx_quiver.policy` is the Quiver integration fragment, and `qnx/security/qemu_target_base.policy` is a `secpolgenerate`-derived target policy for the disposable QEMU guest. Immutable policy enforcement has been boot-tested and verified in QEMU 10.2.1 snapshot mode (`pci-bridge-hotplug`):

```powershell
.\qemu\build_hotplug_test_ifs.ps1 `
  -IncludeServices `
  -SupervisorMode `
  -ActivePolicySource qnx\security\qemu_target_base.policy `
  -PciServerType pci_server_t `
  -QuiverManagerType quiver_pci_t `
  -OutputIfs build\ifs-quiver-supervisor-policy.bin
```

The builder compiles an immutable policy, embeds `/proc/boot/secpol.bin`, pushes it early via `/proc/boot/bin/secpolpush`, types `pci-server` (`pci_server_t`) and `quiver-pci-rm` (`quiver_pci_t`) or `quiver-supervisor` (`quiver_supervisor_t`), and packages positive and negative client probes. When `quiver-supervisor` is used under an active policy, it spawns its children under `quiver_pci_t`, `quiver_can_t`, `quiver_imu_t`, and `quiver_trace_t` via `secpol_posix_spawnattr_settypeid()`.

In the disposable QEMU snapshot tests (`build/ifs-quiver-policy-test.bin` and `build/ifs-quiver-supervisor-policy.bin`), `secpol -Q` reported `immutable`, `pidin -f a_n` confirmed distinct types for `pci-server` and all Quiver services, `quiver-client-probe` (`quiver_can_client_t`) and `quiver-actuator` (`quiver_actuator_t`) passed with live simulator acknowledgement, and `quiver-client-denied-probe` (`quiver_denied_client_t`) passed with `EPERM`. For a production target image, replace `qemu_target_base.policy` with the target's complete active policy and type all remaining long-lived OS services.

See `qnx/security/README.md` for the policy workflow and official QNX references.

## 7. Remaining open items

1. **Native PCI hotplug on QEMU 10.2.1**: Diagnostic testing of `QNX_PCI_TOPOLOGY=pcie-root-port` (`build/ifs-pci-only-diag.bin`) showed `pci-server` v3.2 enumerating `1af4:1110` and then terminating after `initialize_slot_event_notifications()` / `_cap_msix_get_nirq()` on `pcie-root-port` (`B0:D2:F0`). Native hot-unplug/reinsert remains unsupported on the tested QNX 8.0 / QEMU 10.2.1 topologies; `pci-server` restart recovery (`quiver-pci-rm` self-re-exec) is the verified software recovery path.
2. **Production target policy**: Integrate `qnx/security/qnx_quiver.policy` with the production target's complete active policy and type all non-Quiver OS services in startup before production deployment.

## 8. Key project files

- `README.md` — project overview and quick start.
- `docs/implementation-status.md` — source, host, and guest verification status.
- `docs/protocol.md` — QVB1 ring protocol and ownership rules.
- `docs/environment.md` — local toolchain and VM setup.
- `docs/pci-service-investigation.md` — original PCI fault cause and correction.
- `qemu/launch_qnx_quiver_wsl.sh` — WSL QEMU launch, topology, QMP, and ports.
- `qemu/build_hotplug_test_ifs.ps1` — disposable QNX IFS and policy-test builder.
- `qemu/run_quiver_sim_wsl.sh` — host simulator using the shared file.
- `tests/README.md` — host, guest, and QMP test descriptions.
- `tests/qemu_pci_hotplug.py` — QMP device lifecycle test.
- `qnx/dev-quiver/` — PCI manager, client API, and probes.
- `qnx/dev-can-quiver/` — CAN DDK frontend and probes.
- `qnx/services/` — IMU, actuator, trace, and controller examples.
- `qnx/supervisor/` — development service supervisor.
- `qnx/security/README.md` — policy integration workflow.
