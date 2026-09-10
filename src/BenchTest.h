#ifndef BENCHTEST_H
#define BENCHTEST_H

// Set to 1 to build the bench firmware, which differs from the shipping build in
// three ways so that a full spool and upload cycle can be watched in minutes
// instead of a day:
//
//   - a telemetry row every fifteen seconds rather than every ten minutes
//   - a sync every two minutes rather than once at midnight
//   - no actuator movement at all, so nothing mechanical runs on the bench. This
//     matters for more than the noise: homing and maxing take about ninety
//     seconds, they run inside initializeStatus() during setup(), and because the
//     actuator calls loop() back while it moves, setup() never finishes and the
//     access point is never started.
//
// Set it back to 0 before flashing the tracker for real.
#define BENCH_TEST 0

#endif
