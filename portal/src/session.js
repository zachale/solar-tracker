import { createHmac, timingSafeEqual } from "node:crypto";

import { setCookie } from "./cookies.js";

// A signed cookie rather than a session table: the portal has a handful of users
// and no database, and a token that carries its own user id and expiry needs
// nothing stored to check it. Revocation still works because every request also
// looks the person up in users.json (see app.js):
//   - removed from the list: their id is gone, so every cookie naming it fails;
//   - link re-issued: the token carries the start of the key hash it was signed in
//     with ("k"), which no longer matches the new hash, so every device that used
//     the old link is signed out without the server keeping a list of devices.
//
// token = base64url(JSON {u: user id, k: first 16 hex of the key hash, iat, exp})
//         + "." + base64url(HMAC-SHA256), with times in epoch milliseconds.

export const SESSION_COOKIE = "tracker_session";

const DAY = 24 * 60 * 60 * 1000;
// Opening a link should sign a device in for good. Browsers cap a cookie's lifetime
// at 400 days whatever Max-Age asks for, so that is the longest it can be, and the
// token's own expiry matches it.
export const SESSION_MS = 400 * DAY;
// The cookie is re-issued once it is a day old, so a device that opens the page at
// least once every 400 days stays signed in indefinitely, without a Set-Cookie on
// every request.
export const RENEW_AFTER_MS = DAY;
// 64 bits of the key hash are plenty to tell one issue of a person's link from the
// next; the cookie has no need to carry the whole hash.
const KEY_TAG_LENGTH = 16;
const KEY_TAG = /^[0-9a-f]{16}$/;

function mac(payload, secret) {
  return createHmac("sha256", secret).update(payload).digest("base64url");
}

export function signSession({ id, keyHash, now }, secret) {
  const claims = { u: id, k: keyHash.slice(0, KEY_TAG_LENGTH), iat: now, exp: now + SESSION_MS };
  const payload = Buffer.from(JSON.stringify(claims)).toString("base64url");
  return `${payload}.${mac(payload, secret)}`;
}

// Returns {id, keyTag, iat, exp} for a genuine, unexpired token, else null.
export function verifySession(token, secret, now) {
  if (typeof token !== "string") return null;
  const parts = token.split(".");
  if (parts.length !== 2) return null;
  const [payload, signature] = parts;

  // Compared as text, not decoded bytes: base64url decoding ignores stray
  // characters, so two different strings could decode to the same signature.
  const expected = Buffer.from(mac(payload, secret));
  const given = Buffer.from(signature);
  if (given.length !== expected.length || !timingSafeEqual(given, expected)) return null;

  let claims;
  try {
    claims = JSON.parse(Buffer.from(payload, "base64url").toString("utf8"));
  } catch {
    return null;
  }
  const { u, k, iat, exp } = claims ?? {};
  // A short or empty k would match the start of any hash, so its shape is checked
  // even though only this service can sign one.
  if (typeof u !== "string" || typeof k !== "string" || !KEY_TAG.test(k)) return null;
  if (!Number.isFinite(iat) || !Number.isFinite(exp)) return null;
  if (now >= exp) return null;
  return { id: u, keyTag: k, iat, exp };
}

export function shouldRenew(session, now) {
  return now - session.iat > RENEW_AFTER_MS;
}

export function sessionCookie(token) {
  return setCookie(SESSION_COOKIE, token, SESSION_MS / 1000);
}
