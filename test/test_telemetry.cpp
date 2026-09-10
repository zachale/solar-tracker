// Telemetry spooling: row formatting, the append/read/clear round trip, and the
// size cap that stops the spool from outgrowing the heap.
#include "doctest.h"
#include <SD.h>
#include "../src/Telemetry/Telemetry.h"

namespace {
int countLines(const String &s)
{
  int n = 0;
  for (char c : s.s)
  {
    if (c == '\n') n++;
  }
  return n;
}
}

TEST_SUITE("telemetry")
{
  TEST_CASE("formatRow emits the eight fields in order")
  {
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 3, true, true, false);
    CHECK(row.s == "1788947478,22.25,29,ACTIVE,3,1,1,0");
  }

  TEST_CASE("formatRow renders booleans as 1 and 0, never true/false")
  {
    String row = Telemetry::formatRow(1u, 0.0f, 0, "AWAY", 0, false, false, true);
    CHECK(row.s == "1,0.00,0,AWAY,0,0,0,1");
  }

  TEST_CASE("formatRow keeps two decimals of temperature and rounds nothing else")
  {
    String row = Telemetry::formatRow(42u, -7.5f, 100, "SAFE", 31, true, false, false);
    CHECK(row.s == "42,-7.50,100,SAFE,31,1,0,0");
  }

  TEST_CASE("a row is stored with a single newline and no carriage return")
  {
    mock::reset();
    Telemetry::append("1,2.00,3,ACTIVE,4,1,1,0");
    String stored = Telemetry::readAll();
    CHECK(stored.s == "1,2.00,3,ACTIVE,4,1,1,0\n");
    CHECK(stored.indexOf('\r') == -1);
  }

  TEST_CASE("appended rows accumulate in order")
  {
    mock::reset();
    Telemetry::append("first");
    Telemetry::append("second");
    Telemetry::append("third");
    CHECK(Telemetry::readAll().s == "first\nsecond\nthird\n");
    CHECK(countLines(Telemetry::readAll()) == 3);
  }

  TEST_CASE("the log file name fits the SD library's 8.3 limit")
  {
    // A name the hardware rejects would make every append silently fail, so this
    // is checked against the same rule the SD stub enforces.
    CHECK(isValid83(Telemetry::logFile));
  }

  TEST_CASE("reading an absent spool yields an empty string, not a failure")
  {
    mock::reset();
    CHECK(Telemetry::readAll().s == "");
    CHECK(Telemetry::size() == 0);
  }

  TEST_CASE("clear removes the spool")
  {
    mock::reset();
    Telemetry::append("a row");
    REQUIRE(Telemetry::size() > 0);
    Telemetry::clear();
    CHECK(Telemetry::size() == 0);
    CHECK(Telemetry::readAll().s == "");
  }

  TEST_CASE("clearing an already absent spool is harmless")
  {
    mock::reset();
    Telemetry::clear();
    Telemetry::clear();
    CHECK(Telemetry::size() == 0);
  }

  TEST_CASE("size tracks the bytes written")
  {
    mock::reset();
    Telemetry::append("12345");            // five characters plus newline
    CHECK(Telemetry::size() == 6);
    Telemetry::append("678");
    CHECK(Telemetry::size() == 10);
  }

  TEST_CASE("the spool is dropped once it reaches the cap instead of growing")
  {
    mock::reset();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 3, true, true, false);
    size_t rowBytes = row.length() + 1;

    while (Telemetry::size() + rowBytes < Telemetry::maxBytes)
    {
      REQUIRE(Telemetry::append(row));
    }
    size_t beforeCap = Telemetry::size();
    REQUIRE(beforeCap < Telemetry::maxBytes);

    // Push it over the line; the next append has to start a fresh spool.
    while (Telemetry::size() < Telemetry::maxBytes)
    {
      Telemetry::append(row);
    }
    REQUIRE(Telemetry::size() >= Telemetry::maxBytes);

    Telemetry::append(row);
    CHECK(Telemetry::size() == rowBytes);
    CHECK(countLines(Telemetry::readAll()) == 1);
  }

  TEST_CASE("a spool at the cap still reads back whole and fits the upload budget")
  {
    mock::reset();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 3, true, true, false);
    while (Telemetry::size() + row.length() + 1 < Telemetry::maxBytes)
    {
      Telemetry::append(row);
    }
    String body = Telemetry::readAll();
    CHECK(body.length() == Telemetry::size());
    CHECK(body.length() <= Telemetry::maxBytes);
    CHECK(body.s.back() == '\n');
  }

  TEST_CASE("ten minute sampling keeps a day of readings inside the cap")
  {
    mock::reset();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 31, true, true, false);
    const int rowsPerDay = 24 * 6;
    CHECK((row.length() + 1) * rowsPerDay < Telemetry::maxBytes);
  }
}
