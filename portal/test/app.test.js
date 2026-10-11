// The whole sign-in flow over real HTTP, against in-memory buckets and a clock the
// tests move by hand. Each test starts its own server, so they run concurrently.
import { describe, test } from "node:test";
import assert from "node:assert/strict";
import { createHmac, randomBytes } from "node:crypto";
import { once } from "node:events";

import { createApp } from "../src/app.js";
import { createMemoryStore } from "../src/stores.js";
import { hashKey } from "../src/users.js";

const SECOND = 1000;
const MINUTE = 60 * SECOND;
const HOUR = 60 * MINUTE;
const DAY = 24 * HOUR;
const START = Date.parse("2026-10-11T16:00:00.000Z");
const SECRET = "test-secret";

// Someone with a personal link, as tools/portal/users.sh makes one: the key goes in
// the link, only its hash goes in users.json.
function person(id, name) {
  const key = randomBytes(32).toString("base64url");
  return { id, name, key, keyHash: hashKey(key) };
}
const ZACH = person("zach", "Zach");
const UNCLE = person("uncle", "Uncle Dave");
const usersJson = (people) =>
  JSON.stringify({
    users: people.map(({ id, name, keyHash }) => ({ id, name, keyHash, issuedAt: "2026-10-01T12:00:00.000Z" })),
  });

const ATTRIBUTES = "Path=/; HttpOnly; Secure; SameSite=Lax";
const CLEARED = `tracker_session=; ${ATTRIBUTES}; Max-Age=0`;
const claimsOf = (token) => JSON.parse(Buffer.from(token.split(".")[0], "base64url").toString());

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

async function startPortal(t, { people = [ZACH, UNCLE] } = {}) {
  const clock = { now: START };
  const portalStore = createMemoryStore({ "users.json": usersJson(people) });
  const app = createApp({
    telemetryStore: createMemoryStore(TELEMETRY),
    portalStore,
    sessionSecret: SECRET,
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
    return { call, jar };
  }

  // What the page does with #key=... from the address bar.
  async function open(who, as = browser()) {
    const res = await as.call("POST", "/auth/link", { json: { key: who.key } });
    assert.equal(res.status, 200);
    assert.deepEqual(res.data, { name: who.name });
    return as;
  }

  // What tools/portal/users.sh does: rewrite users.json.
  const setPeople = (people) => portalStore.write("users.json", usersJson(people));

  return { clock, portalStore, browser, open, setPeople };
}

describe("portal API", { concurrency: true }, () => {
  test("open a link, read telemetry, sign out", async (t) => {
    const portal = await startPortal(t);
    const phone = portal.browser();
    assert.deepEqual((await phone.call("GET", "/session")).data, { error: "unauthenticated" });

    const opened = await phone.call("POST", "/auth/link", { json: { key: UNCLE.key } });
    assert.equal(opened.status, 200);
    assert.deepEqual(opened.data, { name: "Uncle Dave" });
    assert.equal(opened.setCookies.length, 1);
    // Always persistent: no "remember me", and no login screen next time.
    assert.match(opened.setCookies[0], new RegExp(`^tracker_session=[\\w-]+\\.[\\w-]+; ${ATTRIBUTES}; Max-Age=34560000$`));
    assert.deepEqual(claimsOf(phone.jar.get("tracker_session")), {
      u: "uncle",
      k: UNCLE.keyHash.slice(0, 16),
      iat: START,
      exp: START + 400 * DAY,
    });
    assert.ok(!opened.setCookies[0].includes(UNCLE.key), "the key itself is not in the cookie");

    const session = await phone.call("GET", "/session");
    assert.equal(session.status, 200);
    assert.deepEqual(session.data, { name: "Uncle Dave" });
    assert.equal(session.headers.get("cache-control"), "no-store");

    const telemetry = await phone.call("GET", "/telemetry");
    assert.equal(telemetry.status, 200);
    assert.equal(telemetry.headers.get("cache-control"), "private, max-age=30");
    assert.deepEqual(telemetry.data, {
      generatedAt: "2026-10-11T16:00:00.000Z",
      lastUploadAt: "2026-10-11T15:00:00.000Z",
      statuses: ["ACTIVE", "NIGHT", "SAFE", "AWAY"],
      rows: [
        [Date.parse("2026-10-05T04:00:00Z"), 40, 12.5, 2, 1, 1],
        [Date.parse("2026-10-11T14:55:00Z"), 40, 12.5, 2, 0, 1],
      ],
    });

    const out = await phone.call("POST", "/auth/logout", { json: {} });
    assert.equal(out.status, 204);
    assert.deepEqual(out.setCookies, [CLEARED]);
    assert.equal((await phone.call("GET", "/session")).status, 401);
    assert.equal((await phone.call("GET", "/telemetry")).status, 401);

    // Signing out a device does not spend the link.
    await portal.open(UNCLE, phone);
    assert.equal((await phone.call("GET", "/session")).status, 200);
  });

  test("one link signs in any number of devices, each as its person", async (t) => {
    const portal = await startPortal(t);
    const phone = await portal.open(UNCLE);
    const laptop = await portal.open(UNCLE);
    const zach = await portal.open(ZACH);
    assert.deepEqual((await phone.call("GET", "/session")).data, { name: "Uncle Dave" });
    assert.deepEqual((await laptop.call("GET", "/session")).data, { name: "Uncle Dave" });
    assert.deepEqual((await zach.call("GET", "/session")).data, { name: "Zach" });

    // Opening another person's link on a signed-in device switches it to them.
    await portal.open(ZACH, laptop);
    assert.deepEqual((await laptop.call("GET", "/session")).data, { name: "Zach" });
  });

  test("a key that is not base64url of 20 to 100 characters is a 400 with no cookie", async (t) => {
    const portal = await startPortal(t);
    const anyone = portal.browser();
    for (const key of [
      "short",
      "a".repeat(19),
      "a".repeat(101),
      `${UNCLE.key}=`,
      UNCLE.key.replace(/^./, "+"),
      UNCLE.key.replace(/^./, "/"),
      ` ${UNCLE.key}`,
      "",
      42,
      null,
      true,
      [UNCLE.key],
      { key: UNCLE.key },
    ]) {
      const res = await anyone.call("POST", "/auth/link", { json: { key } });
      assert.equal(res.status, 400, JSON.stringify(key));
      assert.deepEqual(res.data, { error: "invalid_key" });
      assert.deepEqual(res.setCookies, []);
    }
    for (const json of [{}, { link: UNCLE.key }, [UNCLE.key]]) {
      const res = await anyone.call("POST", "/auth/link", { json });
      assert.equal(res.status, 400, JSON.stringify(json));
      assert.deepEqual(res.data, { error: "invalid_key" });
    }
    // A JSON POST with no body at all.
    const empty = await anyone.call("POST", "/auth/link", { type: "application/json" });
    assert.equal(empty.status, 400);
    assert.deepEqual(empty.data, { error: "invalid_key" });
    assert.equal((await anyone.call("GET", "/session")).status, 401);
  });

  test("a well-formed key that is nobody's is a 401 with no cookie", async (t) => {
    const portal = await startPortal(t);
    const anyone = portal.browser();
    for (const key of [person("x", "x").key, "a".repeat(20), "Z".repeat(100), UNCLE.keyHash]) {
      const res = await anyone.call("POST", "/auth/link", { json: { key } });
      assert.equal(res.status, 401, key);
      assert.deepEqual(res.data, { error: "invalid_link" });
      assert.deepEqual(res.setCookies, []);
    }

    // A device already signed in stays signed in as before.
    const zach = await portal.open(ZACH);
    const dead = await zach.call("POST", "/auth/link", { json: { key: person("x", "x").key } });
    assert.equal(dead.status, 401);
    assert.deepEqual(dead.setCookies, []);
    assert.deepEqual((await zach.call("GET", "/session")).data, { name: "Zach" });
  });

  test("re-issuing a link signs out every device that used the old one, and the old link stops working", async (t) => {
    const portal = await startPortal(t);
    const phone = await portal.open(UNCLE);
    const laptop = await portal.open(UNCLE);
    const zach = await portal.open(ZACH);

    // tools/portal/users.sh link uncle: same id and name, new key.
    const reissued = person("uncle", "Uncle Dave");
    await portal.setPeople([ZACH, reissued]);

    portal.clock.now += 29 * SECOND;
    assert.equal((await phone.call("GET", "/session")).status, 200, "users.json is still cached");
    portal.clock.now += 2 * SECOND;
    const out = await phone.call("GET", "/telemetry");
    assert.equal(out.status, 401);
    assert.deepEqual(out.data, { error: "unauthenticated" });
    assert.deepEqual(out.setCookies, [CLEARED]);
    assert.equal((await laptop.call("GET", "/session")).status, 401);

    const old = await portal.browser().call("POST", "/auth/link", { json: { key: UNCLE.key } });
    assert.equal(old.status, 401);
    assert.deepEqual(old.data, { error: "invalid_link" });

    await portal.open(reissued, phone);
    assert.deepEqual((await phone.call("GET", "/session")).data, { name: "Uncle Dave" });
    assert.deepEqual((await zach.call("GET", "/session")).data, { name: "Zach" }, "nobody else is affected");
  });

  test("removing someone signs out their devices within 30 seconds, and their link stops working", async (t) => {
    const portal = await startPortal(t);
    const phone = await portal.open(UNCLE);
    const zach = await portal.open(ZACH);

    await portal.setPeople([ZACH]);
    portal.clock.now += 29 * SECOND;
    assert.equal((await phone.call("GET", "/session")).status, 200, "still cached");
    portal.clock.now += 2 * SECOND;
    const res = await phone.call("GET", "/session");
    assert.equal(res.status, 401);
    assert.deepEqual(res.data, { error: "unauthenticated" });
    assert.deepEqual(res.setCookies, [CLEARED]);

    const link = await portal.browser().call("POST", "/auth/link", { json: { key: UNCLE.key } });
    assert.equal(link.status, 401);
    assert.deepEqual(link.data, { error: "invalid_link" });
    assert.equal((await zach.call("GET", "/session")).status, 200);
  });

  test("a device in use stays signed in: renewed once a day, always for 400 days", async (t) => {
    const portal = await startPortal(t);
    const phone = await portal.open(UNCLE);
    const original = phone.jar.get("tracker_session");

    portal.clock.now += 23 * HOUR;
    assert.deepEqual((await phone.call("GET", "/session")).setCookies, [], "not yet a day old");

    portal.clock.now += 2 * HOUR;
    const renewed = await phone.call("GET", "/telemetry?days=1");
    assert.equal(renewed.status, 200);
    assert.equal(renewed.setCookies.length, 1);
    assert.match(renewed.setCookies[0], new RegExp(`^tracker_session=[\\w-]+\\.[\\w-]+; ${ATTRIBUTES}; Max-Age=34560000$`));
    const token = phone.jar.get("tracker_session");
    assert.notEqual(token, original);
    assert.deepEqual(claimsOf(token), {
      u: "uncle",
      k: UNCLE.keyHash.slice(0, 16),
      iat: START + 25 * HOUR,
      exp: START + 25 * HOUR + 400 * DAY,
    });
    assert.deepEqual((await phone.call("GET", "/session")).setCookies, [], "renewed only once a day");

    // 400 days after the link was opened the first cookie is dead, but the renewed
    // one still has a day to run, and using it renews it again.
    portal.clock.now = START + 400 * DAY;
    const later = await phone.call("GET", "/session");
    assert.equal(later.status, 200);
    assert.equal(claimsOf(phone.jar.get("tracker_session")).exp, START + 800 * DAY);

    const idle = portal.browser();
    idle.jar.set("tracker_session", original);
    const expired = await idle.call("GET", "/session");
    assert.equal(expired.status, 401);
    assert.deepEqual(expired.setCookies, [CLEARED]);
  });

  test("a users.json left in the old phone-number format lets nobody in, without failing", async (t) => {
    const portal = await startPortal(t);
    const phone = await portal.open(UNCLE);

    await portal.portalStore.write(
      "users.json",
      JSON.stringify({
        users: [
          { phone: "+15195550100", name: "Zach", admin: true },
          { phone: "+15195550101", name: "Uncle Dave", admin: false },
        ],
      }),
    );
    portal.clock.now += 31 * SECOND;
    const session = await phone.call("GET", "/session");
    assert.equal(session.status, 401);
    assert.deepEqual(session.setCookies, [CLEARED]);
    const link = await portal.browser().call("POST", "/auth/link", { json: { key: ZACH.key } });
    assert.equal(link.status, 401);
    assert.deepEqual(link.data, { error: "invalid_link" });

    // users.sh add writes the new format over it, and that works at once.
    await portal.setPeople([ZACH]);
    portal.clock.now += 31 * SECOND;
    await portal.open(ZACH);
  });

  test("a missing users.json lets nobody in", async (t) => {
    const portal = await startPortal(t);
    await portal.portalStore.delete("users.json");
    const res = await portal.browser().call("POST", "/auth/link", { json: { key: ZACH.key } });
    assert.equal(res.status, 401);
    assert.deepEqual(res.data, { error: "invalid_link" });
  });

  test("a session cookie that is forged, garbled or from the old format is refused and cleared", async (t) => {
    const portal = await startPortal(t);
    const phone = await portal.open(UNCLE);
    const [payload, signature] = phone.jar.get("tracker_session").split(".");
    const claims = claimsOf(payload);
    const swap = (change) => `${Buffer.from(JSON.stringify({ ...claims, ...change })).toString("base64url")}.${signature}`;
    // A cookie from before links, signed with the same secret: deploying this
    // version signs out everyone who had one.
    const oldPayload = Buffer.from(
      JSON.stringify({ p: "+15195550100", r: true, iat: START, exp: START + 90 * DAY }),
    ).toString("base64url");
    const oldFormat = `${oldPayload}.${createHmac("sha256", SECRET).update(oldPayload).digest("base64url")}`;

    for (const value of [swap({ u: "zach" }), swap({ k: ZACH.keyHash.slice(0, 16) }), oldFormat, "garbage", ""]) {
      const forger = portal.browser();
      forger.jar.set("tracker_session", value);
      const res = await forger.call("GET", "/telemetry");
      assert.equal(res.status, 401, value);
      assert.deepEqual(res.setCookies, [CLEARED]);
    }
  });

  test("every POST must be JSON", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.open(ZACH);
    for (const path of ["/auth/link", "/auth/logout"]) {
      for (const [type, body] of [
        ["application/x-www-form-urlencoded", `key=${UNCLE.key}`],
        ["text/plain", JSON.stringify({ key: UNCLE.key })],
        ["multipart/form-data; boundary=x", "--x--"],
        [undefined, undefined],
      ]) {
        const res = await zach.call("POST", path, { body, type });
        assert.equal(res.status, 415, `${path} ${type}`);
        assert.deepEqual(res.data, { error: "unsupported_media_type" });
        assert.deepEqual(res.setCookies, []);
      }
    }
    assert.deepEqual((await zach.call("GET", "/session")).data, { name: "Zach" }, "still signed in, as Zach");

    // Broken JSON is a 400.
    const broken = await zach.call("POST", "/auth/link", { body: "{key:", type: "application/json" });
    assert.equal(broken.status, 400);
    assert.deepEqual(broken.data, { error: "bad_request" });
    // A JSON POST needs no body.
    assert.equal((await zach.call("POST", "/auth/logout", { type: "application/json" })).status, 204);
  });

  test("the phone-number sign-in and admin routes are gone", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.open(ZACH);
    const id = "a".repeat(64);
    for (const [method, path] of [
      ["POST", "/auth/request"],
      ["GET", "/auth/request"],
      ["POST", "/auth/request/cancel"],
      ["GET", "/admin/requests"],
      ["POST", `/admin/requests/${id}/approve`],
      ["POST", `/admin/requests/${id}/deny`],
    ]) {
      for (const who of [zach, portal.browser()]) {
        const res = await who.call(method, path, method === "POST" ? { json: { phone: "+15195550100" } } : {});
        assert.equal(res.status, 404, `${method} ${path}`);
        assert.deepEqual(res.data, { error: "not_found" });
        assert.deepEqual(res.setCookies, []);
      }
    }
  });

  test("days must be a whole number from 1 to 60, 7 by default", async (t) => {
    const portal = await startPortal(t);
    const zach = await portal.open(ZACH);
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
    const nope = await anyone.call("GET", "/nope");
    assert.equal(nope.status, 404);
    assert.deepEqual(nope.data, { error: "not_found" });
  });
});
