// Runs the portal API on this machine against the real telemetry bucket, read-only,
// with users.json and the sign-in requests held in memory. It asks to sign in over
// HTTP the way the page does, approves the request the way tools/portal/requests.sh
// would, and summarizes what /telemetry returns. Nothing is written to any bucket.
//
//   node scripts/smoke.js [days]        default 2
//
// Uses the gcloud CLI's own login (gcloud auth print-access-token), so it works
// without application default credentials.
import { execFileSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import { once } from "node:events";

import { Storage } from "@google-cloud/storage";
// Installed with @google-cloud/storage; used only here, to hand it the CLI's token.
import { OAuth2Client } from "google-auth-library";

import { createApp } from "../src/app.js";
import { requestId } from "../src/requests.js";
import { createGcsStore, createMemoryStore } from "../src/stores.js";
import { uploadTime } from "../src/telemetry.js";

const DAYS = process.argv[2] ?? "2";
const TZ = "America/Toronto";
const BUCKET = "solar-tracker-442903-telemetry";
const PHONE = "+15195550100"; // a reserved 555 number

const accessToken = execFileSync("gcloud", ["auth", "print-access-token"], {
  stdio: ["ignore", "pipe", "inherit"],
})
  .toString()
  .trim();
const authClient = new OAuth2Client();
authClient.setCredentials({ access_token: accessToken });
const storage = new Storage({ authClient, projectId: "solar-tracker-442903" });

// Only list and read are passed on, so a write to the real bucket would throw here.
const { list, read } = createGcsStore(storage.bucket(BUCKET));
const portalStore = createMemoryStore({
  "users.json": JSON.stringify({ users: [{ phone: PHONE, name: "Smoke test", admin: true }] }),
});
const app = createApp({
  telemetryStore: { list, read },
  portalStore,
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
  const asked = await fetch(`${base}/auth/request`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ phone: PHONE, remember: true }),
  });
  const requestCookie = asked.headers.get("set-cookie").split(";")[0];
  const poll = () => fetch(`${base}/auth/request`, { headers: { cookie: requestCookie } });
  const waiting = await (await poll()).json();

  const name = `requests/${requestId(requestCookie.split("=")[1])}.json`;
  const request = JSON.parse(await portalStore.read(name));
  await portalStore.write(name, `${JSON.stringify({ ...request, status: "approved" })}\n`);
  const approved = await poll();
  console.log(
    `POST /auth/request ${asked.status}; GET ${JSON.stringify(waiting)}; after approval ${JSON.stringify(await approved.json())}`,
  );
  const cookie = approved.headers.getSetCookie().find((line) => line.startsWith("tracker_session=")).split(";")[0];

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
} finally {
  server.closeAllConnections();
  server.close();
}
