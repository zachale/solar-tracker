// Row parsing is the one place a silent data bug can hide: the firmware cannot be
// serviced remotely, so the server has to cope with whatever reaches it rather
// than expect the board to be corrected.
import test from "node:test";
import assert from "node:assert/strict";

process.env.NODE_ENV = "test";
const { parseRow } = await import("./routes.js");

test("parses a row in the order the firmware writes it", () => {
  const row = parseRow("1788947478,22.25,29,ACTIVE,3,1,1,0");
  assert.deepEqual(row, {
    time: "2026-09-09T09:51:18.000Z",
    temperature: 22.25,
    extended_percent: 29,
    status: "ACTIVE",
    wind_peak: 3,
    button_panel: { settings: true, wind_sensor: true, away: false },
  });
});

test("maps the flag characters to booleans independently", () => {
  const row = parseRow("1788947478,0.00,0,AWAY,0,0,0,1");
  assert.deepEqual(row.button_panel, {
    settings: false,
    wind_sensor: false,
    away: true,
  });
});

test("accepts every status the firmware can report", () => {
  for (const status of ["ACTIVE", "NIGHT", "SAFE", "AWAY"]) {
    assert.equal(parseRow(`1788947478,1.00,1,${status},1,1,1,1`).status, status);
  }
});

test("accepts a negative temperature", () => {
  assert.equal(parseRow("1788947478,-7.50,29,NIGHT,0,1,1,0").temperature, -7.5);
});

test("rejects a row truncated by a power cut mid-write", () => {
  assert.equal(parseRow("1788947478,22.25,29,ACTI"), null);
  assert.equal(parseRow("1788947478,22.25"), null);
  assert.equal(parseRow("17889"), null);
});

test("rejects a row with too many fields", () => {
  assert.equal(parseRow("1788947478,22.25,29,ACTIVE,3,1,1,0,9"), null);
});

test("rejects a non-numeric or zero timestamp", () => {
  assert.equal(parseRow("notatime,22.25,29,ACTIVE,3,1,1,0"), null);
  assert.equal(parseRow("0,22.25,29,ACTIVE,3,1,1,0"), null);
  assert.equal(parseRow("1788947478.5,22.25,29,ACTIVE,3,1,1,0"), null);
});

test("rejects an unknown status rather than storing it", () => {
  assert.equal(parseRow("1788947478,22.25,29,SLEEPY,3,1,1,0"), null);
});

test("rejects flags that are not 0 or 1", () => {
  assert.equal(parseRow("1788947478,22.25,29,ACTIVE,3,true,1,0"), null);
  assert.equal(parseRow("1788947478,22.25,29,ACTIVE,3,2,1,0"), null);
  assert.equal(parseRow("1788947478,22.25,29,ACTIVE,3,,1,0"), null);
});

test("rejects a non-numeric reading", () => {
  assert.equal(parseRow("1788947478,nan,29,ACTIVE,3,1,1,0"), null);
  assert.equal(parseRow("1788947478,22.25,x,ACTIVE,3,1,1,0"), null);
});

test("tolerates a stray carriage return", () => {
  // Nothing should write CRLF now, but a card written by an older build might.
  assert.equal(parseRow("1788947478,22.25,29,ACTIVE,3,1,1,0\r").status, "ACTIVE");
});

test("a full day of rows parses with nothing dropped", () => {
  const rows = [];
  for (let i = 0; i < 144; i++) {
    rows.push(`${1788900000 + i * 600},21.50,${i % 101},ACTIVE,${i % 32},1,1,0`);
  }
  const parsed = rows.map(parseRow);
  assert.equal(parsed.filter(Boolean).length, 144);
});
