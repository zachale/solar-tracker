#include "./Telemetry.h"

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

size_t Telemetry::size()
{
  File file = SD.open(logFile, FILE_READ);
  if (!file)
  {
    return 0;
  }
  size_t bytes = file.size();
  file.close();
  return bytes;
}

bool Telemetry::append(const String &row)
{
  // Drop the spool rather than let it grow past what can be held in memory for
  // the upload. This only happens when uploads have been failing for over a
  // day, in which case losing the backlog is preferable to running out of heap.
  if (size() >= maxBytes)
  {
    clear();
  }

  File file = SD.open(logFile, FILE_WRITE);
  if (!file)
  {
    return false;
  }
  // print() twice rather than println(): Arduino's println() terminates with
  // CRLF, and a stray carriage return at the end of every field set is one more
  // thing the server has to strip.
  file.print(row);
  file.print("\n");
  file.close();
  return true;
}

String Telemetry::readAll()
{
  File file = SD.open(logFile, FILE_READ);
  if (!file)
  {
    return "";
  }

  size_t bytes = file.size();
  if (bytes > maxBytes)
  {
    bytes = maxBytes;
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

void Telemetry::clear()
{
  SD.remove(logFile);
}
