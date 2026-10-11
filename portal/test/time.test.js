// Every row's time goes through this, and a mistake here moves the whole chart by
// an hour for half the year, so it is checked on both 2026 DST changes in Toronto:
// clocks went forward at 02:00 on 8 March and go back at 02:00 on 1 November.
import test from "node:test";
import assert from "node:assert/strict";

import { wallClockToInstant } from "../src/time.js";

const TZ = "America/Toronto";
const MINUTE = 60_000;

// "2026-03-08T01:59:59" read as wall time in Toronto, as an ISO instant.
const convert = (wall, tz = TZ) => new Date(wallClockToInstant(Date.parse(`${wall}Z`), tz)).toISOString();

// An independent check: what a clock in Toronto shows at an instant, as
// "2026-03-08 01:59:59" (Swedish formatting is ISO-like).
const showing = new Intl.DateTimeFormat("sv-SE", {
  timeZone: TZ,
  year: "numeric",
  month: "2-digit",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  second: "2-digit",
});

test("summer rows are four hours behind UTC, winter rows five", () => {
  assert.equal(convert("2026-10-10T07:00:20.000"), "2026-10-10T11:00:20.000Z");
  assert.equal(convert("2026-01-15T12:00:00.000"), "2026-01-15T17:00:00.000Z");
});

test("milliseconds and the change of date carry through", () => {
  assert.equal(convert("2026-07-01T00:00:00.123"), "2026-07-01T04:00:00.123Z");
  assert.equal(convert("2026-12-31T22:30:00.000"), "2027-01-01T03:30:00.000Z");
});

test("spring forward, 8 March 2026", () => {
  assert.equal(convert("2026-03-08T01:59:59.000"), "2026-03-08T06:59:59.000Z");
  assert.equal(convert("2026-03-08T03:00:00.000"), "2026-03-08T07:00:00.000Z");
  // Wall times whose own value, read as UTC, is still before the change.
  assert.equal(convert("2026-03-08T06:59:00.000"), "2026-03-08T10:59:00.000Z");
  assert.equal(convert("2026-03-08T07:30:00.000"), "2026-03-08T11:30:00.000Z");
  assert.equal(convert("2026-03-09T00:00:00.000"), "2026-03-09T04:00:00.000Z");
});

test("a skipped wall time is read with the daylight offset, every time", () => {
  // 02:00-02:59 never happened on 8 March. The rule in time.js puts it an hour early.
  assert.equal(convert("2026-03-08T02:00:00.000"), "2026-03-08T06:00:00.000Z");
  assert.equal(convert("2026-03-08T02:30:00.000"), "2026-03-08T06:30:00.000Z");
  assert.equal(convert("2026-03-08T02:59:59.000"), "2026-03-08T06:59:59.000Z");
});

test("fall back, 1 November 2026", () => {
  assert.equal(convert("2026-11-01T00:59:59.000"), "2026-11-01T04:59:59.000Z");
  assert.equal(convert("2026-11-01T02:00:00.000"), "2026-11-01T07:00:00.000Z");
  assert.equal(convert("2026-11-01T05:59:00.000"), "2026-11-01T10:59:00.000Z");
  assert.equal(convert("2026-11-01T06:00:00.000"), "2026-11-01T11:00:00.000Z");
  assert.equal(convert("2026-10-31T23:00:00.000"), "2026-11-01T03:00:00.000Z");
});

test("a repeated wall time is read as its first, daylight-time pass", () => {
  assert.equal(convert("2026-11-01T01:00:00.000"), "2026-11-01T05:00:00.000Z");
  assert.equal(convert("2026-11-01T01:30:00.000"), "2026-11-01T05:30:00.000Z");
  assert.equal(convert("2026-11-01T01:59:59.000"), "2026-11-01T05:59:59.000Z");
});

test("every minute of both changeover weekends lands on an instant showing that wall time", () => {
  for (const [from, to] of [
    ["2026-03-07T00:00:00", "2026-03-10T00:00:00"],
    ["2026-10-31T00:00:00", "2026-11-03T00:00:00"],
  ]) {
    for (let wall = Date.parse(`${from}Z`); wall < Date.parse(`${to}Z`); wall += MINUTE) {
      const label = new Date(wall).toISOString();
      if (label.startsWith("2026-03-08T02:")) continue; // skipped hour, covered above
      const instant = wallClockToInstant(wall, TZ);
      assert.equal(showing.format(instant), label.slice(0, 19).replace("T", " "), label);
    }
  }
});

test("a zone without DST is a fixed shift", () => {
  assert.equal(convert("2026-03-08T02:30:00.000", "UTC"), "2026-03-08T02:30:00.000Z");
  assert.equal(convert("2026-11-01T01:30:00.000", "America/Regina"), "2026-11-01T07:30:00.000Z");
});
