# Development supervisor

Build with `make supervisor` and run `build/quiver-supervisor [binary-directory]` in the guest. The default directory is `/opt/qnx-quiver/bin`. It starts the PCI manager first, waits for `/dev/quiver`, then starts the split CAN processes, IMU consumer, and trace writer. It restarts exited children with exponential backoff capped at 30 seconds and stops children on SIGINT/SIGTERM.

The IMU consumer is the sole IMU ring consumer in this group. Do not start `quiver-controller` alongside it: ring reads are work-shared rather than fanned out. The trace utility reads telemetry only and does not drain data rings.

This is a development process supervisor. Its children inherit the supervisor's security type, and a missing PCI device causes restart/backoff rather than a product-specific readiness notification. The production image must launch components under separate least-privilege types and add readiness, restart policy, and logging rules to its own service manager. The supervisor has been cross-compiled; QNX guest startup/failure/restart/shutdown behavior still requires runtime verification.
