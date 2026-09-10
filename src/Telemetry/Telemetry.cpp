#include "./Telemetry.h"

int Telemetry::writeIndex = -1;

String Telemetry::formatRow(uint32_t timestamp,
                            float temperature,
                            int extendedPercent,
                            const String &status,
                            int windPeak,
                            bool settingsServer,
                            bool windSensor,
                            bool away)
{
  String row;
  row.reserve(48);
  row += String(timestamp);
  row += ',';
  row += String(temperature, 2);
  row += ',';
  row += String(extendedPercent);
  row += ',';
  row += status;
  row += ',';
  row += String(windPeak);
  row += ',';
  row += settingsServer ? '1' : '0';
  row += ',';
  row += windSensor ? '1' : '0';
  row += ',';
  row += away ? '1' : '0';
  return row;
}

String Telemetry::pathFor(int index)
{
  return "tlm" + String(index) + ".csv";
}

size_t Telemetry::chunkSize(int index)
{
  File file = SD.open(pathFor(index).c_str(), FILE_READ);
  if (!file)
  {
    return 0;
  }
  size_t bytes = file.size();
  file.close();
  return bytes;
}

int Telemetry::currentIndex()
{
  return writeIndex < 0 ? 0 : writeIndex;
}

void Telemetry::resume()
{
  writeIndex = -1;
}

int Telemetry::nextChunk(int from)
{
  for (int i = from; i < maxChunks; i++)
  {
    if (chunkSize(i) > 0)
    {
      return i;
    }
  }
  return -1;
}

// Decide which chunk the next row belongs in. On the first call after a reboot the
// card is the only record of where writing left off, so the highest numbered chunk
// that already exists is resumed rather than starting again at zero and
// overwriting it.
void Telemetry::selectWriteIndex(size_t incoming)
{
  if (writeIndex < 0)
  {
    writeIndex = 0;
    for (int i = maxChunks - 1; i >= 0; i--)
    {
      if (chunkSize(i) > 0)
      {
        writeIndex = i;
        break;
      }
    }
  }

  if (chunkSize(writeIndex) + incoming <= chunkBytes)
  {
    return;
  }

  writeIndex = (writeIndex + 1) % maxChunks;
  // Wrapping onto a chunk that still holds unsent rows means the card has more
  // backlog than maxChunks will keep. The oldest goes, because recent readings
  // are the ones worth having.
  if (chunkSize(writeIndex) > 0)
  {
    clearChunk(writeIndex);
  }
}

bool Telemetry::append(const String &row)
{
  selectWriteIndex(row.length() + 1);

  File file = SD.open(pathFor(writeIndex).c_str(), FILE_WRITE);
  if (!file)
  {
    return false;
  }
  // print() twice rather than println(): Arduino's println() terminates with
  // CRLF, and a stray carriage return at the end of every row is one more thing
  // the server has to strip.
  file.print(row);
  file.print("\n");
  file.close();
  return true;
}

String Telemetry::readChunk(int index)
{
  File file = SD.open(pathFor(index).c_str(), FILE_READ);
  if (!file)
  {
    return "";
  }

  size_t bytes = file.size();
  if (bytes > chunkBytes)
  {
    bytes = chunkBytes;
  }

  // Read a known number of bytes rather than using readString(), which waits out
  // the stream timeout at end of file. Reserving up front keeps this to a single
  // allocation instead of letting String grow by doubling.
  String contents;
  contents.reserve(bytes + 1);
  for (size_t i = 0; i < bytes; i++)
  {
    int c = file.read();
    if (c < 0)
    {
      break;
    }
    contents += (char)c;
  }
  file.close();
  return contents;
}

void Telemetry::clearChunk(int index)
{
  SD.remove(pathFor(index).c_str());
}
