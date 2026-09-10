#ifndef BENCHTEST_H
#define BENCHTEST_H

// Set to 1 to build the bench firmware, which differs from the shipping build in
// three ways so that a full spool and upload cycle can be watched in minutes
// instead of a day:
//
//   - a telemetry row every fifteen seconds rather than every ten minutes
//   - a sync every two minutes rather than once at midnight
//   - no actuator recalibration, so nothing mechanical runs on the bench
//
// Set it back to 0 before flashing the tracker for real.
#define BENCH_TEST 1

#endif
