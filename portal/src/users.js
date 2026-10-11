import { normalizePhone } from "./phone.js";

// users.json is the allowlist and the only record of who is an admin:
//   {"users":[{"phone":"+15195551234","name":"Zach","admin":true}]}
// It is edited by hand with tools/portal/users.sh, so the portal re-reads it every
// 30 seconds rather than at start-up. That is also the longest a removed user can
// keep using a cookie they already hold.
const CACHE_MS = 30_000;
const OBJECT = "users.json";

export function createUsers(store, now) {
  let cache = null;

  async function load() {
    const text = await store.read(OBJECT);
    const byPhone = new Map();
    if (text === null) return byPhone;
    for (const entry of JSON.parse(text).users ?? []) {
      // Normalized again on the way in, so a number typed straight into the file
      // as "519-555-1234" still matches.
      const phone = normalizePhone(entry.phone);
      if (phone) byPhone.set(phone, { phone, name: String(entry.name ?? ""), admin: entry.admin === true });
    }
    return byPhone;
  }

  return {
    // The user record for an E.164 number, or null if it is not on the list.
    async find(phone) {
      if (!cache || now() - cache.at >= CACHE_MS) {
        // The promise is cached, not the result, so a burst of requests after the
        // cache expires shares one read.
        const entry = { at: now(), users: load() };
        cache = entry;
        // A failed read (or a file that is not JSON) is not kept: the request that
        // hit it fails, and the next one tries again.
        entry.users.catch(() => {
          if (cache === entry) cache = null;
        });
      }
      return (await cache.users).get(phone) ?? null;
    },
  };
}
