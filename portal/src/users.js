import { createHash } from "node:crypto";

// users.json lists who may sign in, each with the SHA-256 of their personal link's key:
//   {"users":[{"id":"uncle","name":"Uncle Dave","keyHash":"<64 hex>","issuedAt":"<ISO>"}]}
// Only the hash is kept, so the file (or anyone who can read the bucket) cannot be
// turned back into a working link. It is written by tools/portal/users.sh, so the
// portal re-reads it every 30 seconds rather than at start-up. That is also the
// longest a removed person, or a link that has been re-issued, keeps working.
const CACHE_MS = 30_000;
const OBJECT = "users.json";

const ID = /^[a-z0-9-]{1,32}$/;
const HASH = /^[0-9a-f]{64}$/;
// users.sh makes a key from 32 random bytes, which is 43 base64url characters. The
// range is looser than that only so the check is about shape, not about one
// generator; anything outside it is not a key from any link.
const KEY = /^[A-Za-z0-9_-]{20,100}$/;

export const isKey = (raw) => typeof raw === "string" && KEY.test(raw);
export const hashKey = (key) => createHash("sha256").update(key).digest("hex");

// The usable entries of users.json (null when there is no file). Anything unusable is
// skipped rather than failing the load: a file left over from an older format, or a
// bad hand edit, then lets nobody in through that entry instead of breaking the
// service for everyone listed correctly.
function parseUsers(text) {
  const byId = new Map();
  const byKeyHash = new Map();
  const entries = text === null ? [] : JSON.parse(text)?.users;
  for (const entry of Array.isArray(entries) ? entries : []) {
    const { id, name, keyHash } = entry ?? {};
    if (typeof id !== "string" || !ID.test(id)) continue;
    if (typeof keyHash !== "string" || !HASH.test(keyHash)) continue;
    if (typeof name !== "string" || !name.trim()) continue;
    // users.sh never writes an id twice; if a hand edit does, the first one counts,
    // so a link and the cookie it leads to always resolve to the same entry.
    if (byId.has(id)) continue;
    const user = { id, name, keyHash };
    byId.set(id, user);
    byKeyHash.set(keyHash, user);
  }
  return { byId, byKeyHash };
}

export function createUsers(store, now) {
  let cache = null;

  function current() {
    if (!cache || now() - cache.at >= CACHE_MS) {
      // The promise is cached, not the result, so a burst of requests after the
      // cache expires shares one read.
      const entry = { at: now(), users: store.read(OBJECT).then(parseUsers) };
      cache = entry;
      // A failed read (or a file that is not JSON at all) is not kept: the request
      // that hit it fails, and the next one tries again.
      entry.users.catch(() => {
        if (cache === entry) cache = null;
      });
    }
    return cache.users;
  }

  return {
    // The person a session cookie names, or null if they are no longer listed.
    async find(id) {
      return (await current()).byId.get(id) ?? null;
    },

    // The person whose link carries this key, or null. Looked up by the key's hash,
    // so no comparison ever touches the key itself: how long a Map lookup takes can
    // only say something about hashes, and nobody can pick a key that lands on a
    // chosen hash.
    async findByKey(key) {
      return (await current()).byKeyHash.get(hashKey(key)) ?? null;
    },
  };
}
