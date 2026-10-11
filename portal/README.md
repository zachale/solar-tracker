# Tracker portal API

A small read-only API behind the solar tracker page on zachlegesse.ca. It serves the tracker's
telemetry to a handful of signed-in people (family) and handles their sign-in. There are no
passwords and no texted codes: someone on the allowlist asks to sign in, Zach approves the
request, and that browser is in.

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
   ^                                          users.json, requests/
   | Netlify proxy: /api/tracker/*  ->  <tracker-api URL>/:splat  (200 rewrite)
   |
the page on zachlegesse.ca (and Netlify deploy previews)
```

Because Netlify rewrites `/api/tracker/*` to this service, the browser sees the API on the page's
own origin: no CORS, and the cookies are first-party. The service serves its routes at the root
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
| POST | `/auth/request` | anyone | `{phone, remember}` → 202 `{ok: true}` and a `tracker_request` cookie, always. 400 `invalid_phone` if it is not a phone number. |
| GET | `/auth/request` | the asking browser | `{status: "pending"}`, `{status: "approved", name, admin}` (and the session cookie is set), `{status: "denied"}` or `{status: "expired"}`. 404 `no_request` without a request cookie. |
| POST | `/auth/request/cancel` | the asking browser | Forgets the request. 204. |
| POST | `/auth/logout` | anyone | Clears the session cookie. 204. |
| GET | `/session` | signed in | `{name, admin}`, else 401 `unauthenticated` |
| GET | `/telemetry?days=N` | signed in | See below. `N` is 1 to 60, default 7; otherwise 400 `invalid_days`. |
| GET | `/admin/requests` | admin | `{requests: [{id, name, phone, requestedAt, expiresAt}]}`, requests waiting for an answer, newest first |
| POST | `/admin/requests/:id/approve` | admin | 200 `{ok: true}`; 400 `invalid_id` unless `id` is 64 hex digits; 404 `not_found` if there is no such open request |
| POST | `/admin/requests/:id/deny` | admin | as approve |

Signed-in routes answer 401 `unauthenticated` without a valid session, and admin routes 403
`forbidden` for a signed-in non-admin.

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

1. The page posts the person's number (and whether to remember them) to `POST /auth/request`.
   If the number is in `users.json`, the API files a request; either way it answers 202 and sets
   `tracker_request`, an HttpOnly cookie holding a 32-byte random secret.
2. The request appears in the admin view on the page (and in `tools/portal/requests.sh list`)
   with the person's name and number.
3. The page polls `GET /auth/request`. Once Zach approves, the next poll sets the session cookie
   and the person is in. A denial or a request that lapses ends the wait.

Security properties:

- **Allowlist.** Only numbers in `gs://solar-tracker-442903-portal/users.json` can ever be signed
  in, and only someone with an admin session (or bucket access) can approve them.
- **No enumeration.** `POST /auth/request` answers the same for every well-formed number: 202, a
  request cookie, and at least half a second. For a number that is not on the list (or one that
  already has three requests waiting) the secret is random and nothing is filed, and polling it
  says `pending` until the cookie expires, exactly like a request nobody has answered.
- **The approval belongs to the browser that asked.** The request is filed under the SHA-256 of
  the secret, and that hash is the id the admin sees. Only the asking browser holds the secret, in
  an HttpOnly cookie, so knowing the id (or the person's number) does not let anyone else collect
  the approval. A phone number is not a secret, so the approval is the real check: approve only
  requests you are expecting.
- **24 hours.** A request, answered or not, lapses 24 hours after it was made, and the request
  cookie lasts as long. At most three can be waiting per number.
- **Session cookie.** `tracker_session` is HMAC-SHA256 signed with `SESSION_SECRET`, HttpOnly,
  Secure and SameSite=Lax. "Remember me" makes it last 90 days, renewed on use once it is a day
  old; otherwise it is a browser-session cookie good for 12 hours at most.
- **Revocation.** Every signed-in request also checks that the number is still in `users.json`
  (re-read every 30 s), so removing someone signs them out within half a minute, and an approved
  request is not honoured for someone removed before collecting it. Rotating `SESSION_SECRET` signs
  everyone out.
- **CSRF.** POSTs must be `application/json`, which a cross-site form cannot send and a
  cross-site fetch cannot send without a CORS preflight the API never approves; and SameSite=Lax
  keeps the cookies off cross-site POSTs anyway.
- **No third parties.** No SMS provider, no identity provider: nobody's number leaves Google
  Cloud, nothing costs per message, and there is no account elsewhere to maintain.

Known limits: someone who knows a listed number can file up to three requests that Zach will see
and should deny. A client that ignores cookie expiry and keeps polling past 24 hours sees
`expired` for a real request but `pending` for a number not on the list. Cloud Run is capped at two
instances and the three-request cap is enforced per instance, so a burst split across both could
file up to six.

## Configuration

| Variable | Default | |
| --- | --- | --- |
| `SESSION_SECRET` | none, required | Cookie signing key. The service refuses to start without it. |
| `TELEMETRY_BUCKET` | `solar-tracker-442903-telemetry` | Read-only. |
| `PORTAL_BUCKET` | `solar-tracker-442903-portal` | `users.json` and `requests/`. |
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

To sign everyone out, add a new secret version and start a new revision, which reads it:

```sh
openssl rand -base64 48 | tr -d '\n' | gcloud secrets versions add portal-session-secret --data-file=-
gcloud run services update tracker-api --region us-central1
```

## Users

```sh
tools/portal/users.sh list
tools/portal/users.sh add 519-555-1234 "Aunt May"          # --admin to let them approve requests
tools/portal/users.sh remove 519-555-1234                  # signed out within 30 s
tools/portal/requests.sh list                              # open requests, with ids
tools/portal/requests.sh approve 3fa29c                    # any unique id prefix of 6+ characters
tools/portal/requests.sh deny 3fa29c
```

The very first sign-in has no admin to approve it, so it is approved from the command line:

1. `tools/portal/users.sh add <your number> Zach --admin`
2. On the page, ask to sign in with that number.
3. `tools/portal/requests.sh list`, then `tools/portal/requests.sh approve <id>`. The page signs in
   on its next poll.

After that, approve people from the admin view on the page.

## Development

```sh
cd portal
npm install
npm test                   # node:test, no network
node scripts/smoke.js 2    # the real telemetry bucket, read-only; sign-in state in memory
```

`scripts/smoke.js` signs in over HTTP against a local server, approving its own request in memory,
and summarizes `/telemetry?days=N`: row count, first and last rows, when each day's ACTIVE stretch
ran in Toronto time, and how long after its newest row each of today's uploads arrived (a few
minutes if the wall-clock conversion is right). It uses the gcloud CLI's login, so no application
default credentials are needed.

## Cost

About $0 a month, inside the free tiers:

- **Cloud Run**: scales to zero, and a few people loading a page is far below the free 2 million
  requests and 180,000 vCPU-seconds a month.
- **Cloud Storage**: reads of the telemetry bucket are cached in memory (objects for good, listings
  for a minute), so a cold start costs a few hundred reads and a page left open costs two listings
  a minute; the portal bucket holds a few small objects.
- **Secret Manager**: one secret, read when an instance starts.
- **Cloud Build / Artifact Registry**: a deploy is a minute or two of build time; old images can be
  deleted if storage passes the free 0.5 GB.
