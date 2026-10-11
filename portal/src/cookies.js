// The portal sets two cookies, the session (session.js) and a sign-in request's
// secret (requests.js), with the same attributes:
//   Secure    each is a credential on its own, so never sent in clear;
//   HttpOnly  script on the page has no use for either and must not read them;
//   SameSite=Lax  not sent on cross-site POSTs (see the CSRF note in app.js);
//   Path=/    the API is reached through Netlify at /api/tracker/*, a prefix this
//             service never sees, so it cannot scope them any tighter.
const ATTRIBUTES = "Path=/; HttpOnly; Secure; SameSite=Lax";

// Without maxAgeSeconds the cookie lasts until the browser closes.
export function setCookie(name, value, maxAgeSeconds) {
  return `${name}=${value}; ${ATTRIBUTES}${maxAgeSeconds === undefined ? "" : `; Max-Age=${maxAgeSeconds}`}`;
}

export function clearCookie(name) {
  return setCookie(name, "", 0);
}

// The Cookie header is "a=1; b=2". Parsed here rather than with a dependency
// because the values never need decoding: both are base64url, the session with
// one dot.
export function readCookie(header, name) {
  if (typeof header !== "string") return undefined;
  for (const part of header.split(";")) {
    const eq = part.indexOf("=");
    if (eq !== -1 && part.slice(0, eq).trim() === name) return part.slice(eq + 1).trim();
  }
  return undefined;
}
