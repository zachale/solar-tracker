// Runs the portal API on this machine against the real telemetry bucket and the
// upload service's real request log, both read-only, with users.json held in memory.
// It lists one person with a fresh key, the way tools/portal/users.sh does, opens
// their link over HTTP the way the page does, summarizes what /telemetry returns,
// and signs out. Nothing is written to any bucket or log.
//
//   node scripts/smoke.js [days]        default 2
//
// Uses the gcloud CLI's own login (gcloud auth print-access-token), so it works
// without application default credentials.
import { execFileSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import { once } from "node:events";

import { Logging } from "@google-cloud/logging";
import { Storage } from "@google-cloud/storage";
// Installed with @google-cloud/storage; used only here, to hand it the CLI's token.
import { OAuth2Client } from "google-auth-library";
// Installed with @google-cloud/logging, whose client authenticates through it. It is
// a newer major version of google-auth-library than the one above, and each client
// only takes a token holder from its own.
import { googleAuthLibrary } from "google-gax";

import { createApp } from "../src/app.js";
import { createGcsStore, createMemoryStore } from "../src/stores.js";
import { createLoggingSyncLog } from "../src/syncs.js";
import { uploadTime } from "../src/telemetry.js";
import { hashKey } from "../src/users.js";

const DAYS = process.argv[2] ?? "2";
const TZ = "America/Toronto";
const PROJECT = "solar-tracker-442903";
const BUCKET = "solar-tracker-442903-telemetry";
// Made here and never stored, as users.sh does.
const KEY = randomBytes(32).toString("base64url");

const accessToken = execFileSync("gcloud", ["auth", "print-access-token"], {
  stdio: ["ignore", "pipe", "inherit"],
})
  .toString()
  .trim();
const authClient = new OAuth2Client();
authClient.setCredentials({ access_token: accessToken });
const storage = new Storage({ authClient, projectId: PROJECT });
const logAuthClient = new googleAuthLibrary.OAuth2Client();
logAuthClient.setCredentials({ access_token: accessToken });
const syncLog = createLoggingSyncLog(new Logging({ authClient: logAuthClient, projectId: PROJECT }), {
  service: "test-image",
});

// Only list and read are passed on, so a write to the real bucket would throw here.
const { list, read } = createGcsStore(storage.bucket(BUCKET));
// Counted, to show what a request costs against the log's read quota.
const logReads = [];
const portalStore = createMemoryStore({
  "users.json": JSON.stringify({
    users: [{ id: "smoke", name: "Smoke test", keyHash: hashKey(KEY), issuedAt: new Date().toISOString() }],
  }),
});
const app = createApp({
  telemetryStore: { list, read },
  portalStore,
  syncLog: {
    list(fromMs, toMs) {
      logReads.push(`${new Date(fromMs).toISOString()} to ${new Date(toMs).toISOString()}`);
      return syncLog.list(fromMs, toMs);
    },
  },
  sessionSecret: randomBytes(32).toString("base64"),
  siteTz: TZ,
});
const server = app.listen(0, "127.0.0.1");
await once(server, "listening");
const base = `http://127.0.0.1:${server.address().port}`;

const local = new Intl.DateTimeFormat("sv-SE", {
  timeZone: TZ,
  dateStyle: "short",
  timeStyle: "medium",
});
const show = (ms) => `${new Date(ms).toISOString()} (${local.format(ms)} Toronto)`;

try {
  const post = (path, body, cookie) =>
    fetch(`${base}${path}`, {
      method: "POST",
      headers: { "content-type": "application/json", ...(cookie ? { cookie } : {}) },
      body: JSON.stringify(body),
    });

  const opened = await post("/auth/link", { key: KEY });
  const setCookie = opened.headers.getSetCookie()[0];
  console.log(`POST /auth/link ${opened.status} ${JSON.stringify(await opened.json())}`);
  console.log(`  Set-Cookie: ${setCookie.replace(/=[^;]+;/, "=<token>;")}`);
  const cookie = setCookie.split(";")[0];
  const session = await fetch(`${base}/session`, { headers: { cookie } });
  console.log(`GET /session ${session.status} ${JSON.stringify(await session.json())}`);
  const unknown = await post("/auth/link", { key: randomBytes(32).toString("base64url") });
  console.log(`POST /auth/link with a key nobody has: ${unknown.status} ${JSON.stringify(await unknown.json())}`);

  let started = Date.now();
  const res = await fetch(`${base}/telemetry?days=${DAYS}`, { headers: { cookie } });
  const body = await res.json();
  const firstMs = Date.now() - started;
  started = Date.now();
  await fetch(`${base}/telemetry?days=${DAYS}`, { headers: { cookie } });
  const cachedMs = Date.now() - started;

  const { rows, statuses } = body;
  console.log(`GET /telemetry?days=${DAYS}: ${res.status}, cache-control "${res.headers.get("cache-control")}"`);
  console.log(`  took ${firstMs} ms cold, ${cachedMs} ms cached`);
  console.log(`  generatedAt ${body.generatedAt}, lastUploadAt ${body.lastUploadAt}`);
  console.log(`  rows ${rows.length}`);
  if (rows.length === 0) process.exit(1);
  console.log(`  first ${show(rows[0][0])} ${statuses[rows[0][4]]}`);
  console.log(`  last  ${show(rows.at(-1)[0])} ${statuses[rows.at(-1)[4]]}`);

  const since = Date.parse(body.generatedAt) - Number(DAYS) * 24 * 60 * 60 * 1000;
  const ascending = rows.every((row, i) => i === 0 || row[0] > rows[i - 1][0]);
  console.log(`  strictly ascending: ${ascending}; all inside the window: ${rows.every((row) => row[0] >= since)}`);

  const counts = {};
  for (const row of rows) counts[statuses[row[4]]] = (counts[statuses[row[4]]] ?? 0) + 1;
  console.log(`  by status: ${JSON.stringify(counts)}`);

  // From the request log, not the bucket: each one halfway between the two syncs
  // either side of it, so within about 7.5 minutes of when it happened.
  const { calibrations } = body;
  console.log(`  calibrations ${calibrations.length}`);
  for (const [t, count] of calibrations) console.log(`    ${show(t)} x${count}`);
  console.log(`  request log reads for both requests: ${logReads.length} (${logReads.join("; ")})`);

  // When each local day's ACTIVE stretch began and ended: should be around sunrise
  // and sunset in Toronto, not shifted four hours either way.
  const byDay = new Map();
  for (const row of rows) {
    if (statuses[row[4]] !== "ACTIVE") continue;
    const [day, time] = local.format(row[0]).split(" ");
    const span = byDay.get(day) ?? { from: time, to: time };
    span.to = time;
    byDay.set(day, span);
  }
  for (const [day, { from, to }] of byDay) console.log(`  ACTIVE on ${day}: ${from} to ${to} Toronto time`);

  const gaps = rows.slice(1).map((row, i) => row[0] - rows[i][0]);
  gaps.sort((a, b) => a - b);
  console.log(
    `  spacing: median ${gaps[Math.floor(gaps.length / 2)] / 1000} s, largest ${(gaps.at(-1) / 60000).toFixed(1)} min`,
  );

  // The newest object's last record, raw, against the row it became.
  const today = new Date().toISOString().slice(0, 10);
  const names = (await list(`raw/${today}/`)).filter((name) => uploadTime(name) !== null);
  if (names.length) {
    const newest = names.at(-1);
    const last = JSON.parse((await read(newest)).trim().split("\n").at(-1));
    const row = rows.find((r) => local.format(r[0]).replace(" ", "T") === last.time.slice(0, 19));
    console.log(`  newest object ${newest}`);
    console.log(`    stored time ${last.time} (Toronto wall clock) -> row ${row ? show(row[0]) : "NOT FOUND"}`);

    // A row cannot be uploaded before it was taken. Had the conversion used the
    // wrong offset, the newest row of an upload would land after the upload, or an
    // hour too early. Checked over today's uploads.
    const lags = [];
    for (const name of names) {
      const times = (await read(name))
        .trim()
        .split("\n")
        .map((line) => JSON.parse(line).time.slice(0, 19));
      const newestRow = rows.filter((r) => times.includes(local.format(r[0]).replace(" ", "T"))).at(-1);
      if (newestRow) lags.push((uploadTime(name) - newestRow[0]) / 60000);
    }
    lags.sort((a, b) => a - b);
    console.log(
      `    today's ${lags.length} uploads arrive ${lags[0].toFixed(1)} to ${lags.at(-1).toFixed(1)} min (median ${lags[Math.floor(lags.length / 2)].toFixed(1)}) after their newest row`,
    );
  }

  const out = await post("/auth/logout", {}, cookie);
  console.log(`POST /auth/logout ${out.status}; Set-Cookie ${out.headers.get("set-cookie")}`);
  // Signing out only makes the browser drop its cookie; nothing is recorded on the
  // server, so a copy of the token kept elsewhere still works until the link is
  // re-issued. That is the revocation, not this.
  const after = await fetch(`${base}/telemetry?days=${DAYS}`, { headers: { cookie } });
  console.log(`  the dropped cookie, sent again anyway: ${after.status} (200 expected; see the comment)`);
} finally {
  server.closeAllConnections();
  server.close();
}
