#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "../BenchTest.h"

// Spools telemetry rows onto the SD card as a series of numbered chunk files,
// tlm0.csv through tlm63.csv, each one small enough to be POSTed in a single
// request from a board with 32 KB of RAM.
//
// Chunking is per file rather than by offset into one big file on purpose. The
// tracker is switched off by hand, so any scheme that rewrote a file to drop the
// rows it had already sent would risk losing the whole spool to a power cut
// mid-rewrite. Deleting a whole file once the server has confirmed it is atomic
// and cannot leave a half state.
//
// Rows carry their own timestamps and the server stores each batch as its own
// object, so the order chunks are uploaded in does not matter. That is what makes
// it safe to wrap around and overwrite the oldest chunk when the card has more
// unsent data than this is willing to keep.
//
// Every entry point tolerates failure quietly. Telemetry is the least important
// job this firmware has and the device cannot be serviced remotely, so a missing
// card or a failed write must never do anything worse than lose a row.
class Telemetry
{
public:
#if BENCH_TEST
  // Small enough that a bench run fills several chunks in a few minutes and the
  // multiple chunk upload path actually gets exercised.
  static constexpr size_t chunkBytes = 256;
#else
  // Roughly 117 rows. Comfortably inside the ~21 KB of free RAM, with room for
  // the request the body is copied into.
  static constexpr size_t chunkBytes = 4096;
#endif

  // 64 chunks is about 50 days of ten minute rows. Past that the oldest unsent
  // chunk is overwritten rather than letting the card fill indefinitely.
  static const int maxChunks = 64;

  static String formatRow(uint32_t timestamp,
                          float temperature,
                          int extendedPercent,
                          const String &status,
                          int windPeak,
                          bool settingsServer,
                          bool windSensor,
                          bool away);

  static bool append(const String &row);

  // Index of the first chunk at or after `from` that holds data, or -1 if there
  // is none. Callers walk the chunks with this rather than assuming they are
  // contiguous, because uploads delete them out of order after a wrap.
  static int nextChunk(int from);
  static String readChunk(int index);
  static void clearChunk(int index);
  static size_t chunkSize(int index);

  // Index currently being appended to, for logging.
  static int currentIndex();

  // Forget the cached write position so the next append works it out from the card
  // again. Called once the spool has been emptied, which lets writing start back at
  // chunk 0 rather than carrying on from wherever the ring happened to be.
  static void resume();

private:
  static int writeIndex;
  static String pathFor(int index);
  static void selectWriteIndex(size_t incoming);
};

#endif
