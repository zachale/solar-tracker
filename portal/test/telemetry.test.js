import test from "node:test";
import assert from "node:assert/strict";

import { createMemoryStore } from "../src/stores.js";
import { STATUSES, createTelemetry, parseObject, toRow, uploadTime } from "../src/telemetry.js";

const TZ = "America/Toronto";
const SECOND = 1000;
const DAY = 24 * 60 * 60 * SECOND;
// Noon in Toronto (EDT) on 11 October 2026.
const NOW = Date.parse("2026-10-11T16:00:00.000Z");

// A stored record as the upload service writes it; time is Toronto wall clock.
function record(wall, overrides = {}) {
  return {
    time: `${wall}.000Z`,
    temperature: 12.5,
    extended_percent: 40,
    status: "ACTIVE",
    wind_peak: 2,
    button_panel: { settings: false, wind_sensor: true, away: false },
    ...overrides,
  };
}
const ndjson = (...records) => records.map((r) => (typeof r === "string" ? r : JSON.stringify(r))).join("\n") + "\n";
const at = (iso) => Date.parse(iso);

// A store that counts what is asked of it, to check the caching.
function counting(store) {
  const calls = { list: [], read: [] };
  return {
    calls,
    async list(prefix) {
      calls.list.push(prefix);
      return store.list(prefix);
    },
    async read(name) {
      calls.read.push(name);
      return store.read(name);
    },
  };
}

test("a record becomes [tMs, extended, temperature, windPeak, statusIndex, windSensor]", () => {
  assert.deepEqual(toRow(record("2026-10-10T07:00:20"), TZ), [at("2026-10-10T11:00:20Z"), 40, 12.5, 2, 0, 1]);
  assert.deepEqual(
    toRow(
      record("2026-01-15T23:55:00", {
        status: "AWAY",
        temperature: -7.25,
        button_panel: { settings: true, wind_sensor: false, away: true },
      }),
      TZ,
    ),
    [at("2026-01-16T04:55:00Z"), 40, -7.25, 2, 3, 0],
  );
  assert.deepEqual(STATUSES, ["ACTIVE", "NIGHT", "SAFE", "AWAY"]);
  assert.equal(toRow(record("2026-10-10T07:00:20", { status: "SAFE" }), TZ)[4], 2);
});

test("records that fail validation are dropped, not fatal", () => {
  const bad = [
    record("2026-10-10T07:00:00", { status: "SLEEPY" }),
    record("2026-10-10T07:00:00", { status: "active" }),
    record("2026-10-10T07:00:00", { temperature: "12.5" }),
    record("2026-10-10T07:00:00", { temperature: null }),
    record("2026-10-10T07:00:00", { extended_percent: undefined }),
    record("2026-10-10T07:00:00", { wind_peak: Number.NaN }),
    record("2026-10-10T07:00:00", { button_panel: undefined }),
    record("2026-10-10T07:00:00", { button_panel: { wind_sensor: "true" } }),
    { ...record("2026-10-10T07:00:00"), time: "2026-10-10T07:00:00" },
    { ...record("2026-10-10T07:00:00"), time: "2026-13-40T07:00:00.000Z" },
    { ...record("2026-10-10T07:00:00"), time: 1791990000 },
    null,
    [],
    "ACTIVE",
  ];
  for (const r of bad) assert.equal(toRow(r, TZ), null, JSON.stringify(r));

  const text = ndjson(
    record("2026-10-10T07:00:00"),
    '{"time":"2026-10-10T07:05:00.000Z","temperature":12',
    "not json at all",
    "",
    record("2026-10-10T07:05:00", { status: "SLEEPY" }),
    record("2026-10-10T07:10:00", { status: "NIGHT" }),
  );
  assert.deepEqual(
    parseObject(text, TZ, at("2026-10-10T11:15:00Z")).map((row) => row[0]),
    [at("2026-10-10T11:00:00Z"), at("2026-10-10T11:10:00Z")],
  );
});

test("a row dated after the upload that carried it is a clock gone wrong", async () => {
  // As in the bucket: on 10 September the tracker's clock came back in 2043.
  const name = "raw/2026-09-10/2026-09-10T20-09-06-030Z.ndjson";
  const text = ndjson(
    record("2043-04-29T18:11:44"),
    record("2026-09-10T16:05:00"), // 20:05Z, before the upload
    record("2026-09-10T17:05:00"), // 21:05Z, 56 min after it: within the slack
    record("2026-09-10T17:15:00"), // 21:15Z, 66 min after it
  );
  assert.deepEqual(
    parseObject(text, TZ, uploadTime(name)).map((row) => new Date(row[0]).toISOString()),
    ["2026-09-10T20:05:00.000Z", "2026-09-10T21:05:00.000Z"],
  );

  const telemetry = createTelemetry({ store: createMemoryStore({ [name]: text }), tz: TZ, now: () => NOW });
  const { rows } = await telemetry.read(60);
  assert.equal(rows.length, 2);
  assert.ok(rows.every((row) => row[0] < NOW));
});

test("nothing more than an hour in the future is served, whatever the object says", async () => {
  // An object stamped three hours ahead (a server clock or a hand-copied object
  // gone wrong): the per-upload check lets its rows through, the request's own
  // clock does not.
  const store = createMemoryStore({
    "raw/2026-10-11/2026-10-11T19-00-00-000Z.ndjson": ndjson(
      record("2026-10-11T12:00:00"), // 16:00Z, now
      record("2026-10-11T12:59:00"), // 16:59Z, 59 min ahead
      record("2026-10-11T13:01:00"), // 17:01Z, 61 min ahead
      record("2026-10-11T14:00:00"), // 18:00Z, 2 h ahead
    ),
  });
  const telemetry = createTelemetry({ store, tz: TZ, now: () => NOW });
  assert.deepEqual(
    (await telemetry.read(1)).rows.map((row) => new Date(row[0]).toISOString()),
    ["2026-10-11T16:00:00.000Z", "2026-10-11T16:59:00.000Z"],
  );
});

test("an object name gives its upload time", () => {
  assert.equal(uploadTime("raw/2026-10-10/2026-10-10T10-02-02-772Z.ndjson"), at("2026-10-10T10:02:02.772Z"));
  assert.equal(uploadTime("raw/2026-10-10/notes.txt"), null);
});

test("rows are converted, filtered to the window, sorted and deduplicated", async () => {
  const store = createMemoryStore({
    // Before the one-day window (13:00 EDT on 10 October = 17:00Z is inside it;
    // 07:00 EDT = 11:00Z is not).
    "raw/2026-10-10/2026-10-10T12-00-00-000Z.ndjson": ndjson(
      record("2026-10-10T07:00:00"),
      record("2026-10-10T07:05:00"),
    ),
    // Uploaded the next morning after a night offline: last evening's rows, the
    // first of them exactly on the window's edge.
    "raw/2026-10-11/2026-10-11T10-03-04-886Z.ndjson": ndjson(
      record("2026-10-10T12:00:00", { temperature: 21 }),
      record("2026-10-10T19:00:00", { status: "NIGHT", extended_percent: 100 }),
      record("2026-10-10T19:05:00", { status: "BOGUS" }),
    ),
    // Two uploads carrying the same row (a retried upload), written out of order.
    "raw/2026-10-11/2026-10-11T15-00-00-000Z.ndjson": ndjson(
      record("2026-10-11T10:55:00", { wind_peak: 9 }),
      record("2026-10-11T10:50:00", { status: "SAFE", wind_peak: 31 }),
    ),
    "raw/2026-10-11/2026-10-11T15-30-00-500Z.ndjson": ndjson(
      record("2026-10-11T10:55:00", { wind_peak: 1 }),
      record("2026-10-11T11:00:00", { button_panel: { settings: false, wind_sensor: false, away: false } }),
    ),
    "raw/2026-10-11/notes.txt": "not telemetry",
  });
  const telemetry = createTelemetry({ store, tz: TZ, now: () => NOW });

  const day = await telemetry.read(1);
  assert.equal(day.generatedAt, "2026-10-11T16:00:00.000Z");
  assert.equal(day.lastUploadAt, "2026-10-11T15:30:00.500Z");
  assert.deepEqual(day.statuses, STATUSES);
  assert.deepEqual(day.rows, [
    [at("2026-10-10T16:00:00Z"), 40, 21, 2, 0, 1],
    [at("2026-10-10T23:00:00Z"), 100, 12.5, 2, 1, 1],
    [at("2026-10-11T14:50:00Z"), 40, 12.5, 31, 2, 1],
    [at("2026-10-11T14:55:00Z"), 40, 12.5, 9, 0, 1],
    [at("2026-10-11T15:00:00Z"), 40, 12.5, 2, 0, 0],
  ]);

  const twoDays = await telemetry.read(2);
  assert.deepEqual(
    twoDays.rows.slice(0, 2).map((row) => row[0]),
    [at("2026-10-10T11:00:00Z"), at("2026-10-10T11:05:00Z")],
  );
  assert.equal(twoDays.rows.length, 7);
});

test("it lists each UTC date from a day before the window through today", async () => {
  const store = counting(createMemoryStore());
  const telemetry = createTelemetry({ store, tz: TZ, now: () => NOW });
  const empty = await telemetry.read(2);
  assert.deepEqual(store.calls.list, ["raw/2026-10-08/", "raw/2026-10-09/", "raw/2026-10-10/", "raw/2026-10-11/"]);
  assert.deepEqual(empty.rows, []);
  assert.equal(empty.lastUploadAt, null);
});

test("objects are read once; only today's and yesterday's listings are refreshed", async () => {
  const memory = createMemoryStore({
    "raw/2026-10-09/2026-10-09T20-00-00-000Z.ndjson": ndjson(record("2026-10-09T15:00:00")),
    "raw/2026-10-10/2026-10-10T20-00-00-000Z.ndjson": ndjson(record("2026-10-10T15:00:00")),
    "raw/2026-10-11/2026-10-11T15-00-00-000Z.ndjson": ndjson(record("2026-10-11T10:55:00")),
  });
  const store = counting(memory);
  let now = NOW;
  const telemetry = createTelemetry({ store, tz: TZ, now: () => now });

  await telemetry.read(2);
  assert.equal(store.calls.list.length, 4);
  assert.equal(store.calls.read.length, 3);

  // Within a minute nothing is fetched again.
  now += 30 * SECOND;
  await telemetry.read(2);
  assert.equal(store.calls.list.length, 4);
  assert.equal(store.calls.read.length, 3);

  // After a minute today and yesterday are listed again and only the new upload is read.
  await memory.write("raw/2026-10-11/2026-10-11T16-00-10-000Z.ndjson", ndjson(record("2026-10-11T12:00:00")));
  now += 31 * SECOND;
  store.calls.list.length = 0;
  store.calls.read.length = 0;
  const later = await telemetry.read(2);
  assert.deepEqual(store.calls.list, ["raw/2026-10-10/", "raw/2026-10-11/"]);
  assert.deepEqual(store.calls.read, ["raw/2026-10-11/2026-10-11T16-00-10-000Z.ndjson"]);
  assert.equal(later.rows.at(-1)[0], at("2026-10-11T16:00:00Z"));
  assert.equal(later.lastUploadAt, "2026-10-11T16:00:10.000Z");

  // Two days on, 10 and 11 October are old. Each is listed once more, because it
  // was last listed while it could still grow, and then never again.
  now += 2 * DAY;
  store.calls.list.length = 0;
  await telemetry.read(3);
  // 9 October was already old when first listed, so it is not listed again.
  assert.deepEqual(store.calls.list, ["raw/2026-10-10/", "raw/2026-10-11/", "raw/2026-10-12/", "raw/2026-10-13/"]);
  now += 2 * 60 * SECOND;
  store.calls.list.length = 0;
  await telemetry.read(3);
  assert.deepEqual(store.calls.list, ["raw/2026-10-12/", "raw/2026-10-13/"]);
});

test("a failed read is not cached", async () => {
  const memory = createMemoryStore({
    "raw/2026-10-11/2026-10-11T15-00-00-000Z.ndjson": ndjson(record("2026-10-11T10:55:00")),
  });
  let failNext = true;
  const store = {
    list: (prefix) => memory.list(prefix),
    async read(name) {
      if (failNext) {
        failNext = false;
        throw new Error("bucket unavailable");
      }
      return memory.read(name);
    },
  };
  const telemetry = createTelemetry({ store, tz: TZ, now: () => NOW });
  await assert.rejects(telemetry.read(1), /bucket unavailable/);
  assert.equal((await telemetry.read(1)).rows.length, 1);
});

test("a bad zone name fails at construction", () => {
  assert.throws(() => createTelemetry({ store: createMemoryStore(), tz: "America/Nowhere", now: () => NOW }), RangeError);
});
