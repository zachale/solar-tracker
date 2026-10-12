// The tracker's clock is set from a wall-clock string (/time/est on the upload
// service), so every row's "time" is the site's local time with a Z stuck on the
// end: "2026-10-10T07:00:20.000Z" means 07:00:20 in Toronto, which is 11:00:20 UTC
// in summer and 12:00:20 in winter. The page plots real instants, so each row is
// moved from that mislabelled wall clock to the instant it actually names.

const formatters = new Map();

function formatter(tz) {
  let f = formatters.get(tz);
  if (!f) {
    // Building a formatter is far slower than using one, and every row needs two.
    // h23 because some ICU builds write midnight as hour 24 under hour12: false.
    f = new Intl.DateTimeFormat("en-US", {
      timeZone: tz,
      hourCycle: "h23",
      year: "numeric",
      month: "2-digit",
      day: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    });
    formatters.set(tz, f);
  }
  return f;
}

// How far the wall clock in tz is ahead of UTC at a given instant, in ms
// (negative west of Greenwich: -4 h for EDT, -5 h for EST).
export function offsetAt(instantMs, tz) {
  const f = {};
  for (const { type, value } of formatter(tz).formatToParts(instantMs)) {
    f[type] = Number(value);
  }
  const wallMs = Date.UTC(f.year, f.month - 1, f.day, f.hour, f.minute, f.second);
  // The formatter drops milliseconds, so compare against the whole second.
  return wallMs - Math.floor(instantMs / 1000) * 1000;
}

// wallMs is the wall-clock reading as if it were UTC (Date.parse of the row's
// time). The offset is first guessed at wallMs itself, which is off by the zone's
// own offset, then taken again at the instant that guess gives. Away from a DST
// change both agree; across one, the second look lands on the right side of it.
//
// Two wall times are not a single instant and the answer for them is fixed
// rather than accidental. For Toronto (and any zone west of Greenwich):
//   - a repeated hour (01:00-01:59 on the first Sunday of November) is read as its
//     first pass, still on daylight time;
//   - a skipped hour (02:00-02:59 on the second Sunday of March) is read with the
//     daylight offset, so it lands an hour earlier, among the 01:xx EST rows.
// Neither should appear in practice unless the tracker's clock is resynced in the
// middle of the change, and either way at most an hour a year is affected.
export function wallClockToInstant(wallMs, tz) {
  const guess = wallMs - offsetAt(wallMs, tz);
  return wallMs - offsetAt(guess, tz);
}
