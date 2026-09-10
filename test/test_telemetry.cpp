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
    Telemetry::resume();
    Telemetry::append("1,2.00,3,ACTIVE,4,1,1,0");
    String stored = Telemetry::readChunk(0);
    CHECK(stored.s == "1,2.00,3,ACTIVE,4,1,1,0\n");
    CHECK(stored.indexOf('\r') == -1);
  }

  TEST_CASE("appended rows accumulate in order inside one chunk")
  {
    mock::reset();
    Telemetry::resume();
    Telemetry::append("first");
    Telemetry::append("second");
    Telemetry::append("third");
    CHECK(Telemetry::readChunk(0).s == "first\nsecond\nthird\n");
    CHECK(countLines(Telemetry::readChunk(0)) == 3);
  }

  TEST_CASE("chunk file names fit the SD library's 8.3 limit")
  {
    // A name the hardware rejects would make every append silently fail, so each
    // one is checked against the same rule the SD stub enforces.
    mock::reset();
    Telemetry::resume();
    for (int i = 0; i < Telemetry::maxChunks; i++)
    {
      CHECK(isValid83(("tlm" + std::to_string(i) + ".csv")));
    }
  }

  TEST_CASE("reading an absent chunk yields an empty string, not a failure")
  {
    mock::reset();
    Telemetry::resume();
    CHECK(Telemetry::readChunk(0).s == "");
    CHECK(Telemetry::chunkSize(0) == 0);
    CHECK(Telemetry::nextChunk(0) == -1);
  }

  TEST_CASE("clearing a chunk removes it, and clearing an absent one is harmless")
  {
    mock::reset();
    Telemetry::resume();
    Telemetry::append("a row");
    REQUIRE(Telemetry::chunkSize(0) > 0);
    Telemetry::clearChunk(0);
    CHECK(Telemetry::chunkSize(0) == 0);
    Telemetry::clearChunk(0);
    CHECK(Telemetry::chunkSize(0) == 0);
  }

  TEST_CASE("chunkSize tracks the bytes written")
  {
    mock::reset();
    Telemetry::resume();
    Telemetry::append("12345");            // five characters plus newline
    CHECK(Telemetry::chunkSize(0) == 6);
    Telemetry::append("678");
    CHECK(Telemetry::chunkSize(0) == 10);
  }

  TEST_CASE("a full chunk rolls over to the next one instead of overflowing")
  {
    mock::reset();
    Telemetry::resume();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 3, true, true, false);
    size_t rowBytes = row.length() + 1;

    while (Telemetry::currentIndex() == 0)
    {
      REQUIRE(Telemetry::append(row));
    }

    CHECK(Telemetry::currentIndex() == 1);
    CHECK(Telemetry::chunkSize(0) <= Telemetry::chunkBytes);
    CHECK(Telemetry::chunkSize(0) + rowBytes > Telemetry::chunkBytes);
    CHECK(Telemetry::chunkSize(1) == rowBytes);
  }

  TEST_CASE("no chunk ever exceeds the upload budget")
  {
    mock::reset();
    Telemetry::resume();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 31, true, true, false);
    for (int i = 0; i < 500; i++)
    {
      Telemetry::append(row);
    }
    for (int i = 0; i < Telemetry::maxChunks; i++)
    {
      CHECK(Telemetry::chunkSize(i) <= Telemetry::chunkBytes);
    }
  }

  TEST_CASE("nextChunk walks only the chunks that hold data")
  {
    mock::reset();
    Telemetry::resume();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 3, true, true, false);
    while (Telemetry::currentIndex() < 3)
    {
      Telemetry::append(row);
    }

    CHECK(Telemetry::nextChunk(0) == 0);

    // Uploading deletes out of order once a wrap has happened, so gaps have to be
    // skipped rather than assumed away.
    Telemetry::clearChunk(0);
    Telemetry::clearChunk(1);
    CHECK(Telemetry::nextChunk(0) == 2);
    CHECK(Telemetry::nextChunk(3) == 3);
    CHECK(Telemetry::nextChunk(4) == -1);
  }

  TEST_CASE("every chunk read back is whole and newline terminated")
  {
    mock::reset();
    Telemetry::resume();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 3, true, true, false);
    for (int i = 0; i < 200; i++)
    {
      Telemetry::append(row);
    }
    for (int i = Telemetry::nextChunk(0); i >= 0; i = Telemetry::nextChunk(i + 1))
    {
      String body = Telemetry::readChunk(i);
      CHECK(body.length() == Telemetry::chunkSize(i));
      CHECK(body.s.back() == '\n');
    }
  }

  TEST_CASE("wrapping past the last chunk drops the oldest rather than growing")
  {
    mock::reset();
    Telemetry::resume();
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 3, true, true, false);

    // Fill every chunk, then keep going so the write index has to wrap onto 0.
    while (Telemetry::currentIndex() < Telemetry::maxChunks - 1)
    {
      Telemetry::append(row);
    }
    while (Telemetry::currentIndex() != 0)
    {
      Telemetry::append(row);
    }

    // Chunk 0 was reset on the wrap, so it holds only what has been written since.
    CHECK(Telemetry::chunkSize(0) > 0);
    CHECK(Telemetry::chunkSize(0) <= Telemetry::chunkBytes);
    for (int i = 0; i < Telemetry::maxChunks; i++)
    {
      CHECK(Telemetry::chunkSize(i) <= Telemetry::chunkBytes);
    }
  }

  TEST_CASE("a day of ten minute rows fits well inside the chunk ring")
  {
    String row = Telemetry::formatRow(1788947478u, 22.25f, 29, "ACTIVE", 31, true, true, false);
    const int rowsPerDay = 24 * 6;
    size_t perDay = (row.length() + 1) * rowsPerDay;
    CHECK(perDay < Telemetry::chunkBytes * Telemetry::maxChunks);
  }
}
