# QNX-Quiver policy integration and test

`qnx_quiver.policy` is an integration fragment, not a complete boot policy. It compiles with QNX SDP 8.0, but it cannot safely be pushed into the supplied guest by itself: the guest image has no product policy source, and enforcing a fragment would omit rules needed by the OS and PCI server.

## Build a disposable policy-enabled IFS

Start with the complete policy source used by the target image, before adding Quiver's fragment. It must declare the PCI server type used by that image. The IFS builder merges the Quiver fragment and adds the narrowly scoped Quiver-to-PCI-server channel permission for the configured PCI server type. If the supplied full policy already contains the Quiver fragment, pass `-PolicySourceIncludesQuiver` to avoid merging it twice.

```powershell
qemu\build_hotplug_test_ifs.ps1 `
  -ActivePolicySource C:\path\to\target-policy.policy `
  -PciServerType pci_server_t `
  -QuiverManagerType quiver_pci_t `
  -OutputIfs build\ifs-quiver-policy-test.bin
```

The builder compiles the combined policy as an immutable policy, includes it at `/proc/boot/secpol.bin`, pushes it before the persistent services start, and launches `pci-server` and the Quiver manager under their configured types. It rejects the supervisor profile because its children currently inherit one type. Provide the actual target policy source and PCI-server type; do not use a synthetic `unrestricted` policy as an enforcement test.

For a disposable policy probe image, the builder includes the positive `quiver-client-probe` and the negative `quiver-client-denied-probe`. After confirming `/dev/quiver` is online, run:

```sh
on -T quiver_can_client_t /proc/boot/quiver-client-probe
on -T quiver_denied_client_t /proc/boot/quiver-client-denied-probe
```

The second command passes only when `open("/dev/quiver", O_RDONLY)` fails with `EACCES` or `EPERM`; it fails if access succeeds or fails for an unrelated reason. The denied type is given only enough path and execute privilege to launch its probe. You can select a different test type with `-DeniedClientType` if the full policy source already declares the default name.

This builder is a Quiver policy test scaffold. It types the PCI server and Quiver manager; the base image's other persistent services still use their original startup commands. For a production acceptance image, update the full target startup so every long-lived service is launched with its own intended policy type and required user/group, then repeat the boot and service tests. A bootable, fully enforcing product image cannot be generated from the Quiver fragment alone.

The target image must already have valid rules for its boot services. Verify the resulting IFS contents before booting it, then boot only in QEMU snapshot mode or another disposable target. Check `pidin -f a_n` for the intended process types, inspect policy denials with `secpolmonitor`, and exercise the manager and client API. An allowed client must complete a read-only probe under its assigned client type. A deliberately unpermitted client type must be denied opening `/dev/quiver`; it must not be given `channel connect` permission. Also verify that permitted service types cannot map BAR2 or access private host-owned controls.

A disposable QEMU target policy derived via `secpolgenerate` is checked in at `qnx/security/qemu_target_base.policy` and has been boot-tested in snapshot mode on `pci-bridge-hotplug` with `pci-server` typed as `pci_server_t`, `quiver-pci-rm` as `quiver_pci_t`, and immutable enforcement (`secpol -Q` -> `immutable`) verified by both `quiver-client-probe` (`quiver_can_client_t`) and `quiver-client-denied-probe` (`quiver_denied_client_t` -> `EPERM`). For a production target image, replace `qemu_target_base.policy` with the target's own complete policy and type all remaining long-lived OS services.

## Develop or discover a target policy

On the disposable target, use QNX's `secpolgenerate` workflow to observe all normal boot and Quiver workloads, export the generated policy text, integrate the Quiver rules, and compile the resulting complete source with `secpolcompile`. `secpolgenerate` development mode is an observation phase, not enforcement. For the final test image, include the compiled policy at `/proc/boot/secpol.bin`, remove development generation/preload startup lines, run `secpolpush` early, and explicitly type every persistent service. Keep a recovery copy of the original IFS.

QNX references: [policy integration](https://qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/using_security_policies.html), [booting securely](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/boot_securely.html), and [secpolcompile](https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/secpolcompile.html).
