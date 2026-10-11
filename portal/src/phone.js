// A phone number is the only identity the portal knows, so every number is reduced
// to one E.164 form before it is compared, hashed or stored. Without that,
// "519-555-1234" typed on the page and "+15195551234" in users.json would be two
// different people. tools/portal/users.sh applies the same rules in Python; keep
// the two in step.
//
// Ten digits are a North American number missing its country code, eleven
// starting with 1 are one with it. Anything else has to be written with a leading
// + so the country code is the caller's choice rather than a guess.
export function normalizePhone(raw) {
  if (typeof raw !== "string") return null;
  const trimmed = raw.trim();
  const digits = trimmed.replace(/\D/g, "");

  if (trimmed.startsWith("+")) {
    return digits.length >= 8 && digits.length <= 15 ? `+${digits}` : null;
  }
  if (digits.length === 10) return `+1${digits}`;
  if (digits.length === 11 && digits.startsWith("1")) return `+${digits}`;
  return null;
}
