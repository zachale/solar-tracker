import express from "express";

import { createCalibrations } from "./calibrations.js";
import { clearCookie, readCookie } from "./cookies.js";
import { SESSION_COOKIE, sessionCookie, shouldRenew, signSession, verifySession } from "./session.js";
import { createTelemetry } from "./telemetry.js";
import { createUsers, isKey } from "./users.js";

// The page is served by Netlify, which forwards /api/tracker/* here unchanged
// apart from the prefix, so to the browser this API is on the page's own origin.
// That is why there are no CORS headers: nothing cross-origin is meant to call it.

const DAY = 24 * 60 * 60 * 1000;
const DEFAULT_DAYS = 7;
const MAX_DAYS = 60;

// Express 4 does not catch a rejected promise from a handler; this hands it to
// the error handler instead of leaving the request hanging.
const route = (handler) => (req, res, next) => Promise.resolve(handler(req, res, next)).catch(next);

function parseDays(raw) {
  if (raw === undefined) return DEFAULT_DAYS;
  if (typeof raw !== "string" || !/^\d{1,2}$/.test(raw)) return null;
  const days = Number(raw);
  return days >= 1 && days <= MAX_DAYS ? days : null;
}

export function createApp({ telemetryStore, portalStore, syncLog, sessionSecret, siteTz, now = Date.now }) {
  const users = createUsers(portalStore, now);
  const telemetry = createTelemetry({ store: telemetryStore, tz: siteTz, now });
  const calibrations = createCalibrations({ log: syncLog, now });

  const app = express();
  app.disable("x-powered-by");

  // Everything here is per user, so nothing may be kept by Netlify's CDN or any
  // other shared cache. /telemetry relaxes this to a short private cache.
  app.use((req, res, next) => {
    res.set("Cache-Control", "no-store");
    next();
  });

  // CSRF. Every state-changing route is a POST and every POST must say it is JSON.
  // A page on another site can make a browser send a cross-site POST only as a
  // form or a "simple" fetch, which are limited to form or text/plain bodies;
  // anything declaring application/json needs a CORS preflight, and this API
  // never approves one. On top of that the session cookie is SameSite=Lax, so a
  // browser does not attach it to a cross-site POST at all, only to top-level GET
  // navigations, and no GET here does anything but read (or renew the caller's own
  // cookie).
  app.use((req, res, next) => {
    if (req.method === "POST" && !req.is("application/json")) {
      return res.status(415).json({ error: "unsupported_media_type" });
    }
    next();
  });
  app.use(express.json({ limit: "10kb" }));

  function setSession(res, user) {
    const token = signSession({ id: user.id, keyHash: user.keyHash, now: now() }, sessionSecret);
    res.append("Set-Cookie", sessionCookie(token));
  }

  // Signed in means: a session cookie with a good signature, not expired, for
  // someone still in users.json whose link has not been re-issued since. Checking
  // the list on every request is what makes removing someone, or re-issuing their
  // link, take effect within the 30 s users cache instead of when cookies run out.
  const authenticate = route(async (req, res, next) => {
    const token = readCookie(req.headers.cookie, SESSION_COOKIE);
    const session = token ? verifySession(token, sessionSecret, now()) : null;
    const user = session ? await users.find(session.id) : null;

    if (!user || !user.keyHash.startsWith(session.keyTag)) {
      if (token !== undefined) res.append("Set-Cookie", clearCookie(SESSION_COOKIE));
      return res.status(401).json({ error: "unauthenticated" });
    }
    // Sliding: a device in use never reaches the 400-day expiry.
    if (shouldRenew(session, now())) setSession(res, user);
    req.user = user;
    next();
  });

  app.get("/health", (req, res) => {
    res.type("text/plain").send("ok");
  });

  // Opening a personal link. The key is in the link's fragment (#key=...), which a
  // browser never sends anywhere, so it stays out of Netlify's and Cloud Run's
  // request logs and Referer headers; the page reads it and sends it here once, in
  // a POST body, which neither logs. A key is 256 random bits, so there is nothing
  // to gain by guessing and no need to slow guesses down.
  app.post(
    "/auth/link",
    route(async (req, res) => {
      const key = req.body?.key;
      if (!isKey(key)) return res.status(400).json({ error: "invalid_key" });

      const user = await users.findByKey(key);
      if (!user) {
        // Most likely a link that has since been re-issued, or one for someone removed.
        console.log("A sign-in link that is no longer valid was opened");
        return res.status(401).json({ error: "invalid_link" });
      }
      setSession(res, user);
      console.log(`${user.name} signed in a device with their link`);
      res.json({ name: user.name });
    }),
  );

  app.post("/auth/logout", (req, res) => {
    res.set("Set-Cookie", clearCookie(SESSION_COOKIE));
    res.status(204).end();
  });

  app.get("/session", authenticate, (req, res) => {
    res.json({ name: req.user.name });
  });

  app.get(
    "/telemetry",
    authenticate,
    route(async (req, res) => {
      const days = parseDays(req.query.days);
      if (days === null) return res.status(400).json({ error: "invalid_days" });
      // One instant for both, so the calibrations cover exactly the rows' window.
      const at = now();
      const [body, events] = await Promise.all([
        telemetry.read(days, at),
        // The calibrations only mark up the chart. If the log cannot be read the
        // page still gets its readings, just without the marks.
        calibrations.read(at - days * DAY, at).catch((error) => {
          console.error("Reading the calibrations from the request log failed:", error);
          return [];
        }),
      ]);
      // Private: the response is for a signed-in user only. 30 s, because a new
      // upload arrives every 15 minutes or so and the page may poll.
      res.set("Cache-Control", "private, max-age=30").json({ ...body, calibrations: events });
    }),
  );

  app.use((req, res) => {
    res.status(404).json({ error: "not_found" });
  });

  // Express recognizes an error handler by its four parameters, so next stays.
  app.use((error, req, res, next) => {
    if (res.headersSent) return next(error);
    // express.json() reports a malformed or oversized body as a 4xx error.
    if (error.status >= 400 && error.status < 500) {
      return res.status(error.status).json({ error: "bad_request" });
    }
    console.error(`${req.method} ${req.path} failed:`, error);
    res.status(500).json({ error: "internal" });
  });

  return app;
}
