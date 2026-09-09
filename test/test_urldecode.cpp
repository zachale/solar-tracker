// Tests for form-value decoding.
//
// The dashboard posts as application/x-www-form-urlencoded, so anything the
// operator types that is not alphanumeric arrives escaped. Storing the escaped
// text means the device authenticates with a string nobody typed — which is
// exactly how an SSID of "Rock Lodge" became "Rock+Lodge" and a password
// containing '!' became "%21".
#include "../src/Http/UrlDecode.h"
#include "doctest.h"

TEST_SUITE("url decode") {

  TEST_CASE("plus becomes a space") {
    CHECK(urlDecode("Rock+Lodge") == String("Rock Lodge"));
    CHECK(urlDecode("a+b+c") == String("a b c"));
  }

  TEST_CASE("percent escapes become their character") {
    CHECK(urlDecode("B0%21gercreek") == String("B0!gercreek"));
    CHECK(urlDecode("%20") == String(" "));
    CHECK(urlDecode("19%3A30") == String("19:30"));
    CHECK(urlDecode("%2F%3F%26%3D") == String("/?&="));
  }

  TEST_CASE("lower case hex is accepted") {
    CHECK(urlDecode("%2f") == String("/"));
    CHECK(urlDecode("%3a") == String(":"));
  }

  TEST_CASE("unescaped text passes through untouched") {
    CHECK(urlDecode("plainSSID") == String("plainSSID"));
    CHECK(urlDecode("") == String(""));
    CHECK(urlDecode("hunter2") == String("hunter2"));
  }

  TEST_CASE("a malformed escape is preserved rather than swallowing the rest") {
    // A password ending in a literal '%' must not truncate the value.
    CHECK(urlDecode("abc%") == String("abc%"));
    CHECK(urlDecode("abc%2") == String("abc%2"));
    CHECK(urlDecode("abc%zz") == String("abc%zz"));
  }

  TEST_CASE("a realistic password survives intact") {
    CHECK(urlDecode("P%40ss+w%2Frd%21") == String("P@ss w/rd!"));
  }
}
