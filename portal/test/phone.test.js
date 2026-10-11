import test from "node:test";
import assert from "node:assert/strict";

import { normalizePhone } from "../src/phone.js";

test("a ten-digit North American number gains +1", () => {
  assert.equal(normalizePhone("5195551234"), "+15195551234");
  assert.equal(normalizePhone("(519) 555-1234"), "+15195551234");
  assert.equal(normalizePhone("519.555.1234"), "+15195551234");
  assert.equal(normalizePhone("  519 555 1234 "), "+15195551234");
});

test("eleven digits starting with 1 gain only the +", () => {
  assert.equal(normalizePhone("15195551234"), "+15195551234");
  assert.equal(normalizePhone("1-519-555-1234"), "+15195551234");
});

test("a number written with + keeps its country code", () => {
  assert.equal(normalizePhone("+1 519 555 1234"), "+15195551234");
  assert.equal(normalizePhone("+44 20 7946 0958"), "+442079460958");
  assert.equal(normalizePhone(" +33 1 23 45 67 89"), "+33123456789");
});

test("a + number must have 8 to 15 digits", () => {
  assert.equal(normalizePhone("+1234567"), null);
  assert.equal(normalizePhone("+12345678"), "+12345678");
  assert.equal(normalizePhone("+123456789012345"), "+123456789012345");
  assert.equal(normalizePhone("+1234567890123456"), null);
});

test("anything else is not a number", () => {
  assert.equal(normalizePhone("5551234"), null);
  assert.equal(normalizePhone("25195551234"), null);
  assert.equal(normalizePhone("519555123456"), null);
  assert.equal(normalizePhone(""), null);
  assert.equal(normalizePhone("+"), null);
  assert.equal(normalizePhone("call me"), null);
  assert.equal(normalizePhone(undefined), null);
  assert.equal(normalizePhone(5195551234), null);
  assert.equal(normalizePhone({ phone: "5195551234" }), null);
});
