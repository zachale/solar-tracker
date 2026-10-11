// The whole sign-in flow over real HTTP, against in-memory buckets and a clock the
// tests move by hand. Each test starts its own server, so they run concurrently:
// POST /auth/request takes at least half a second on purpose.
import { describe, test } from "node:test";
import assert from "node:assert/strict";
import { once } from "node:events";

import { createApp } from "../src/app.js";
import { requestId } from "../src/requests.js";
import { createMemoryStore } from "../src/stores.js";

const SECOND = 1000;
const MINUTE = 60 * SECOND;
const HOUR = 60 * MINUTE;
const DAY = 24 * HOUR;
const START = Date.parse("2026-10-11T16:00:00.000Z");

const ZACH = { phone: "+15195550100", name: "Zach", admin: true };
const SAM = { phone: "+15195550101", name: "Sam", admin: false };
const ANN = { phone: "+15195550102", name: "Ann", admin: false };
const STRANGER = "+15195550199";

const ATTRIBUTES = "Path=/; HttpOnly; Secure; SameSite=Lax";

// Toronto wall clock, as the tracker stamps it.
const reading = (wall, status = "ACTIVE") =>
  JSON.stringify({
    time: `${wall}.000Z`,
    temperature: 12.5,
    extended_percent: 40,
    status,
    wind_peak: 2,
    button_panel: { settings: false, wind_sensor: true, away: false },
  });
const TELEMETRY = {
  // 08:00 EDT on 4 October is 12:00Z, more than seven days before START;
  // midnight EDT on 5 October is 04:00Z, inside them.
  "raw/2026-10-05/2026-10-05T05-00-00-000Z.ndjson": `${reading("2026-10-04T08:00:00")}\n${reading("2026-10-05T00:00:00", "NIGHT")}\n`,
  "raw/2026-10-11/2026-10-11T15-00-00-000Z.ndjson": `${reading("2026-10-11T10:55:00")}\n`,
};

// The bucket as seen over a network: every call takes a little while, so requests
// handled at the same time really do interleave.
function withLatency(store, ms) {
  const later = (fn) => async (...args) => {
    await new Promise((resolve) => setTimeout(resolve, ms));
    return fn(...args);
  };
  return { list: later(store.list), read: later(store.read), write: later(store.write), delete: later(store.delete) };
}

async function startPortal(t, { users = [ZACH, SAM, ANN], latencyMs = 0 } = {}) {
  const clock = { now: START };
  const portalStore = createMemoryStore({ "users.json": JSON.stringify({ users }) });
  const app = createApp({
    telemetryStore: createMemoryStore(TELEMETRY),
    portalStore: latencyMs ? withLatency(portalStore, latencyMs) : portalStore,
    sessionSecret: "test-secret",
    siteTz: "America/Toronto",
    now: () => clock.now,
  });
  const server = app.listen(0, "127.0.0.1");
  await once(server, "listening");
  t.after(() => {
    server.closeAllConnections();
    server.close();
  });
  const base = `http://127.0.0.1:${server.address().port}`;

  // A browser's cookie jar, kept from Set-Cookie. It never expires a cookie by
  // itself (the tests move the clock, not time), so it also stands in for a
  // client that ignores Max-Age.
  function browser() {
    const jar = new Map();
    async function call(method, path, { json, body, type } = {}) {
      const headers = {};
      let payload;
      if (json !== undefined) {
        payload = JSON.stringify(json);
        headers["content-type"] = "application/json";
      } else if (body !== undefined) {
        payload = body;
      }
      if (type !== undefined) headers["content-type"] = type;
      if (jar.size) headers.cookie = [...jar].map(([name, value]) => `${name}=${value}`).join("; ");

      const res = await fetch(base + path, { method, headers, body: payload });
      const setCookies = res.headers.getSetCookie();
      for (const line of setCookies) {
        const [pair] = line.split(";");
        const eq = pair.indexOf("=");
        if (line.endsWith("Max-Age=0")) jar.delete(pair.slice(0, eq));
        else jar.set(pair.slice(0, eq), pair.slice(eq + 1));
      }
      const text = await res.text();
      let data = text;
      try {
        data = JSON.parse(text);
      } catch {}
      return { status: res.status, data, setCookies, headers: res.headers };
    }
    // The id the admin sees for this browser's request.
    const id = () => requestId(jar.get("tracker_request"));
    return { call, jar, id };
  }

  const requestObjects = async () => portalStore.list("requests/");
  const readRequest = async (id) => {
    const text = await portalStore.read(`requests/${id}.json`);
    return text === null ? null : JSON.parse(text);
  };
  // What tools/portal/requests.sh does: rewrite the object with an answer.
  const answerInBucket = async (id, status) => {
    const request = await readRequest(id);
    await portalStore.write(`requests/${id}.json`, `${JSON.stringify({ ...request, status })}\n`);
  };

  async function signIn(user, { remember = true, as = browser() } = {}) {
    assert.equal((await as.call("POST", "/auth/request", { json: { phone: user.phone, remember } })).status, 202);
    await answerInBucket(as.id(), "approved");
    const res = await as.call("GET", "/auth/request");
    assert.equal(res.data.status, "approved");
    return { ...as, res };
  }

  return { clock, portalStore, browser, requestObjects, readRequest, answerInBucket, signIn };
}

describe("portal API", { concurrency: true }, () => {
  test("request, wait, approve, sign in, read telemetry, sign out", async (t) => {
    const portal = await startPortal(t);

    // Zach's own first sign-in, approved in the bucket with tools/portal/requests.sh.
    const zach = portal.browser();
    assert.deepEqual((await zach.call("GET", "/session")).data, { error: "unauthenticated" });
    const asked = await zach.call("POST", "/auth/request", { json: { phone: "(519) 555-0100", remember: true } });
    assert.equal(asked.status, 202);
    assert.deepEqual(asked.data, { ok: true });
    assert.equal(asked.setCookies.length, 1);
    assert.match(asked.setCookies[0], new RegExp(`^tracker_request=[\\w-]{43}; ${ATTRIBUTES}; Max-Age=86400$`));
    assert.deepEqual(await portal.readRequest(zach.id()), {
      id: zach.id(),
      phone: ZACH.phone,
      name: "Zach",
      remember: true,
      requestedAt: "2026-10-11T16:00:00.000Z",
      expiresAt: "2026-10-12T16:00:00.000Z",
      status: "pending",
    });
    assert.deepEqual((await zach.call("GET", "/auth/request")).data, { status: "pending" });

    await portal.answerInBucket(zach.id(), "approved");
    const zachIn = await zach.call("GET", "/auth/request");
    assert.deepEqual(zachIn.data, { status: "approved", name: "Zach", admin: true });
    assert.deepEqual(zachIn.setCookies.length, 2);
    assert.equal(zachIn.setCookies[0], `tracker_request=; ${ATTRIBUTES}; Max-Age=0`);
    assert.match(zachIn.setCookies[1], new RegExp(`^tracker_session=[\\w-]+\\.[\\w-]+; ${ATTRIBUTES}; Max-Age=7776000$`));
    assert.deepEqual(await portal.requestObjects(), [], "a collected request is gone");
    assert.deepEqual((await zach.call("GET", "/session")).data, { name: "Zach", admin: true });

    // Sam asks; Zach sees it in the admin view and approves it there.
    const sam = portal.browser();
    portal.clock.now += MINUTE;
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    const listed = await zach.call("GET", "/admin/requests");
    assert.equal(listed.status, 200);
    assert.deepEqual(listed.data, {
      requests: [
        {
          id: sam.id(),
          name: "Sam",
          phone: SAM.phone,
          requestedAt: "2026-10-11T16:01:00.000Z",
          expiresAt: "2026-10-12T16:01:00.000Z",
        },
      ],
    });
    assert.deepEqual((await sam.call("GET", "/auth/request")).data, { status: "pending" });

    const approved = await zach.call("POST", `/admin/requests/${sam.id()}/approve`, { json: {} });
    assert.equal(approved.status, 200);
    assert.deepEqual(approved.data, { ok: true });
    assert.equal((await portal.readRequest(sam.id())).decidedAt, "2026-10-11T16:01:00.000Z");
    assert.deepEqual((await zach.call("GET", "/admin/requests")).data, { requests: [] });

    const samIn = await sam.call("GET", "/auth/request");
    assert.deepEqual(samIn.data, { status: "approved", name: "Sam", admin: false });
    // Sam did not ask to be remembered: a browser-session cookie.
    assert.match(samIn.setCookies[1], new RegExp(`^tracker_session=[\\w-]+\\.[\\w-]+; ${ATTRIBUTES}$`));
    assert.deepEqual((await sam.call("GET", "/auth/request")).data, { error: "no_request" });

    const telemetry = await sam.call("GET", "/telemetry");
    assert.equal(telemetry.status, 200);
    assert.equal(telemetry.headers.get("cache-control"), "private, max-age=30");
    assert.deepEqual(telemetry.data, {
      generatedAt: "2026-10-11T16:01:00.000Z",
      lastUploadAt: "2026-10-11T15:00:00.000Z",
      statuses: ["ACTIVE", "NIGHT", "SAFE", "AWAY"],
      rows: [
        [Date.parse("2026-10-05T04:00:00Z"), 40, 12.5, 2, 1, 1],
        [Date.parse("2026-10-11T14:55:00Z"), 40, 12.5, 2, 0, 1],
      ],
    });
    assert.equal((await sam.call("GET", "/session")).headers.get("cache-control"), "no-store");

    const out = await sam.call("POST", "/auth/logout", { json: {} });
    assert.equal(out.status, 204);
    assert.deepEqual(out.setCookies, [`tracker_session=; ${ATTRIBUTES}; Max-Age=0`]);
    assert.equal((await sam.call("GET", "/session")).status, 401);
    assert.equal((await sam.call("GET", "/telemetry")).status, 401);
  });

  test("a denied request ends in denied, with no session", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    const sam = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone, remember: true } });

    assert.deepEqual((await zach.call("POST", `/admin/requests/${sam.id()}/deny`, { json: {} })).data, { ok: true });
    const answer = await sam.call("GET", "/auth/request");
    assert.deepEqual(answer.data, { status: "denied" });
    assert.deepEqual(answer.setCookies, [`tracker_request=; ${ATTRIBUTES}; Max-Age=0`]);
    assert.deepEqual(await portal.requestObjects(), []);
    assert.equal((await sam.call("GET", "/session")).status, 401);
    assert.deepEqual((await sam.call("GET", "/auth/request")).data, { error: "no_request" });
  });

  test("an answer can be changed until it is collected", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    const sam = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    await zach.call("POST", `/admin/requests/${sam.id()}/approve`, { json: {} });
    await zach.call("POST", `/admin/requests/${sam.id()}/deny`, { json: {} });
    assert.deepEqual((await sam.call("GET", "/auth/request")).data, { status: "denied" });
  });

  test("a request lasts 24 hours", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);

    const sam = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    portal.clock.now += DAY - SECOND;
    assert.deepEqual((await sam.call("GET", "/auth/request")).data, { status: "pending" });
    portal.clock.now += SECOND;
    const expired = await sam.call("GET", "/auth/request");
    assert.deepEqual(expired.data, { status: "expired" });
    assert.deepEqual(expired.setCookies, [`tracker_request=; ${ATTRIBUTES}; Max-Age=0`]);
    assert.deepEqual(await portal.requestObjects(), []);

    // Too late to answer, and an approval not collected in time lapses too.
    const ann = portal.browser();
    await ann.call("POST", "/auth/request", { json: { phone: ANN.phone } });
    const late = portal.browser();
    await late.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    await zach.call("POST", `/admin/requests/${ann.id()}/approve`, { json: {} });
    portal.clock.now += DAY;
    assert.deepEqual((await ann.call("GET", "/auth/request")).data, { status: "expired" });
    assert.equal(ann.jar.has("tracker_session"), false);
    const tooLate = await zach.call("POST", `/admin/requests/${late.id()}/approve`, { json: {} });
    assert.equal(tooLate.status, 404);
    assert.deepEqual(tooLate.data, { error: "not_found" });
  });

  test("a number not on the list gets the same answers and waits forever", async (t) => {
    const portal = await startPortal(t);
    const sam = portal.browser();
    const stranger = portal.browser();
    const real = await sam.call("POST", "/auth/request", { json: { phone: SAM.phone, remember: true } });
    const decoy = await stranger.call("POST", "/auth/request", { json: { phone: STRANGER, remember: true } });

    assert.equal(decoy.status, real.status);
    assert.deepEqual(decoy.data, real.data);
    const shape = (setCookies) => setCookies.map((line) => line.replace(/=[\w-]{43};/, "=<secret>;"));
    assert.deepEqual(shape(decoy.setCookies), shape(real.setCookies));
    assert.notEqual(stranger.jar.get("tracker_request"), sam.jar.get("tracker_request"));

    assert.deepEqual(await portal.requestObjects(), [`requests/${sam.id()}.json`], "nothing filed for the stranger");
    assert.deepEqual((await stranger.call("GET", "/auth/request")).data, { status: "pending" });
    portal.clock.now += 30 * DAY;
    assert.deepEqual((await stranger.call("GET", "/auth/request")).data, { status: "pending" });
    assert.equal((await stranger.call("POST", "/auth/request/cancel", { json: {} })).status, 204);
  });

  test("at most three requests wait per number", async (t) => {
    const portal = await startPortal(t, { latencyMs: 20 });
    const zach = await portal.signIn(ZACH);
    const browsers = [portal.browser(), portal.browser(), portal.browser(), portal.browser()];

    // Sent together: the cap still holds.
    const answers = await Promise.all(
      browsers.map((b) => b.call("POST", "/auth/request", { json: { phone: SAM.phone } })),
    );
    assert.ok(answers.every((res) => res.status === 202 && res.setCookies.length === 1));
    assert.equal((await portal.requestObjects()).length, 3);
    for (const b of browsers) assert.deepEqual((await b.call("GET", "/auth/request")).data, { status: "pending" });

    // An answered request no longer counts.
    const filed = [];
    for (const b of browsers) if (await portal.readRequest(b.id())) filed.push(b);
    await zach.call("POST", `/admin/requests/${filed[0].id()}/deny`, { json: {} });
    const fifth = portal.browser();
    await fifth.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    assert.ok(await portal.readRequest(fifth.id()));
    assert.equal((await zach.call("GET", "/admin/requests")).data.requests.length, 3);
  });

  test("asking again from the same browser replaces the earlier request", async (t) => {
    const portal = await startPortal(t);
    const sam = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    const first = sam.id();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone, remember: true } });
    assert.notEqual(sam.id(), first);
    assert.deepEqual(await portal.requestObjects(), [`requests/${sam.id()}.json`]);
    assert.equal((await portal.readRequest(sam.id())).remember, true);
  });

  test("cancelling forgets the request", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    const sam = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    const id = sam.id();

    const cancelled = await sam.call("POST", "/auth/request/cancel", { json: {} });
    assert.equal(cancelled.status, 204);
    assert.deepEqual(cancelled.setCookies, [`tracker_request=; ${ATTRIBUTES}; Max-Age=0`]);
    assert.deepEqual(await portal.requestObjects(), []);
    assert.deepEqual((await sam.call("GET", "/auth/request")).data, { error: "no_request" });
    assert.equal((await zach.call("POST", `/admin/requests/${id}/approve`, { json: {} })).status, 404);
    // Nothing to cancel is not an error.
    assert.equal((await portal.browser().call("POST", "/auth/request/cancel", { json: {} })).status, 204);
  });

  test("an approval is collected only by the browser that asked", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    const sam = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    await zach.call("POST", `/admin/requests/${sam.id()}/approve`, { json: {} });

    // Someone who has the public id (from the admin page or the bucket) presents it.
    const thief = portal.browser();
    thief.jar.set("tracker_request", sam.id());
    const stolen = await thief.call("GET", "/auth/request");
    assert.deepEqual(stolen.data, { status: "pending" });
    assert.equal(thief.jar.has("tracker_session"), false);

    assert.deepEqual((await sam.call("GET", "/auth/request")).data, { status: "approved", name: "Sam", admin: false });
  });

  test("someone removed from the list after approval is not signed in", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    const sam = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    await zach.call("POST", `/admin/requests/${sam.id()}/approve`, { json: {} });

    await portal.portalStore.write("users.json", JSON.stringify({ users: [ZACH, ANN] }));
    portal.clock.now += 31 * SECOND;
    const answer = await sam.call("GET", "/auth/request");
    assert.deepEqual(answer.data, { status: "denied" });
    assert.equal(sam.jar.has("tracker_session"), false);
  });

  test("admin routes need a signed-in admin", async (t) => {
    const portal = await startPortal(t);
    const id = "a".repeat(64);
    const nobody = portal.browser();
    assert.equal((await nobody.call("GET", "/admin/requests")).status, 401);
    assert.equal((await nobody.call("POST", `/admin/requests/${id}/approve`, { json: {} })).status, 401);

    const sam = await portal.signIn(SAM);
    for (const [method, path] of [
      ["GET", "/admin/requests"],
      ["POST", `/admin/requests/${id}/approve`],
      ["POST", `/admin/requests/${id}/deny`],
    ]) {
      const res = await sam.call(method, path, method === "POST" ? { json: {} } : {});
      assert.equal(res.status, 403, path);
      assert.deepEqual(res.data, { error: "forbidden" });
    }
  });

  test("request ids are checked", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    for (const id of ["abc", "A".repeat(64), "g".repeat(64), "a".repeat(63), "a".repeat(65)]) {
      const res = await zach.call("POST", `/admin/requests/${id}/approve`, { json: {} });
      assert.equal(res.status, 400, id);
      assert.deepEqual(res.data, { error: "invalid_id" });
    }
    for (const decision of ["approve", "deny"]) {
      const res = await zach.call("POST", `/admin/requests/${"0".repeat(64)}/${decision}`, { json: {} });
      assert.equal(res.status, 404);
      assert.deepEqual(res.data, { error: "not_found" });
    }
  });

  test("the admin list shows waiting requests newest first and clears expired ones", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    const sam = portal.browser();
    const ann = portal.browser();
    await sam.call("POST", "/auth/request", { json: { phone: SAM.phone } });
    portal.clock.now += HOUR;
    await ann.call("POST", "/auth/request", { json: { phone: ANN.phone } });

    const names = async () => (await zach.call("GET", "/admin/requests")).data.requests.map((r) => r.name);
    assert.deepEqual(await names(), ["Ann", "Sam"]);
    portal.clock.now += 23 * HOUR;
    assert.deepEqual(await names(), ["Ann"]);
    assert.equal(await portal.readRequest(sam.id()), null, "the expired request was deleted");
  });

  test("without remember me the session is a browser-session cookie that lasts 12 hours", async (t) => {
    const portal = await startPortal(t);
    const sam = await portal.signIn(SAM, { remember: false });
    assert.match(sam.res.setCookies[1], new RegExp(`^tracker_session=[^;]+; ${ATTRIBUTES}$`));

    portal.clock.now += 12 * HOUR - SECOND;
    const still = await sam.call("GET", "/session");
    assert.equal(still.status, 200);
    assert.deepEqual(still.setCookies, [], "a short session is never renewed");

    portal.clock.now += SECOND;
    const over = await sam.call("GET", "/session");
    assert.equal(over.status, 401);
    assert.deepEqual(over.setCookies, [`tracker_session=; ${ATTRIBUTES}; Max-Age=0`]);
  });

  test("a remembered session is renewed once it is a day old", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH, { remember: true });
    const original = zach.jar.get("tracker_session");

    portal.clock.now += 23 * HOUR;
    assert.deepEqual((await zach.call("GET", "/session")).setCookies, []);

    portal.clock.now += 2 * HOUR;
    const renewed = await zach.call("GET", "/telemetry?days=1");
    assert.equal(renewed.status, 200);
    assert.match(renewed.setCookies[0], /^tracker_session=.+; Max-Age=7776000$/);
    assert.notEqual(zach.jar.get("tracker_session"), original);

    // Ninety days after signing in the original cookie is dead, but the renewed
    // one, issued a day later, still has a day to run.
    portal.clock.now = START + 90 * DAY;
    assert.equal((await zach.call("GET", "/session")).status, 200);
    const stale = portal.browser();
    stale.jar.set("tracker_session", original);
    assert.equal((await stale.call("GET", "/session")).status, 401);
  });

  test("removing someone from users.json signs them out within 30 seconds", async (t) => {
    const portal = await startPortal(t);
    const sam = await portal.signIn(SAM);
    assert.equal((await sam.call("GET", "/session")).status, 200);

    await portal.portalStore.write("users.json", JSON.stringify({ users: [ZACH, ANN] }));
    portal.clock.now += 29 * SECOND;
    assert.equal((await sam.call("GET", "/session")).status, 200, "still cached");
    portal.clock.now += 2 * SECOND;
    const res = await sam.call("GET", "/telemetry");
    assert.equal(res.status, 401);
    assert.deepEqual(res.data, { error: "unauthenticated" });
    assert.deepEqual(res.setCookies, [`tracker_session=; ${ATTRIBUTES}; Max-Age=0`]);
  });

  test("a session cookie that is forged or garbled is refused and cleared", async (t) => {
    const portal = await startPortal(t);
    const sam = await portal.signIn(SAM);
    const [payload, signature] = sam.jar.get("tracker_session").split(".");
    const claims = JSON.parse(Buffer.from(payload, "base64url").toString());
    const promoted = Buffer.from(JSON.stringify({ ...claims, p: ZACH.phone })).toString("base64url");

    for (const value of [`${promoted}.${signature}`, "garbage", ""]) {
      const forger = portal.browser();
      forger.jar.set("tracker_session", value);
      const res = await forger.call("GET", "/admin/requests");
      assert.equal(res.status, 401, value);
      assert.deepEqual(res.setCookies, [`tracker_session=; ${ATTRIBUTES}; Max-Age=0`]);
    }
  });

  test("every POST must be JSON", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    const id = "a".repeat(64);
    for (const path of [
      "/auth/request",
      "/auth/request/cancel",
      "/auth/logout",
      `/admin/requests/${id}/approve`,
      `/admin/requests/${id}/deny`,
    ]) {
      for (const [type, body] of [
        ["application/x-www-form-urlencoded", `phone=${SAM.phone}`],
        ["text/plain", JSON.stringify({ phone: SAM.phone })],
        ["multipart/form-data; boundary=x", "--x--"],
        [undefined, undefined],
      ]) {
        const res = await zach.call("POST", path, { body, type });
        assert.equal(res.status, 415, `${path} ${type}`);
        assert.deepEqual(res.data, { error: "unsupported_media_type" });
      }
    }
    assert.deepEqual(await portal.requestObjects(), []);
    assert.equal((await zach.call("GET", "/session")).status, 200, "still signed in");

    // A JSON POST needs no body.
    assert.equal((await zach.call("POST", "/auth/request/cancel", { type: "application/json" })).status, 204);
    // Broken JSON is a 400.
    const broken = await zach.call("POST", "/auth/request", { body: "{phone:", type: "application/json" });
    assert.equal(broken.status, 400);
    assert.deepEqual(broken.data, { error: "bad_request" });
  });

  test("a malformed number is a 400 and gets no cookie", async (t) => {
    const portal = await startPortal(t);
    const anyone = portal.browser();
    for (const phone of ["12345", "", "call me", 5195550100, null, undefined]) {
      const res = await anyone.call("POST", "/auth/request", { json: { phone } });
      assert.equal(res.status, 400, String(phone));
      assert.deepEqual(res.data, { error: "invalid_phone" });
      assert.deepEqual(res.setCookies, []);
    }
  });

  test("days must be a whole number from 1 to 60, 7 by default", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.signIn(ZACH);
    for (const days of ["0", "61", "abc", "1.5", "-1", "", "007", "7&days=8"]) {
      const res = await zach.call("GET", `/telemetry?days=${days}`);
      assert.equal(res.status, 400, days);
      assert.deepEqual(res.data, { error: "invalid_days" });
    }
    assert.equal((await zach.call("GET", "/telemetry?days=60")).data.rows.length, 3);
    assert.equal((await zach.call("GET", "/telemetry?days=1")).data.rows.length, 1);
    assert.equal((await zach.call("GET", "/telemetry")).data.rows.length, 2);
  });

  test("health needs no sign-in, and unknown paths are 404", async (t) => {
    const portal = await startPortal(t);
    const anyone = portal.browser();
    const health = await anyone.call("GET", "/health");
    assert.equal(health.status, 200);
    assert.equal(health.data, "ok");
    assert.equal((await anyone.call("GET", "/nope")).status, 404);
    assert.deepEqual((await anyone.call("GET", "/auth/request")).data, { error: "no_request" });
  });

  test("a missing users.json lets nobody in", async (t) => {
    const portal = await startPortal(t);
    await portal.portalStore.delete("users.json");
    const zach = portal.browser();
    const res = await zach.call("POST", "/auth/request", { json: { phone: ZACH.phone } });
    assert.equal(res.status, 202);
    assert.deepEqual(await portal.requestObjects(), []);
  });
});
