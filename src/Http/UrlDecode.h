#ifndef URL_DECODE_H
#define URL_DECODE_H

#include <Arduino.h>

// Decodes one application/x-www-form-urlencoded value.
//
// Browsers post form fields with spaces as '+' and anything outside the
// unreserved set as a percent escape, so a password containing '!' arrives as
// "%21" and an SSID with a space arrives with a '+' in it. Storing those raw
// means the device tries to authenticate with text the operator never typed.
//
// A trailing '%' with fewer than two hex digits behind it is passed through
// unchanged rather than dropped, so malformed input cannot truncate the value.
inline String urlDecode(const String &in)
{
  String out;
  for (unsigned int i = 0; i < in.length(); i++)
  {
    char c = in[i];
    if (c == '+')
    {
      out += ' ';
    }
    else if (c == '%' && i + 2 < in.length() &&
             isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2]))
    {
      char hex[3] = {in[i + 1], in[i + 2], '\0'};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    }
    else
    {
      out += c;
    }
  }
  return out;
}

#endif
