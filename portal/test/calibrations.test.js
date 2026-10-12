import test from "node:test";
import assert from "node:assert/strict";

import { calibrationCount, createCalibrations, deriveEvents } from "../src/calibrations.js";
import { createMemorySyncLog } from "../src/syncs.js";

const SECOND = 1000;
const MINUTE = 60 * SECOND;
const HOUR = 60 * MINUTE;
const DAY = 24 * HOUR;
const NOW = Date.parse("2026-10-12T22:00:00.000Z");
const at = (iso) => Date.parse(iso);

// A clock sync as Cloud Run's request log has it.
const URL_BASE = "https://test-image-188091506191.us-central1.run.app/time/est?fw=v12.10";
const sync = (t, cal) => ({
  t: typeof t === "string" ? at(t) : t,
  url: cal === undefined ? URL_BASE : `${URL_BASE}&cal=${cal}&pos=0`,
});
const readable = (events) => events.map(([t, count]) => [new Date(t).toISOString(), count]);
const ALL = [0, Number.MAX_SAFE_INTEGER];

// A log that records what is asked of it, to check the caching.
function counting(log) {
  const calls = [];
  return {
    calls,
    async list(fromMs, toMs) {
      calls.push([fromMs, toMs]);
      return log.list(fromMs, toMs);
    },
  };
}

test("cal is read from the sync's URL, and only a whole number counts", () => {
  assert.equal(calibrationCount(sync(NOW, 0).url), 0);
  assert.equal(calibrationCount(sync(NOW, 3).url), 3);
  assert.equal(calibrationCount(`${URL_BASE}&cal=12`), 12);
  assert.equal(calibrationCount("/time/est?cal=2&pos=1"), 2);

  for (const url of [
    URL_BASE, // before v12.6
    `${URL_BASE}&cal=`,
    `${URL_BASE}&cal=-1`,
    `${URL_BASE}&cal=1.5`,
    `${URL_BASE}&cal=abc`,
    `${URL_BASE}&cal=%201`,
    `${URL_BASE}&cal=1e3`,
    `${URL_BASE}&cal=0x1`,
    `${URL_BASE}&cal=99999999999999999999`,
    `${URL_BASE}&local=1`,
    undefined,
    null,
    42,
  ]) {
    assert.equal(calibrationCount(url), null, String(url));
  }
});

test("a rising count is that many recalibrations, halfway between the two syncs", () => {
  const syncs = [
    sync("2026-10-12T17:30:00Z", 0),
    sync("2026-10-12T17:45:00Z", 0),
    sync("2026-10-12T18:00:00Z", 1),
    sync("2026-10-12T18:15:00Z", 1),
    sync("2026-10-12T18:30:00Z", 3),
  ];
  assert.deepEqual(readable(deriveEvents(syncs, ...ALL)), [
    ["2026-10-12T17:52:30.000Z", 1],
    ["2026-10-12T18:22:30.000Z", 2],
  ]);

  // The real ones, to the millisecond: the boot calibration after v12.10 went on.
  const real = [sync("2026-10-12T17:45:46.906Z", 0), sync("2026-10-12T18:00:37.573Z", 1)];
  assert.deepEqual(readable(deriveEvents(real, ...ALL)), [["2026-10-12T17:53:12.240Z", 1]]);
});

test("a count that drops is a reboot, and the new boot's count is what it did", () => {
  const syncs = [
    sync("2026-10-12T18:00:00Z", 2),
    sync("2026-10-12T18:15:00Z", 1), // rebooted and calibrated once
    sync("2026-10-12T18:30:00Z", 0), // rebooted and has not calibrated: nothing
    sync("2026-10-12T18:45:00Z", 0),
    sync("2026-10-12T19:00:00Z", 1),
  ];
  assert.deepEqual(readable(deriveEvents(syncs, ...ALL)), [
    ["2026-10-12T18:07:30.000Z", 1],
    ["2026-10-12T18:52:30.000Z", 1],
  ]);

  // The blind spot: a reboot that is back to the same count by the next sync is
  // not seen, and one past it counts only the difference.
  assert.deepEqual(deriveEvents([sync("2026-10-12T18:00:00Z", 1), sync("2026-10-12T18:15:00Z", 1)], ...ALL), []);
  assert.deepEqual(
    readable(deriveEvents([sync("2026-10-12T18:00:00Z", 1), sync("2026-10-12T18:15:00Z", 3)], ...ALL)),
    [["2026-10-12T18:07:30.000Z", 2]],
  );
});

test("syncs without a usable cal are skipped, and the ones either side compared", () => {
  const syncs = [
    sync("2026-10-12T16:30:00Z"), // v12.5, before cal existed
    sync("2026-10-12T16:45:00Z", 0),
    sync("2026-10-12T17:00:00Z"),
    sync("2026-10-12T17:15:00Z", "x"),
    sync("2026-10-12T17:30:00Z", -1),
    sync("2026-10-12T17:45:00Z", 1),
  ];
  assert.deepEqual(readable(deriveEvents(syncs, ...ALL)), [["2026-10-12T17:15:00.000Z", 1]]);
  assert.deepEqual(deriveEvents([sync("2026-10-12T16:30:00Z"), sync("2026-10-12T16:45:00Z", 2)], ...ALL), []);
  assert.deepEqual(deriveEvents([], ...ALL), []);
});

test("only events inside the window are kept, both ends included", () => {
  const syncs = [
    sync("2026-10-12T17:45:00Z", 0),
    sync("2026-10-12T17:55:00Z", 1), // 17:50, before
    sync("2026-10-12T18:05:00Z", 2), // 18:00, the start
    sync("2026-10-12T18:30:00Z", 2),
    sync("2026-10-12T18:55:00Z", 2),
    sync("2026-10-12T19:05:00Z", 3), // 19:00, the end
    sync("2026-10-12T19:15:00Z", 4), // 19:10, after
  ];
  assert.deepEqual(readable(deriveEvents(syncs, at("2026-10-12T18:00:00Z"), at("2026-10-12T19:00:00Z"))), [
    ["2026-10-12T18:00:00.000Z", 1],
    ["2026-10-12T19:00:00.000Z", 1],
  ]);
});

test("the first sync in the window is compared with the last before it, up to a day back", async () => {
  const since = NOW - DAY;
  const log = counting(createMemorySyncLog([sync(since - 5 * MINUTE, 0), sync(since + 10 * MINUTE, 1)]));
  const calibrations = createCalibrations({ log, now: () => NOW });
  assert.deepEqual(await calibrations.read(since, NOW), [[since + 2.5 * MINUTE, 1]]);
  assert.deepEqual(log.calls, [[since - DAY, NOW]]);

  // A predecessor more than a day before the window is not read, so the first sync
  // in it has nothing to compare with.
  const sparse = createMemorySyncLog([sync(since - DAY - MINUTE, 0), sync(since + 10 * MINUTE, 1)]);
  assert.deepEqual(await createCalibrations({ log: sparse, now: () => NOW }).read(since, NOW), []);
});

test("the settled log is read once; the last 15 minutes again at most every 2 minutes", async () => {
  const syncs = [sync(NOW - 20 * HOUR, 0), sync(NOW - 10 * MINUTE, 0)];
  const log = counting(createMemorySyncLog(syncs));
  let now = NOW;
  const calibrations = createCalibrations({ log, now: () => now });

  assert.deepEqual(await calibrations.read(now - DAY, now), []);
  assert.deepEqual(log.calls, [[NOW - 2 * DAY, NOW]]);

  // A recalibration at the next sync, and an entry that turns up late in the part
  // of the log already settled (which Cloud Logging does not do, so it is not looked
  // for). Within two minutes neither is seen and nothing is read.
  syncs.push(sync(NOW + 90 * SECOND, 1), sync(NOW - 30 * MINUTE, 5));
  now += 119 * SECOND;
  assert.deepEqual(await calibrations.read(now - DAY, now), []);
  assert.equal(log.calls.length, 1);

  // At two minutes only the unsettled part is read again: from 15 minutes before the
  // first read, so the sync 10 minutes before it is read twice and counted once.
  now = NOW + 2 * MINUTE;
  assert.deepEqual(await calibrations.read(now - DAY, now), [[NOW - 4.25 * MINUTE, 1]]);
  assert.deepEqual(log.calls.slice(1), [[NOW - 15 * MINUTE, now]]);

  // A longer window reads only the days before what is cached.
  now += MINUTE;
  assert.deepEqual(await calibrations.read(now - 3 * DAY, now), [[NOW - 4.25 * MINUTE, 1]]);
  assert.deepEqual(log.calls.slice(2), [[now - 4 * DAY, NOW - 2 * DAY]]);
  // And a shorter one reads nothing.
  assert.deepEqual(await calibrations.read(now - DAY, now), [[NOW - 4.25 * MINUTE, 1]]);
  assert.equal(log.calls.length, 3);

  // Next time round the read starts where the settled part had got to.
  now = NOW + 5 * MINUTE;
  await calibrations.read(now - DAY, now);
  assert.deepEqual(log.calls.slice(3), [[NOW - 13 * MINUTE, now]]);
});

test("requests made together share one read", async () => {
  const log = counting(createMemorySyncLog([sync(NOW - HOUR, 0), sync(NOW - 45 * MINUTE, 1)]));
  const calibrations = createCalibrations({ log, now: () => NOW });
  const results = await Promise.all([calibrations.read(NOW - DAY, NOW), calibrations.read(NOW - DAY, NOW)]);
  assert.deepEqual(results, [
    [[NOW - 52.5 * MINUTE, 1]],
    [[NOW - 52.5 * MINUTE, 1]],
  ]);
  assert.equal(log.calls.length, 1);
});

test("a failed read is not cached, and the next request tries again", async () => {
  const memory = createMemorySyncLog([sync(NOW - HOUR, 0), sync(NOW - 45 * MINUTE, 1)]);
  let failures = 1;
  const log = {
    async list(fromMs, toMs) {
      if (failures-- > 0) throw new Error("PERMISSION_DENIED");
      return memory.list(fromMs, toMs);
    },
  };
  const calibrations = createCalibrations({ log, now: () => NOW });
  await assert.rejects(calibrations.read(NOW - DAY, NOW), /PERMISSION_DENIED/);
  assert.deepEqual(await calibrations.read(NOW - DAY, NOW), [[NOW - 52.5 * MINUTE, 1]]);
});
