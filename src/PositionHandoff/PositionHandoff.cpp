#include "./PositionHandoff.h"
#include <SD.h>

bool PositionHandoff::save(const char *version, const LinearActuator::Snapshot &s, uint32_t now)
{
  SD.remove(FILE_NAME); // never leave an older one behind
  if (s.maxPos <= 0)
  {
    return false; // nothing worth handing over: the next boot calibrates either way
  }
  File f = SD.open(FILE_NAME, FILE_WRITE);
  if (!f)
  {
    return false;
  }
  f.print(String(version) + " " + String(now) + " " + String(s.pos) + " " + String(s.maxPos) + " " +
          String(s.sinceCalibrationMs) + " " + String(s.target) + " " + String(s.owed ? 1 : 0) + "\n");
  f.close();
  return true;
}

bool PositionHandoff::take(const char *version, uint32_t now, LinearActuator::Snapshot &s)
{
  if (!SD.exists(FILE_NAME))
  {
    return false;
  }
  File f = SD.open(FILE_NAME, FILE_READ);
  String line = f ? f.readString() : String();
  if (f)
  {
    f.close();
  }
  SD.remove(FILE_NAME);

  char written[17] = {0};
  unsigned long at = 0, since = 0;
  long pos = 0, maxPos = 0;
  int target = -1, owed = 0;
  if (sscanf(line.c_str(), "%16s %lu %ld %ld %lu %d %d", written, &at, &pos, &maxPos, &since, &target, &owed) != 7)
  {
    return false;
  }
  if (strcmp(written, version) != 0 || maxPos <= 0 || now < at || now - at > MAX_AGE_S)
  {
    return false;
  }
  s = LinearActuator::Snapshot{pos, maxPos, since, target, owed != 0};
  return true;
}
