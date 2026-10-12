// The tracker sets its clock from the upload service (test-image) about every 15
// minutes, and from v12.6 it reports its actuator's calibration count as it does:
//   GET /time/est?fw=v12.10&cal=1&pos=0
// The route ignores the query and stores nothing, so Cloud Run's request log is the
// only record of these. Like the buckets in stores.js, the log is reached through
// one call, so the calibrations can be worked out from an in-memory copy in tests:
//
//   list(fromMs, toMs)   [{t, url}] for each sync from fromMs up to (not including)
//                        toMs, oldest first: when it was made, in real UTC epoch ms,
//                        and the URL it asked for
//
// Reading the log needs roles/logging.viewer on the project.

// entries.list returns 50 entries a page unless asked for more, and 1000 at most. A
// day is about 96 syncs and the log keeps 30 days, so this is three pages at worst
// instead of 58, out of a quota of 60 reads a minute for the whole project.
const PAGE_SIZE = 1000;
// The telemetry response waits for this read, so a slow or unreachable log may hold
// the page up by this long (retries included) per page. The library's default is a
// minute.
const TIMEOUT_MS = 10_000;

export function createLoggingSyncLog(logging, { service }) {
  return {
    async list(fromMs, toMs) {
      // requestUrl:"..." is a substring match. Syncs from before v12.6 have no cal
      // and are left out here rather than read and thrown away.
      const filter = [
        'resource.type="cloud_run_revision"',
        `resource.labels.service_name=${JSON.stringify(service)}`,
        'httpRequest.requestUrl:"/time/est"',
        'httpRequest.requestUrl:"cal="',
        `timestamp >= "${new Date(fromMs).toISOString()}"`,
        `timestamp < "${new Date(toMs).toISOString()}"`,
      ].join(" AND ");

      const syncs = [];
      let pageToken;
      do {
        // Paged here rather than by the library, which warns when it auto-pages
        // with a page size set.
        const [entries, next] = await logging.getEntries({
          filter,
          orderBy: "timestamp asc",
          pageSize: PAGE_SIZE,
          pageToken,
          autoPaginate: false,
          gaxOptions: { timeout: TIMEOUT_MS },
        });
        // The library turns each entry's timestamp into a Date. It is when the
        // request reached Cloud Run, by Google's clock, not the tracker's.
        for (const { metadata } of entries) {
          syncs.push({ t: metadata.timestamp.getTime(), url: metadata.httpRequest?.requestUrl });
        }
        pageToken = next?.pageToken;
      } while (pageToken);
      return syncs;
    },
  };
}

// The array is read on every call, so a test can push syncs to it as time goes on.
export function createMemorySyncLog(syncs = []) {
  return {
    async list(fromMs, toMs) {
      return syncs.filter(({ t }) => t >= fromMs && t < toMs).sort((a, b) => a.t - b.t);
    },
  };
}
