import test from "node:test";
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";

import { createMemoryStore } from "../src/stores.js";
import { createUsers, hashKey, isKey } from "../src/users.js";

const SECOND = 1000;
const NOW = Date.parse("2026-10-11T16:00:00.000Z");

// What tools/portal/users.sh makes: 32 random bytes, base64url, 43 characters.
const newKey = () => randomBytes(32).toString("base64url");

function person(id, name) {
  const key = newKey();
  return { key, entry: { id, name, keyHash: hashKey(key), issuedAt: new Date(NOW).toISOString() } };
}

function usersFrom(text, clock = { now: NOW }) {
  const store = createMemoryStore(text === undefined ? {} : { "users.json": text });
  return { store, clock, users: createUsers(store, () => clock.now) };
}

test("a key is a base64url string of 20 to 100 characters", () => {
  assert.equal(newKey().length, 43);
  for (const good of [newKey(), "a".repeat(20), "Z".repeat(100), "abc-DEF_0123456789xyz"]) {
    assert.equal(isKey(good), true, good);
  }
  for (const bad of [
    "a".repeat(19),
    "a".repeat(101),
    "",
    `${newKey()}=`, // padding is not base64url
    newKey().replace(/^./, "+"),
    newKey().replace(/^./, "/"),
    ` ${newKey()}`,
    `${newKey().slice(0, 30)} ${newKey().slice(0, 10)}`,
    "é".repeat(30),
    undefined,
    null,
    42,
    ["a".repeat(43)],
    { key: "a".repeat(43) },
  ]) {
    assert.equal(isKey(bad), false, JSON.stringify(bad));
  }
});

test("a key's hash is its SHA-256 in lowercase hex, as users.sh writes it", () => {
  assert.equal(hashKey("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
});

test("people are found by id and by key", async () => {
  const uncle = person("uncle", "Uncle Dave");
  const zach = person("zach", "Zach");
  const { users } = usersFrom(JSON.stringify({ users: [uncle.entry, zach.entry] }));

  const expected = { id: "uncle", name: "Uncle Dave", keyHash: uncle.entry.keyHash };
  assert.deepEqual(await users.find("uncle"), expected);
  assert.deepEqual(await users.findByKey(uncle.key), expected);
  assert.equal((await users.findByKey(zach.key)).id, "zach");
  assert.equal(await users.find("nobody"), null);
  assert.equal(await users.findByKey(newKey()), null);
  // The hash is not the key.
  assert.equal(await users.findByKey(uncle.entry.keyHash), null);
});

test("an entry that is not in the current format is ignored, and the rest still work", async () => {
  const good = person("uncle", "Uncle Dave");
  const { key: otherKey, entry: other } = person("other", "Other");
  const bad = [
    // The previous, phone-number format.
    { phone: "+15195550100", name: "Zach", admin: true },
    { ...other, id: "Zach" }, // upper case
    { ...other, id: "" },
    { ...other, id: "a".repeat(33) },
    { ...other, id: "with space" },
    { ...other, id: 7 },
    { ...other, keyHash: other.keyHash.toUpperCase() },
    { ...other, keyHash: other.keyHash.slice(1) },
    { ...other, keyHash: undefined },
    { ...other, key: otherKey, keyHash: undefined },
    { ...other, name: "" },
    { ...other, name: "   " },
    { ...other, name: undefined },
    null,
    "uncle",
    42,
  ];
  const { users } = usersFrom(JSON.stringify({ users: [...bad, good.entry] }));
  assert.equal((await users.findByKey(good.key)).id, "uncle");
  assert.equal(await users.findByKey(otherKey), null);
  assert.equal(await users.find("other"), null);
});

test("a whole file in the old format, or the wrong shape, lets nobody in without failing", async () => {
  const old = JSON.stringify({
    users: [
      { phone: "+15195550100", name: "Zach", admin: true },
      { phone: "+15195550101", name: "Uncle Dave", admin: false },
    ],
  });
  for (const text of [old, "{}", "[]", "null", '{"users":null}', '{"users":"zach"}', '{"users":{}}', "42"]) {
    const { users } = usersFrom(text);
    assert.equal(await users.find("zach"), null, text);
    assert.equal(await users.findByKey(newKey()), null, text);
  }
  // No file at all is an empty list.
  assert.equal(await usersFrom(undefined).users.find("zach"), null);
});

test("when an id is listed twice the first entry counts, for links and cookies alike", async () => {
  const first = person("uncle", "First");
  const second = person("uncle", "Second");
  const { users } = usersFrom(JSON.stringify({ users: [first.entry, second.entry] }));
  assert.equal((await users.find("uncle")).name, "First");
  assert.equal((await users.findByKey(first.key)).name, "First");
  assert.equal(await users.findByKey(second.key), null);
});

test("users.json is re-read every 30 seconds, and a failed read is not kept", async () => {
  const uncle = person("uncle", "Uncle Dave");
  const { store, clock, users } = usersFrom(JSON.stringify({ users: [uncle.entry] }));
  assert.ok(await users.find("uncle"));

  await store.write("users.json", JSON.stringify({ users: [] }));
  clock.now += 29 * SECOND;
  assert.ok(await users.find("uncle"), "still cached");
  clock.now += SECOND;
  assert.equal(await users.find("uncle"), null);

  // A file that is not JSON fails the request that read it...
  await store.write("users.json", "{not json");
  clock.now += 30 * SECOND;
  await assert.rejects(users.find("uncle"), SyntaxError);
  // ...and the next one reads again rather than reusing the failure.
  await store.write("users.json", JSON.stringify({ users: [uncle.entry] }));
  assert.ok(await users.find("uncle"));
});
