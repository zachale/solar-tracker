import { wallClockToInstant } from "./time.js";

// The upload service (test-image) writes one immutable object per upload,
//   raw/<UTC date of the upload>/<UTC upload time, 2026-10-10T10-02-02-772Z>.ndjson
// with a row every five minutes, so a day is about 46 objects and 288 rows. The
// page gets them as compact arrays rather than the stored records: 60 days is
// about 17,000 rows, and an array is about a sixth the size of its JSON record.
//   [tMs, extendedPercent, temperatureC, windPeak, statusIndex, windSensor 0|1]

export const STATUSES = ["ACTIVE", "NIGHT", "SAFE", "AWAY"];

const DAY = 24 * 60 * 60 * 1000;
// Today's and yesterday's listings can still grow; older days cannot, since an
// object is filed under the UTC date it was uploaded on.
const LIVE_LISTING_MS = 60_000;
const FETCH_CONCURRENCY = 16;
// A row is taken before the upload that carries it. After losing power the
// tracker's clock has come back years out (rows dated 2000, 2015 and 2043 are in
// the bucket); the past ones fall outside any window, but a 2043 row would stretch
// the page's time axis seventeen years. So a row dated after its upload is
// dropped, with an hour's slack for a clock running a little fast and for the
// hour after the November change while a clock not yet resynced is still on
// daylight time.
const CLOCK_SLACK_MS = 60 * 60 * 1000;

const ISO_UTC = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,3})?Z$/;
const UPLOAD_STAMP = /(\d{4})-(\d{2})-(\d{2})T(\d{2})-(\d{2})-(\d{2})-(\d{3})Z\.ndjson$/;

// One stored record to a row, or null if it is not usable. A bad row is dropped
// rather than failing the request: one corrupt line must not hide a day of good ones.
export function toRow(record, tz) {
  if (record === null || typeof record !== "object") return null;
  const { time, temperature, extended_percent: extended, status, wind_peak: windPeak } = record;

  // Checked against the exact shape before Date.parse, which would read a string
  // without the Z in the server's own zone.
  const wallMs = typeof time === "string" && ISO_UTC.test(time) ? Date.parse(time) : NaN;
  const statusIndex = STATUSES.indexOf(status);
  const windSensor = record.button_panel?.wind_sensor;

  if (!Number.isFinite(wallMs) || statusIndex === -1 || typeof windSensor !== "boolean") return null;
  // Number.isFinite, not isFinite: "21.5" as a string is not a reading.
  if (![temperature, extended, windPeak].every((n) => Number.isFinite(n))) return null;

  return [wallClockToInstant(wallMs, tz), extended, temperature, windPeak, statusIndex, windSensor ? 1 : 0];
}

// The usable rows of one uploaded object.
export function parseObject(text, tz, uploadedAt) {
  const rows = [];
  for (const line of text.split("\n")) {
    if (!line.trim()) continue;
    let record;
    try {
      record = JSON.parse(line);
    } catch {
      continue;
    }
    const row = toRow(record, tz);
    if (row && row[0] <= uploadedAt + CLOCK_SLACK_MS) rows.push(row);
  }
  return rows;
}

// The upload time an object's name records, in epoch ms, or null.
export function uploadTime(name) {
  const m = UPLOAD_STAMP.exec(name);
  return m ? Date.UTC(+m[1], m[2] - 1, +m[3], +m[4], +m[5], +m[6], +m[7]) : null;
}

const utcDate = (ms) => new Date(ms).toISOString().slice(0, 10);

function utcDates(fromMs, toMs) {
  const dates = [];
  for (let t = Math.floor(fromMs / DAY) * DAY; t <= toMs; t += DAY) dates.push(utcDate(t));
  return dates;
}

async function mapLimit(items, limit, fn) {
  const results = new Array(items.length);
  let next = 0;
  async function worker() {
    while (next < items.length) {
      const i = next++;
      results[i] = await fn(items[i]);
    }
  }
  await Promise.all(Array.from({ length: Math.min(limit, items.length) }, worker));
  return results;
}

export function createTelemetry({ store, tz, now }) {
  // A bad zone name should stop the service at start-up, not fail every request.
  wallClockToInstant(0, tz);

  // Both caches hold promises, so concurrent requests share one read, and both
  // drop an entry whose read failed so the next request tries again.
  const listings = new Map(); // UTC date -> {names, at, final}
  const objects = new Map(); // object name -> rows; objects never change, so kept for good

  function listDate(date, at) {
    const cached = listings.get(date);
    if (cached && (cached.final || at - cached.at < LIVE_LISTING_MS)) return cached.names;

    const entry = {
      names: store.list(`raw/${date}/`).then((names) => names.filter((name) => uploadTime(name) !== null)),
      at,
      // Only final if the date was already over when listed. A listing taken
      // during its own day stays on the 60 s clock even after that day ends.
      final: date < utcDate(at - DAY),
    };
    listings.set(date, entry);
    entry.names.catch(() => {
      if (listings.get(date) === entry) listings.delete(date);
    });
    return entry.names;
  }

  function rowsOf(name) {
    let rows = objects.get(name);
    if (!rows) {
      rows = store.read(name).then((text) => parseObject(text ?? "", tz, uploadTime(name)));
      objects.set(name, rows);
      const entry = rows;
      rows.catch(() => {
        if (objects.get(name) === entry) objects.delete(name);
      });
    }
    return rows;
  }

  return {
    async read(days) {
      const at = now();
      const since = at - days * DAY;

      // A row is uploaded at or after the moment it describes, so its object is
      // filed on that date or later. The extra day before covers a tracker clock
      // running ahead of the server's.
      const listed = await Promise.all(utcDates(since - DAY, at).map((date) => listDate(date, at)));
      const names = listed.flat();
      const batches = await mapLimit(names, FETCH_CONCURRENCY, rowsOf);

      // Inside the window, and no more than the clock slack into the future. The
      // per-upload check in parseObject already implies the second bound for any
      // object stamped by the upload service; this one holds even if an object's
      // stamp is wrong.
      // Objects are in upload order and the sort is stable, so when a row was sent
      // twice (a retried upload), the first copy is the one kept.
      const sorted = batches
        .flat()
        .filter((row) => row[0] >= since && row[0] <= at + CLOCK_SLACK_MS)
        .sort((a, b) => a[0] - b[0]);
      const rows = sorted.filter((row, i) => i === 0 || row[0] !== sorted[i - 1][0]);

      return {
        generatedAt: new Date(at).toISOString(),
        lastUploadAt: names.length ? new Date(Math.max(...names.map(uploadTime))).toISOString() : null,
        statuses: STATUSES,
        rows,
      };
    },
  };
}
