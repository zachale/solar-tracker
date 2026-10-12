import test from "node:test";
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";

import { clearCookie, readCookie, setCookie } from "../src/cookies.js";
import { RENEW_AFTER_MS, SESSION_MS, sessionCookie, shouldRenew, signSession, verifySession } from "../src/session.js";

const SECRET = "test-secret";
const NOW = Date.parse("2026-10-11T16:00:00.000Z");
const HOUR = 60 * 60 * 1000;
const DAY = 24 * HOUR;
const ID = "uncle";
const KEY_HASH = "0123456789abcdef".repeat(4);
const sign = (now = NOW) => signSession({ id: ID, keyHash: KEY_HASH, now }, SECRET);

// Signed by hand with the real secret, so only the claim checks can stop it.
const signClaims = (claims) => {
  const payload = Buffer.from(JSON.stringify(claims)).toString("base64url");
  return `${payload}.${createHmac("sha256", SECRET).update(payload).digest("base64url")}`;
};

test("a token signed here verifies, carrying the id, the start of the key hash and a 400-day expiry", () => {
  assert.deepEqual(verifySession(sign(), SECRET, NOW), {
    id: ID,
    keyTag: "0123456789abcdef",
    iat: NOW,
    exp: NOW + 400 * DAY,
  });
  assert.equal(SESSION_MS, 400 * DAY);
  assert.equal(RENEW_AFTER_MS, DAY);

  // Only 16 hex of the hash go into the cookie.
  const claims = JSON.parse(Buffer.from(sign().split(".")[0], "base64url").toString());
  assert.deepEqual(claims, { u: ID, k: "0123456789abcdef", iat: NOW, exp: NOW + 400 * DAY });
});

test("a token signed with another secret is refused", () => {
  const token = signSession({ id: ID, keyHash: KEY_HASH, now: NOW }, "other-secret");
  assert.equal(verifySession(token, SECRET, NOW), null);
});

test("changing the payload breaks the signature", () => {
  const [payload, signature] = sign().split(".");
  const claims = JSON.parse(Buffer.from(payload, "base64url").toString());

  for (const change of [{ u: "zach" }, { k: "fedcba9876543210" }, { exp: claims.exp + 365 * DAY }]) {
    const forged = Buffer.from(JSON.stringify({ ...claims, ...change })).toString("base64url");
    assert.equal(verifySession(`${forged}.${signature}`, SECRET, NOW), null, JSON.stringify(change));
  }
});

test("changing the signature is refused, including variants that decode the same", () => {
  const [payload, signature] = sign().split(".");
  const last = signature.at(-1);
  const flipped = last === "A" ? "B" : "A";
  assert.equal(verifySession(`${payload}.${signature.slice(0, -1)}${flipped}`, SECRET, NOW), null);
  assert.equal(verifySession(`${payload}.${signature}A`, SECRET, NOW), null);
  assert.equal(verifySession(`${payload}.${signature}=`, SECRET, NOW), null);
  assert.equal(verifySession(`${payload}.`, SECRET, NOW), null);
});

test("malformed tokens are refused", () => {
  const token = sign();
  for (const bad of ["", "abc", ".", `${token}.extra`, token.replace(".", ""), undefined, null, 42]) {
    assert.equal(verifySession(bad, SECRET, NOW), null, String(bad));
  }
});

test("a well-signed payload that is not a session is refused", () => {
  const good = { u: ID, k: "0123456789abcdef", iat: NOW, exp: NOW + HOUR };
  assert.ok(verifySession(signClaims(good), SECRET, NOW));

  for (const bad of [
    { ...good, u: 42 },
    { ...good, u: undefined },
    // A short or empty tag would match the start of any key hash.
    { ...good, k: "" },
    { ...good, k: "0123" },
    { ...good, k: "0123456789ABCDEF" },
    { ...good, k: "0123456789abcdef0" },
    { ...good, k: undefined },
    { ...good, iat: undefined },
    { ...good, exp: "never" },
    // The previous format's claims.
    { p: "+15195550100", r: true, iat: NOW, exp: NOW + HOUR },
    null,
  ]) {
    assert.equal(verifySession(signClaims(bad), SECRET, NOW), null, JSON.stringify(bad));
  }
});

test("a token is good until the moment it expires", () => {
  const token = sign();
  assert.ok(verifySession(token, SECRET, NOW + SESSION_MS - 1));
  assert.equal(verifySession(token, SECRET, NOW + SESSION_MS), null);
});

test("a session is renewed once it is more than a day old", () => {
  const session = verifySession(sign(), SECRET, NOW);
  assert.equal(shouldRenew(session, NOW + 23 * HOUR), false);
  assert.equal(shouldRenew(session, NOW + 24 * HOUR), false);
  assert.equal(shouldRenew(session, NOW + 24 * HOUR + 1), true);
});

test("cookie attributes: always persistent, for 400 days", () => {
  assert.equal(
    sessionCookie("tok.en"),
    "tracker_session=tok.en; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=34560000",
  );
  assert.equal(setCookie("a", "b", 60), "a=b; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=60");
  assert.equal(clearCookie("tracker_session"), "tracker_session=; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=0");
});

test("the session cookie is picked out of a Cookie header", () => {
  assert.equal(readCookie("tracker_session=abc.def", "tracker_session"), "abc.def");
  assert.equal(readCookie("a=1; tracker_session=abc.def; b=2", "tracker_session"), "abc.def");
  assert.equal(readCookie("a=1;tracker_session=abc.def", "tracker_session"), "abc.def");
  assert.equal(readCookie("x_tracker_session=abc; tracker_session_old=def", "tracker_session"), undefined);
  assert.equal(readCookie("tracker_session=", "tracker_session"), "");
  assert.equal(readCookie("garbage", "tracker_session"), undefined);
  assert.equal(readCookie(undefined, "tracker_session"), undefined);
});
