// When the actuator recalibrated: drove all the way home and out to its far end to
// find out where it is, rather than trusting its count. The tracker does one at boot
// unless it took its position over from the previous firmware, and another whenever
// it loses track. /telemetry serves them as [tMs, count]: count recalibrations around
// the real instant tMs.
//
// No record says when one happened. Each clock sync (src/syncs.js, about every 15
// minutes) carries cal, the number of recalibrations since that boot, and the events
// are worked out from each pair of consecutive syncs a -> b:
//   b.cal > a.cal              b.cal - a.cal recalibrations
//   b.cal < a.cal, b.cal > 0   a reboot reset the count, and the new boot has done b.cal
// each placed halfway between a and b, since all that is known is that it happened in
// between. So an event is within about 7.5 minutes of when it really was, or further
// across a gap in the syncs.
//
// Blind spot: a reboot shows only as the count going down. A new boot that is back
// to the old count by the next sync is missed entirely (say cal=1, then a power cut
// reboots it and it calibrates once at boot: cal=1 again), and one that is past it
// counts only the difference.

const DAY = 24 * 60 * 60 * 1000;
// Cloud Logging can take a while to make an entry readable. Once a stretch of the log
// ended this long ago nothing more arrives in it, so it is read once and kept.
const SETTLE_MS = 15 * 60 * 1000;
// The newer part is read again at most this often. Reads come out of a quota of 60 a
// minute for the whole project, which the Logs Explorer and gcloud draw on too, and
// a new sync only comes every 15 minutes anyway.
const REFRESH_MS = 2 * 60 * 1000;

const DIGITS = /^\d+$/;

// The cal a sync reported, or null if it has none or it is not a whole number: such
// a sync is skipped, as if it had not been made.
export function calibrationCount(url) {
  if (typeof url !== "string") return null;
  let raw;
  try {
    raw = new URL(url, "https://localhost").searchParams.get("cal");
  } catch {
    return null;
  }
  if (raw === null || !DIGITS.test(raw)) return null;
  const count = Number(raw);
  return Number.isSafeInteger(count) ? count : null;
}

// [[tMs, count], ...] for the events from since to until (both included), oldest
// first, from syncs ({t, url}, oldest first). The syncs should start a day before
// since, so the first one in the window has one before it to compare with.
export function deriveEvents(syncs, since, until) {
  const events = [];
  let previous = null;
  for (const { t, url } of syncs) {
    const cal = calibrationCount(url);
    if (cal === null) continue;
    if (previous) {
      const count = cal > previous.cal ? cal - previous.cal : cal < previous.cal ? cal : 0;
      const at = Math.round((previous.t + t) / 2);
      if (count > 0 && at >= since && at <= until) events.push([at, count]);
    }
    previous = { t, cal };
  }
  return events;
}

export function createCalibrations({ log, now }) {
  // The syncs read so far, oldest first. settled holds every one from `from` up to
  // settledTo, a stretch that had already settled when it was read, so it is kept for
  // good. recent holds those from settledTo to readAt, and is read again once it is
  // REFRESH_MS old. A longer window than any before reads only the days before
  // `from`, so a request costs at most two reads and usually none.
  let cache = null;
  // Requests take turns: one that arrives during a read waits for it and then finds
  // the cache filled, instead of making the same read alongside it. A failed read
  // leaves the cache as it was, so the next request tries again.
  let queue = Promise.resolve();

  async function fill(from, at) {
    if (!cache) {
      const settledTo = Math.max(from, at - SETTLE_MS);
      const syncs = await log.list(from, at);
      cache = {
        from,
        settledTo,
        settled: syncs.filter(({ t }) => t < settledTo),
        recent: syncs.filter(({ t }) => t >= settledTo),
        readAt: at,
      };
      return;
    }
    if (from < cache.from) {
      const older = await log.list(from, cache.from);
      cache.settled = older.concat(cache.settled);
      cache.from = from;
    }
    if (at - cache.readAt >= REFRESH_MS) {
      const settledTo = Math.max(cache.settledTo, at - SETTLE_MS);
      const syncs = await log.list(cache.settledTo, at);
      cache.settled = cache.settled.concat(syncs.filter(({ t }) => t < settledTo));
      cache.recent = syncs.filter(({ t }) => t >= settledTo);
      cache.settledTo = settledTo;
      cache.readAt = at;
    }
  }

  return {
    read(since, until) {
      const from = since - DAY;
      const events = queue.then(async () => {
        await fill(from, now());
        // Only the day before the window, even when more is cached, so the answer
        // does not depend on what earlier requests happened to read.
        const syncs = cache.settled.concat(cache.recent).filter(({ t }) => t >= from && t <= until);
        return deriveEvents(syncs, since, until);
      });
      queue = events.catch(() => {});
      return events;
    },
  };
}
