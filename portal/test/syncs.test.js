import test from "node:test";
import assert from "node:assert/strict";

import { createLoggingSyncLog, createMemorySyncLog } from "../src/syncs.js";

const at = (iso) => Date.parse(iso);
const URL_BASE = "https://test-image-188091506191.us-central1.run.app/time/est";

// An entry as @google-cloud/logging hands it back: the timestamp already a Date.
const entry = (iso, query) => ({
  metadata: { timestamp: new Date(iso), httpRequest: { requestMethod: "GET", requestUrl: `${URL_BASE}?${query}` } },
});

test("the request log is read for one service's syncs with cal, oldest first, a page at a time", async () => {
  const requests = [];
  const pages = [
    [[entry("2026-10-12T17:45:46.906Z", "fw=v12.10&cal=0&pos=0")], { pageToken: "page-2" }],
    [[entry("2026-10-12T18:00:37.573Z", "fw=v12.10&cal=1&pos=0")], null],
  ];
  const logging = {
    async getEntries(request) {
      requests.push(request);
      return pages.shift();
    },
  };
  const log = createLoggingSyncLog(logging, { service: "test-image" });

  assert.deepEqual(await log.list(at("2026-10-11T00:00:00Z"), at("2026-10-12T22:00:00.5Z")), [
    { t: at("2026-10-12T17:45:46.906Z"), url: `${URL_BASE}?fw=v12.10&cal=0&pos=0` },
    { t: at("2026-10-12T18:00:37.573Z"), url: `${URL_BASE}?fw=v12.10&cal=1&pos=0` },
  ]);
  const filter =
    'resource.type="cloud_run_revision" AND resource.labels.service_name="test-image" AND ' +
    'httpRequest.requestUrl:"/time/est" AND httpRequest.requestUrl:"cal=" AND ' +
    'timestamp >= "2026-10-11T00:00:00.000Z" AND timestamp < "2026-10-12T22:00:00.500Z"';
  const common = { filter, orderBy: "timestamp asc", pageSize: 1000, autoPaginate: false, gaxOptions: { timeout: 10_000 } };
  assert.deepEqual(requests, [
    { ...common, pageToken: undefined },
    { ...common, pageToken: "page-2" },
  ]);
});

test("an empty stretch of the log is one read and no syncs", async () => {
  let reads = 0;
  const logging = {
    async getEntries() {
      reads++;
      return [[], null, {}];
    },
  };
  assert.deepEqual(await createLoggingSyncLog(logging, { service: "test-image" }).list(0, 1), []);
  assert.equal(reads, 1);
});

test("the in-memory log returns the syncs from `from` up to `to`, oldest first", async () => {
  const syncs = [
    { t: 30, url: "c" },
    { t: 10, url: "a" },
    { t: 20, url: "b" },
  ];
  const log = createMemorySyncLog(syncs);
  assert.deepEqual(await log.list(10, 30), [
    { t: 10, url: "a" },
    { t: 20, url: "b" },
  ]);
  syncs.push({ t: 25, url: "late" });
  assert.deepEqual(
    (await log.list(0, 100)).map(({ url }) => url),
    ["a", "b", "late", "c"],
  );
});
