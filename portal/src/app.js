import express from "express";

import { clearCookie, readCookie } from "./cookies.js";
import { normalizePhone } from "./phone.js";
import {
  REQUEST_COOKIE,
  createRequests,
  isRequestId,
  newSecret,
  requestCookie,
  requestId,
} from "./requests.js";
import { SESSION_COOKIE, sessionCookie, shouldRenew, signSession, verifySession } from "./session.js";
import { createTelemetry } from "./telemetry.js";
import { createUsers } from "./users.js";

// The page is served by Netlify, which forwards /api/tracker/* here unchanged
// apart from the prefix, so to the browser this API is on the page's own origin.
// That is why there are no CORS headers: nothing cross-origin is meant to call it.

const DEFAULT_DAYS = 7;
const MAX_DAYS = 60;
// POST /auth/request takes at least this long whatever the number. Filing a
// request for an allowlisted number costs a bucket listing, reads and a write
// that an unknown number does not, and that gap would otherwise say who is on the
// list as plainly as a different status code would.
const REQUEST_FLOOR_MS = 500;

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Express 4 does not catch a rejected promise from a handler; this hands it to
// the error handler instead of leaving the request hanging.
const route = (handler) => (req, res, next) => Promise.resolve(handler(req, res, next)).catch(next);

function parseDays(raw) {
  if (raw === undefined) return DEFAULT_DAYS;
  if (typeof raw !== "string" || !/^\d{1,2}$/.test(raw)) return null;
  const days = Number(raw);
  return days >= 1 && days <= MAX_DAYS ? days : null;
}

export function createApp({ telemetryStore, portalStore, sessionSecret, siteTz, now = Date.now }) {
  const users = createUsers(portalStore, now);
  const requests = createRequests(portalStore, now);
  const telemetry = createTelemetry({ store: telemetryStore, tz: siteTz, now });

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
  // never approves one. On top of that both cookies are SameSite=Lax, so a browser
  // does not attach them to a cross-site POST at all, only to top-level GET
  // navigations. The one GET with an effect, collecting the answer to the
  // browser's own sign-in request, gives an outside page nothing it can read.
  app.use((req, res, next) => {
    if (req.method === "POST" && !req.is("application/json")) {
      return res.status(415).json({ error: "unsupported_media_type" });
    }
    next();
  });
  app.use(express.json({ limit: "10kb" }));

  // Signed in means: a session cookie with a good signature, not expired, for a
  // number that is still in users.json. The last check is what makes removing
  // someone from the list take effect at once instead of when their cookie runs out.
  const authenticate = route(async (req, res, next) => {
    const token = readCookie(req.headers.cookie, SESSION_COOKIE);
    const session = token ? verifySession(token, sessionSecret, now()) : null;
    const user = session ? await users.find(session.phone) : null;

    if (!user) {
      if (token !== undefined) res.append("Set-Cookie", clearCookie(SESSION_COOKIE));
      return res.status(401).json({ error: "unauthenticated" });
    }
    if (shouldRenew(session, now())) {
      const fresh = signSession({ phone: user.phone, remember: true, now: now() }, sessionSecret);
      res.append("Set-Cookie", sessionCookie(fresh, true));
    }
    req.user = user;
    next();
  });

  const adminOnly = (req, res, next) => {
    if (!req.user.admin) return res.status(403).json({ error: "forbidden" });
    next();
  };

  app.get("/healthz", (req, res) => {
    res.type("text/plain").send("ok");
  });

  // Ask to sign in. A well-formed number always gets 202 and a request cookie,
  // whether it is on the list, already has three requests waiting, or neither:
  // for those the secret is random and nothing is filed, and the browser cannot
  // tell (see GET /auth/request).
  app.post(
    "/auth/request",
    route(async (req, res) => {
      const phone = normalizePhone(req.body.phone);
      if (!phone) return res.status(400).json({ error: "invalid_phone" });

      const floor = delay(REQUEST_FLOOR_MS);
      // Asking again from the same browser replaces its earlier request rather
      // than adding a second line to the admin list.
      const previous = readCookie(req.headers.cookie, REQUEST_COOKIE);
      if (previous) await requests.remove(requestId(previous));

      const secret = newSecret();
      const user = await users.find(phone);
      if (user && (await requests.file(user, secret, req.body.remember === true))) {
        console.log(`Sign-in requested for ${user.name}`);
      }
      await floor;
      res.set("Set-Cookie", requestCookie(secret));
      res.status(202).json({ ok: true });
    }),
  );

  // The page polls this while it waits for an answer.
  app.get(
    "/auth/request",
    route(async (req, res) => {
      const secret = readCookie(req.headers.cookie, REQUEST_COOKIE);
      if (!secret) return res.status(404).json({ error: "no_request" });

      const id = requestId(secret);
      const request = await requests.find(id);
      // No request on file reads exactly like one still waiting. That is what an
      // unknown number (or one over the cap) gets for as long as its cookie lives,
      // so the answer never says whether a number is on the list; after 24 hours
      // the browser drops the cookie, real or not, and this becomes a 404.
      const at = now();
      if (request === null || (request.status === "pending" && requests.isLive(request, at))) {
        return res.json({ status: "pending" });
      }

      // Anything else ends the request, whatever the answer.
      await requests.remove(id);
      res.append("Set-Cookie", clearCookie(REQUEST_COOKIE));
      if (!requests.isLive(request, at)) return res.json({ status: "expired" });
      if (request.status === "denied") return res.json({ status: "denied" });

      // Approved, but only if they are still on the list now.
      const user = await users.find(request.phone);
      if (!user) return res.json({ status: "denied" });
      const token = signSession({ phone: user.phone, remember: request.remember, now: at }, sessionSecret);
      res.append("Set-Cookie", sessionCookie(token, request.remember));
      console.log(`${user.name} signed in${request.remember ? " (remembered)" : ""}`);
      res.json({ status: "approved", name: user.name, admin: user.admin });
    }),
  );

  app.post(
    "/auth/request/cancel",
    route(async (req, res) => {
      const secret = readCookie(req.headers.cookie, REQUEST_COOKIE);
      if (secret) await requests.remove(requestId(secret));
      res.set("Set-Cookie", clearCookie(REQUEST_COOKIE));
      res.status(204).end();
    }),
  );

  app.post("/auth/logout", (req, res) => {
    res.set("Set-Cookie", clearCookie(SESSION_COOKIE));
    res.status(204).end();
  });

  app.get("/session", authenticate, (req, res) => {
    res.json({ name: req.user.name, admin: req.user.admin });
  });

  app.get(
    "/admin/requests",
    authenticate,
    adminOnly,
    route(async (req, res) => {
      res.json({ requests: await requests.listPending() });
    }),
  );

  const decide = (status) =>
    route(async (req, res) => {
      if (!isRequestId(req.params.id)) return res.status(400).json({ error: "invalid_id" });
      const request = await requests.decide(req.params.id, status);
      if (!request) return res.status(404).json({ error: "not_found" });
      console.log(`${req.user.name} ${status} the sign-in request from ${request.name}`);
      res.json({ ok: true });
    });
  app.post("/admin/requests/:id/approve", authenticate, adminOnly, decide("approved"));
  app.post("/admin/requests/:id/deny", authenticate, adminOnly, decide("denied"));

  app.get(
    "/telemetry",
    authenticate,
    route(async (req, res) => {
      const days = parseDays(req.query.days);
      if (days === null) return res.status(400).json({ error: "invalid_days" });
      const body = await telemetry.read(days);
      // Private: the response is for a signed-in user only. 30 s, because a new
      // upload arrives every 15 minutes or so and the page may poll.
      res.set("Cache-Control", "private, max-age=30").json(body);
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
