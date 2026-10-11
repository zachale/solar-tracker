import test from "node:test";
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";

import { clearCookie, readCookie, setCookie } from "../src/cookies.js";
import { REMEMBER_MS, SESSION_MS, sessionCookie, shouldRenew, signSession, verifySession } from "../src/session.js";

const SECRET = "test-secret";
const NOW = Date.parse("2026-10-11T16:00:00.000Z");
const HOUR = 60 * 60 * 1000;
const PHONE = "+15195550100";

test("a token signed here verifies, carrying its phone and expiry", () => {
  const remembered = verifySession(signSession({ phone: PHONE, remember: true, now: NOW }, SECRET), SECRET, NOW);
  assert.deepEqual(remembered, { phone: PHONE, remember: true, iat: NOW, exp: NOW + REMEMBER_MS });

  const brief = verifySession(signSession({ phone: PHONE, remember: false, now: NOW }, SECRET), SECRET, NOW);
  assert.deepEqual(brief, { phone: PHONE, remember: false, iat: NOW, exp: NOW + SESSION_MS });
  assert.equal(REMEMBER_MS, 90 * 24 * HOUR);
  assert.equal(SESSION_MS, 12 * HOUR);
});

test("a token signed with another secret is refused", () => {
  const token = signSession({ phone: PHONE, remember: true, now: NOW }, "other-secret");
  assert.equal(verifySession(token, SECRET, NOW), null);
});

test("changing the payload breaks the signature", () => {
  const token = signSession({ phone: PHONE, remember: false, now: NOW }, SECRET);
  const [payload, signature] = token.split(".");
  const claims = JSON.parse(Buffer.from(payload, "base64url").toString());

  for (const change of [{ p: "+15195550101" }, { r: true }, { exp: claims.exp + 365 * 24 * HOUR }]) {
    const forged = Buffer.from(JSON.stringify({ ...claims, ...change })).toString("base64url");
    assert.equal(verifySession(`${forged}.${signature}`, SECRET, NOW), null, JSON.stringify(change));
  }
});

test("changing the signature is refused, including variants that decode the same", () => {
  const token = signSession({ phone: PHONE, remember: true, now: NOW }, SECRET);
  const [payload, signature] = token.split(".");
  const last = signature.at(-1);
  const flipped = last === "A" ? "B" : "A";
  assert.equal(verifySession(`${payload}.${signature.slice(0, -1)}${flipped}`, SECRET, NOW), null);
  assert.equal(verifySession(`${payload}.${signature}A`, SECRET, NOW), null);
  assert.equal(verifySession(`${payload}.${signature}=`, SECRET, NOW), null);
  assert.equal(verifySession(`${payload}.`, SECRET, NOW), null);
});

test("malformed tokens are refused", () => {
  const token = signSession({ phone: PHONE, remember: true, now: NOW }, SECRET);
  for (const bad of ["", "abc", ".", `${token}.extra`, token.replace(".", ""), undefined, null, 42]) {
    assert.equal(verifySession(bad, SECRET, NOW), null, String(bad));
  }
});

test("a well-signed payload that is not a session is refused", () => {
  // Signed by hand with the real secret, so only the claim checks can stop it.
  const sign = (claims) => {
    const payload = Buffer.from(JSON.stringify(claims)).toString("base64url");
    return `${payload}.${createHmac("sha256", SECRET).update(payload).digest("base64url")}`;
  };
  assert.equal(verifySession(sign({ p: PHONE, r: "yes", iat: NOW, exp: NOW + HOUR }), SECRET, NOW), null);
  assert.equal(verifySession(sign({ p: 15195550100, r: true, iat: NOW, exp: NOW + HOUR }), SECRET, NOW), null);
  assert.equal(verifySession(sign({ p: PHONE, r: true, iat: NOW }), SECRET, NOW), null);
  assert.equal(verifySession(sign(null), SECRET, NOW), null);
  assert.ok(verifySession(sign({ p: PHONE, r: true, iat: NOW, exp: NOW + HOUR }), SECRET, NOW));
});

test("a token is good until the moment it expires", () => {
  const token = signSession({ phone: PHONE, remember: false, now: NOW }, SECRET);
  assert.ok(verifySession(token, SECRET, NOW + SESSION_MS - 1));
  assert.equal(verifySession(token, SECRET, NOW + SESSION_MS), null);

  const remembered = signSession({ phone: PHONE, remember: true, now: NOW }, SECRET);
  assert.ok(verifySession(remembered, SECRET, NOW + REMEMBER_MS - 1));
  assert.equal(verifySession(remembered, SECRET, NOW + REMEMBER_MS), null);
});

test("only a remembered session more than a day old is renewed", () => {
  const remembered = { phone: PHONE, remember: true, iat: NOW, exp: NOW + REMEMBER_MS };
  assert.equal(shouldRenew(remembered, NOW + 23 * HOUR), false);
  assert.equal(shouldRenew(remembered, NOW + 24 * HOUR), false);
  assert.equal(shouldRenew(remembered, NOW + 24 * HOUR + 1), true);

  const brief = { phone: PHONE, remember: false, iat: NOW, exp: NOW + SESSION_MS };
  assert.equal(shouldRenew(brief, NOW + 11 * HOUR), false);
});

test("cookie attributes: Max-Age only when remembered", () => {
  assert.equal(
    sessionCookie("tok.en", true),
    "tracker_session=tok.en; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=7776000",
  );
  assert.equal(sessionCookie("tok.en", false), "tracker_session=tok.en; Path=/; HttpOnly; Secure; SameSite=Lax");
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
