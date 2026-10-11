import { createHash, randomBytes } from "node:crypto";

import { setCookie } from "./cookies.js";

// Sign-in requests. There are no codes and nothing is texted: someone on the
// allowlist asks to sign in, Zach approves or denies the request in the admin view
// (or with tools/portal/requests.sh), and the browser that asked is signed in the
// next time it checks.
//
// An approval is bound to the browser that asked by a secret only it holds: 32
// random bytes in an HttpOnly cookie. The request is filed under the secret's
// SHA-256, which is also the id the admin sees, so the id (on the admin page or in
// the bucket) is no help to anyone else trying to collect the approval.
//
//   requests/<id>.json = {id, phone, name, remember, requestedAt, expiresAt,
//                         status: "pending" | "approved" | "denied", decidedAt?}

export const REQUEST_COOKIE = "tracker_request";
export const REQUEST_TTL_MS = 24 * 60 * 60 * 1000;
// Waiting requests per number, so someone who knows an allowlisted number cannot
// bury the admin list. Decided ones do not count.
const MAX_WAITING = 3;
const PREFIX = "requests/";

export const newSecret = () => randomBytes(32).toString("base64url");
export const requestId = (secret) => createHash("sha256").update(secret).digest("hex");
export const isRequestId = (id) => /^[0-9a-f]{64}$/.test(id);
const objectName = (id) => `${PREFIX}${id}.json`;

// The browser keeps the secret exactly as long as the request lives.
export function requestCookie(secret) {
  return setCookie(REQUEST_COOKIE, secret, REQUEST_TTL_MS / 1000);
}

export function createRequests(store, now) {
  // Requests for one number are filed one at a time, or a burst sent at once
  // would all count the same waiting requests and all get past the cap. Cloud Run
  // runs at most two instances, so a race between them can at worst double it.
  const queues = new Map();
  function serialized(key, task) {
    const run = (queues.get(key) ?? Promise.resolve()).then(task);
    const settled = run.then(
      () => {},
      () => {},
    );
    queues.set(key, settled);
    settled.then(() => {
      if (queues.get(key) === settled) queues.delete(key);
    });
    return run;
  }

  const parse = (text) => (text === null ? null : JSON.parse(text));
  const read = async (id) => parse(await store.read(objectName(id)));
  const write = (request) => store.write(objectName(request.id), `${JSON.stringify(request)}\n`);
  const remove = (id) => store.delete(objectName(id));
  const isLive = (request, at) => at < Date.parse(request.expiresAt);

  // Every request still inside its 24 hours. Expired ones met on the way are
  // deleted, so the prefix does not collect requests nobody came back for.
  async function liveRequests(at) {
    const names = await store.list(PREFIX);
    const all = (await Promise.all(names.map(async (name) => parse(await store.read(name))))).filter(Boolean);
    await Promise.all(all.filter((request) => !isLive(request, at)).map((request) => remove(request.id)));
    return all.filter((request) => isLive(request, at));
  }

  return {
    isLive,
    find: read,
    remove,

    // Files a request for an allowlisted user under the browser's secret, unless
    // three of theirs are already waiting. The caller answers the same either way.
    file(user, secret, remember) {
      return serialized(user.phone, async () => {
        const at = now();
        const waiting = (await liveRequests(at)).filter(
          (request) => request.phone === user.phone && request.status === "pending",
        );
        if (waiting.length >= MAX_WAITING) return false;

        await write({
          id: requestId(secret),
          phone: user.phone,
          name: user.name,
          remember,
          requestedAt: new Date(at).toISOString(),
          expiresAt: new Date(at + REQUEST_TTL_MS).toISOString(),
          status: "pending",
        });
        return true;
      });
    },

    // Records Zach's answer. Null if there is no such request or it has expired;
    // an answer can be changed until the browser collects it.
    async decide(id, status) {
      const at = now();
      const request = await read(id);
      if (request === null) return null;
      if (!isLive(request, at)) {
        await remove(id);
        return null;
      }
      const decided = { ...request, status, decidedAt: new Date(at).toISOString() };
      await write(decided);
      return decided;
    },

    // What the admin view shows: requests still waiting for an answer, newest first.
    async listPending() {
      const at = now();
      return (await liveRequests(at))
        .filter((request) => request.status === "pending")
        .sort((a, b) => Date.parse(b.requestedAt) - Date.parse(a.requestedAt))
        .map(({ id, name, phone, requestedAt, expiresAt }) => ({ id, name, phone, requestedAt, expiresAt }));
    },
  };
}
