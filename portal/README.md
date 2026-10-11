# Tracker portal API

A small read-only API behind the solar tracker page on zachlegesse.ca. It serves the tracker's
telemetry to a handful of signed-in people (family) and handles their sign-in. There are no
passwords, no codes and no login screen: Zach gives each person a personal link, and opening it
signs in that device for good.

The page itself is an Angular component in the
[zachale/portfolio](https://github.com/zachale/portfolio) repo under `src/app/tracker`.

## Architecture

```
tracker (field unit)
   | CSV over HTTPS, a batch every ~15 min
   v
test-image (Cloud Run, the upload service; not part of this)
   | one immutable object per upload
   v
gs://solar-tracker-442903-telemetry/raw/<UTC date>/<UTC upload time>.ndjson
   | read-only
   v
tracker-api (Cloud Run, this service)  <-->  gs://solar-tracker-442903-portal
   ^                                          users.json
   | Netlify proxy: /api/tracker/*  ->  <tracker-api URL>/:splat  (200 rewrite)
   |
the page on zachlegesse.ca (and Netlify deploy previews)
```

Because Netlify rewrites `/api/tracker/*` to this service, the browser sees the API on the page's
own origin: no CORS, and the cookie is first-party. The service serves its routes at the root
(`/session`, `/telemetry`, ...) and never sees the `/api/tracker` prefix.

The tracker's clock is set from a wall-clock string, so each row's `time` is Toronto local time
labelled `Z`. The API converts every row to the real instant (`src/time.js`) before serving it, and
drops rows the clock got wrong after a power loss: anything dated more than an hour after the upload
that carried it, or more than an hour after the request (the bucket has rows from 2000 and 2043;
the old ones fall outside every window).

## Endpoints

All responses are JSON unless noted. Every POST must be sent as `Content-Type: application/json`
(an empty body is fine) or it is refused with 415.

| Method | Path | Who | What |
| --- | --- | --- | --- |
| GET | `/health` | anyone | `ok` (text). Not `/healthz`: Cloud Run's front end reserves paths ending in `z` |
| POST | `/auth/link` | anyone | `{key}` from a personal link → 200 `{name}` and the session cookie. 400 `invalid_key` unless `key` is a string of 20 to 100 base64url characters; 401 `invalid_link` if it is nobody's current key (never issued, re-issued since, or its person removed). |
| POST | `/auth/logout` | anyone | Clears the session cookie. 204. |
| GET | `/session` | signed in | `{name}` |
| GET | `/telemetry?days=N` | signed in | See below. `N` is 1 to 60, default 7; otherwise 400 `invalid_days`. |

Signed-in routes answer 401 `unauthenticated` without a valid session, and clear the cookie if
one was sent.

`/telemetry` is sent with `Cache-Control: private, max-age=30` (everything else is `no-store`):

```json
{
  "generatedAt": "2026-10-11T19:37:56.111Z",
  "lastUploadAt": "2026-10-11T19:34:05.866Z",
  "statuses": ["ACTIVE", "NIGHT", "SAFE", "AWAY"],
  "rows": [[1791747549000, 100, 21.25, 3, 0, 1]]
}
```

Each row is `[tMs, extendedPercent, temperatureC, windPeak, statusIndex, windSensor]`: the real
instant in epoch milliseconds, ascending and unique; the index into `statuses` (ACTIVE tracking,
NIGHT parked for the night, SAFE parked for wind, AWAY parked by hand); and `windSensor` 0 or 1.
`lastUploadAt` is when the newest upload in the window arrived, or null.

## Signing in

Each person has one personal link, `https://zachlegesse.ca/tracker#key=<key>`, where the key is 32
random bytes in base64url (43 characters). `tools/portal/users.sh` makes it and prints it once;
only the key's SHA-256 is stored, in `gs://solar-tracker-442903-portal/users.json`:

```json
{"users": [{"id": "uncle", "name": "Uncle Dave", "keyHash": "<SHA-256 of the key, hex>", "issuedAt": "2026-10-11T21:49:41.479Z"}]}
```

1. Zach runs `tools/portal/users.sh add uncle "Uncle Dave"` and sends the printed link to his
   uncle, and only to him.
2. Opening the link loads the page, which reads `key` from the fragment, takes the fragment out of
   the address bar (`history.replaceState`) and posts it to `POST /auth/link`.
3. The API looks up the key's hash in `users.json` and sets the session cookie. That device is now
   signed in, with nothing to type and nobody to approve.
4. From then on the page only calls `GET /session`. A device that is not signed in (a new one, or
   one that signed out) gets back in by opening the link again; the link can be used any number of
   times, on any number of devices.

Entries in `users.json` that are not in this format (including the earlier phone-number format) are
ignored, not an error: a stale file signs nobody in and breaks nothing.

Security properties, stated plainly:

- **A link is a bearer credential.** Whoever has it becomes that person, on as many devices as
  they like, with nothing else to prove: a forwarded message, a screenshot or someone else reading
  the person's texts is enough. That is acceptable here because all it unlocks is a read-only view
  of the tracker's telemetry, which is not sensitive, and the point is zero friction for family. It
  would not be the right design for anything that mattered more.
- **Revoke by re-issuing.** `users.sh link <id>` gives the person a new key. The old link stops
  working, and so does every device signed in with it: the session cookie carries the first 16 hex
  digits of the key hash it was issued under, and every signed-in request checks them against the
  person's current hash. `users.sh remove <id>` cuts them off entirely. Either takes effect within
  30 s, the time `users.json` is cached. There is no way to sign out just one of a person's devices;
  re-issue and send them the new link.
- **Out of request logs.** The key is in the URL fragment, which a browser never sends to any
  server, so it does not appear in Netlify's or Cloud Run's request logs or in a Referer header. The
  page sends it once, in a POST body, which neither logs. It does remain wherever the link was sent
  (the text or email) and in the opening browser's history.
- **Unguessable, and stored only as a hash.** 256 random bits leave nothing to brute-force, so
  there is no rate limit or lockout. `users.json` holds only SHA-256 hashes, so read access to the
  bucket does not yield a working link, and a lost link cannot be printed again, only re-issued.
- **Session cookie.** `tracker_session` is HMAC-SHA256 signed with `SESSION_SECRET`, HttpOnly,
  Secure, SameSite=Lax and Path=/. It always lasts 400 days, the most a browser allows, and is
  re-issued for another 400 days on any signed-in request once it is a day old, so a device that
  opens the page at least once every 400 days stays signed in indefinitely. It holds the person's
  id, the 16-digit key tag and its times; nothing in it is secret.
- **Signing out is local.** `POST /auth/logout` makes that browser drop its cookie; nothing is
  recorded on the server, so a copy of the cookie taken beforehand would still work. Re-issuing the
  link is the revocation. Rotating `SESSION_SECRET` (see Deploy) signs out every device but changes
  no link, so everyone gets back in by opening theirs again.
- **CSRF.** POSTs must be `application/json`, which a cross-site form cannot send and a
  cross-site fetch cannot send without a CORS preflight the API never approves; and SameSite=Lax
  keeps the cookie off cross-site POSTs anyway. Opening someone else's link signs a device in as
  them, but only someone who already holds that link can arrange it.
- **No third parties.** No SMS provider, no identity provider, no email service: nothing leaves
  Google Cloud, nothing costs per sign-in, and there is no account elsewhere to maintain.

## Configuration

| Variable | Default | |
| --- | --- | --- |
| `SESSION_SECRET` | none, required | Cookie signing key. The service refuses to start without it. |
| `TELEMETRY_BUCKET` | `solar-tracker-442903-telemetry` | Read-only. |
| `PORTAL_BUCKET` | `solar-tracker-442903-portal` | `users.json`. |
| `SITE_TZ` | `America/Toronto` | The zone of the tracker's clock. |
| `PORT` | `8080` | Set by Cloud Run. |

## One-time setup

```sh
PROJECT=solar-tracker-442903
SA=tracker-portal@$PROJECT.iam.gserviceaccount.com

# APIs for Secret Manager and deploying from source (a no-op for any already on).
gcloud services enable secretmanager.googleapis.com cloudbuild.googleapis.com \
  artifactregistry.googleapis.com run.googleapis.com --project=$PROJECT

# The portal's own bucket: private, no public access possible.
gcloud storage buckets create gs://solar-tracker-442903-portal --project=$PROJECT \
  --location=us-central1 --uniform-bucket-level-access --public-access-prevention

# A service account that can read telemetry, manage the portal bucket and nothing else.
gcloud iam service-accounts create tracker-portal --project=$PROJECT --display-name="Tracker portal API"
gcloud storage buckets add-iam-policy-binding gs://solar-tracker-442903-telemetry \
  --member=serviceAccount:$SA --role=roles/storage.objectViewer
gcloud storage buckets add-iam-policy-binding gs://solar-tracker-442903-portal \
  --member=serviceAccount:$SA --role=roles/storage.objectAdmin

# The cookie signing key, readable by that account only.
openssl rand -base64 48 | tr -d '\n' | gcloud secrets create portal-session-secret \
  --project=$PROJECT --replication-policy=automatic --data-file=-
gcloud secrets add-iam-policy-binding portal-session-secret --project=$PROJECT \
  --member=serviceAccount:$SA --role=roles/secretmanager.secretAccessor
```

## Deploy

From the repository root:

```sh
gcloud run deploy tracker-api --source portal --region us-central1 \
  --service-account tracker-portal@solar-tracker-442903.iam.gserviceaccount.com \
  --allow-unauthenticated \
  --set-secrets SESSION_SECRET=portal-session-secret:latest \
  --min-instances 0 --max-instances 2 --memory 256Mi
```

Buildpacks build it from `package.json` (Node 22, `npm start`); there is no Dockerfile.
`--allow-unauthenticated` because the browser reaches it through Netlify's proxy, which cannot
attach a Google identity token; the API does its own authentication. The Netlify rewrite lives in
the portfolio repo's `src/_redirects`:

```
/api/tracker/*  https://tracker-api-188091506191.us-central1.run.app/:splat  200
```

To sign out every device, add a new secret version and start a new revision, which reads it.
Links keep working, so this only means everyone opens their link once more:

```sh
openssl rand -base64 48 | tr -d '\n' | gcloud secrets versions add portal-session-secret --data-file=-
gcloud run services update tracker-api --region us-central1
```

### Moving over from phone-number sign-in

- Deploying this signs out every existing session: the cookie format changed, and a cookie from
  the phone-number version is refused and cleared.
- The old phone-number `users.json` signs nobody in. Recreate the list with `users.sh add` for each
  person; the first change drops the old entries.
- The page in the portfolio repo has to move to `POST /auth/link` (step 2 above) in the same
  release; the request and admin endpoints it used are gone.
- The old sign-in requests under `requests/` are no longer read. They hold names and phone numbers,
  so delete them: `gcloud storage rm "gs://solar-tracker-442903-portal/requests/**"`.

## Users

```sh
tools/portal/users.sh list
tools/portal/users.sh add uncle "Uncle Dave"   # prints his link: send it to him and nobody else
tools/portal/users.sh link uncle               # a new link; the old one and its devices stop within 30 s
tools/portal/users.sh remove uncle             # signed out everywhere within 30 s
```

An id is 1 to 32 of `a-z`, `0-9` and `-`. The link is printed once, to stdout only, and the key is
never written anywhere; if it is lost, `link` makes a new one. Links point at
`https://zachlegesse.ca/tracker`; set `PORTAL_URL` to make one for another copy of the page, such as
a deploy preview.

## Development

```sh
cd portal
npm install
npm test                   # node:test, no network
node scripts/smoke.js 2    # the real telemetry bucket, read-only; users.json in memory
```

`scripts/smoke.js` lists a person with a fresh key in an in-memory `users.json`, opens their link
over HTTP against a local server, and summarizes `/telemetry?days=N`: row count, first and last
rows, when each day's ACTIVE stretch ran in Toronto time, and how long after its newest row each of
today's uploads arrived (a few minutes if the wall-clock conversion is right). It uses the gcloud
CLI's login, so no application default credentials are needed.

## Cost

About $0 a month, inside the free tiers:

- **Cloud Run**: scales to zero, and a few people loading a page is far below the free 2 million
  requests and 180,000 vCPU-seconds a month.
- **Cloud Storage**: reads of the telemetry bucket are cached in memory (objects for good, listings
  for a minute), so a cold start costs a few hundred reads and a page left open costs two listings
  a minute; the portal bucket holds one small object, read at most every 30 s per instance.
- **Secret Manager**: one secret, read when an instance starts.
- **Cloud Build / Artifact Registry**: a deploy is a minute or two of build time; old images can be
  deleted if storage passes the free 0.5 GB.
