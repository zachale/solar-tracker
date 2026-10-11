// The portal sets one cookie, the session (session.js), with these attributes:
//   Secure    it is a credential on its own, so never sent in clear;
//   HttpOnly  script on the page has no use for it and must not read it;
//   SameSite=Lax  not sent on cross-site POSTs (see the CSRF note in app.js);
//   Path=/    the API is reached through Netlify at /api/tracker/*, a prefix this
//             service never sees, so it cannot scope it any tighter.
const ATTRIBUTES = "Path=/; HttpOnly; Secure; SameSite=Lax";

export function setCookie(name, value, maxAgeSeconds) {
  return `${name}=${value}; ${ATTRIBUTES}; Max-Age=${maxAgeSeconds}`;
}

export function clearCookie(name) {
  return setCookie(name, "", 0);
}

// The Cookie header is "a=1; b=2". Parsed here rather than with a dependency
// because the value never needs decoding: it is base64url with one dot.
export function readCookie(header, name) {
  if (typeof header !== "string") return undefined;
  for (const part of header.split(";")) {
    const eq = part.indexOf("=");
    if (eq !== -1 && part.slice(0, eq).trim() === name) return part.slice(eq + 1).trim();
  }
  return undefined;
}
