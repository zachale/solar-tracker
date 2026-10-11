import { createHmac, timingSafeEqual } from "node:crypto";

import { setCookie } from "./cookies.js";

// A signed cookie rather than a session table: the portal has a handful of users
// and no database, and a token that carries its own phone number and expiry needs
// nothing stored to check it. Revocation still works because every request also
// looks the phone number up in users.json (see app.js), so removing someone there
// shuts them out without a server-side session list.
//
// token = base64url(JSON {p: phone, r: remember, iat, exp}) + "." + base64url(HMAC-SHA256)
// with times in epoch milliseconds.

export const SESSION_COOKIE = "tracker_session";

const HOUR = 60 * 60 * 1000;
const DAY = 24 * HOUR;
export const REMEMBER_MS = 90 * DAY;
export const SESSION_MS = 12 * HOUR;
// A remembered cookie is re-issued once it is a day old, so someone who opens the
// page every few weeks never meets the 90-day limit, without a Set-Cookie on
// every request.
export const RENEW_AFTER_MS = DAY;

function mac(payload, secret) {
  return createHmac("sha256", secret).update(payload).digest("base64url");
}

export function signSession({ phone, remember, now }, secret) {
  const claims = { p: phone, r: remember, iat: now, exp: now + (remember ? REMEMBER_MS : SESSION_MS) };
  const payload = Buffer.from(JSON.stringify(claims)).toString("base64url");
  return `${payload}.${mac(payload, secret)}`;
}

// Returns {phone, remember, iat, exp} for a genuine, unexpired token, else null.
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
  const { p, r, iat, exp } = claims ?? {};
  if (typeof p !== "string" || typeof r !== "boolean") return null;
  if (!Number.isFinite(iat) || !Number.isFinite(exp)) return null;
  if (now >= exp) return null;
  return { phone: p, remember: r, iat, exp };
}

export function shouldRenew(session, now) {
  return session.remember && now - session.iat > RENEW_AFTER_MS;
}

// Without "remember me" there is no Max-Age, which makes it a browser-session
// cookie that goes when the browser closes, on top of the 12 h expiry in the token.
export function sessionCookie(token, remember) {
  return setCookie(SESSION_COOKIE, token, remember ? REMEMBER_MS / 1000 : undefined);
}
