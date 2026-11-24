# QNX-Quiver controller demonstration

The compiled `qnx/services/controller_demo.c` is a small proportional-control example using the `/dev/quiver` API. It consumes six-float IMU records, clamps actuator output to `[-1,1]`, sends commands with a 100 ms TTL, and publishes a zero command after 100 ms without a new sample. It never maps BAR2 directly.

Run it only when `quiver-imu` is stopped because the IMU ring has one work-shared consumer, not per-client copies. The host simulator acknowledges command IDs and resets the virtual output to zero when a TTL expires. This is a stale-data/fail-safe demonstration, not a validated vehicle controller or certified actuator safe state. It has been cross-compiled and host behavior is unit-tested; the complete QNX controller path remains to be exercised in the guest.
