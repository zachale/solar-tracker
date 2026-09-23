# 10 — Reconcile the server branches

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing. This is the first thing to do in the set.
**Blocks:** 02 (serve the `.ota`), 06 (log ingest endpoint), 07 (telemetry schema v2) — all three edit `server/`
**Estimate:** 3.5 hours of work, plus up to 30 minutes of waiting for one live device sync to confirm. No firmware change, no flash, no device risk.
**Cost:** none. No new resources. Dropping the unused `MONGODB_URI` binding also retires two Secret Manager secret versions from the bill.

**Do this: fast-forward `main` to `feature/telemetry-api`, push, delete `feature/telemetry-api`, redeploy from `main`, and write down the deploy command.** The fast-forward is provably clean — `git merge-base main feature/telemetry-api` returns `main` itself, so `feature/telemetry-api` is exactly one commit ahead and zero behind. Everything else in this document is verification and paperwork around those five steps.

## Problem

The server tree that is running in production lives on a side branch, and nothing in the repository or in the Cloud Run configuration records that fact. Revision `test-image-00017-w2x` (created 2026-09-10T18:40:10Z, 100% of traffic) was built from a source zip at `gs://run-sources-solar-tracker-442903-us-central1/services/test-image/1789065589.041328-092d013b245a4d24b00c4f7930c9bde7.zip`, and all six tracked files in that zip are byte-identical to `feature/telemetry-api:server/` — verified by SHA-1 in this session, not assumed. Meanwhile `main:server/routes.js:53-58` still calls `mongoose.connect(process.env.MONGODB_URI)` on every request and writes through `main:server/schemas/data.js`, so `main`'s server cannot store a single telemetry row from the current firmware. PRDs 02, 06 and 07 each need `server/` changes; on today's layout, an edit made on `main` ships nowhere and an edit made on `feature/telemetry-api` ships from a branch no document names.

## Goal

`main` holds the exact server tree that the running Cloud Run revision was built from, and one documented command in the repository redeploys it.

## Non-goals

- No firmware change of any kind. Nothing is compiled, nothing is flashed.
- Does not add, remove or alter any endpoint the device calls. The `/data` 8-column CSV contract and the `raw/<day>/<stamp>.ndjson` object layout are untouched — PRDs 06 and 07 own those.
- Does not build CI/CD. `run.googleapis.com/build-enable-automatic-updates` stays `false`; deploys stay manual and deliberate, which is what Zach's "nothing I can't fix remotely" constraint wants.
- Does not change scaling or fix the cold start. This PRD measures it and writes the number down so 02 and 06 can plan around it; `min-instances` stays 0.
- Does not reconcile the *firmware* branches (`feature/telemetry` vs `test/native-suite`) or prune the other stale remote branches. Same disease, different PRD.
- Does not delete old Cloud Run revisions (`test-image-00001` … `00016`). They cost nothing and they are the only rollback that exists today.
- Does not implement the `.ota` serving path. This PRD only removes the dead scaffolding that would otherwise tempt PRD 02 into reusing it.

## Requirements

1. Fast-forward `main` to `feature/telemetry-api` with `git merge --ff-only`, producing no merge commit and no rewritten history.
2. Push `main` to `origin` and confirm `git rev-parse main` equals `git rev-parse origin/main`.
3. Delete `feature/telemetry-api` locally and on `origin` once `main` contains `ad4bd77`, so there is exactly one branch that can deploy.
4. Add `server/.gcloudignore` to version control — it is in the deployed source zip and in the working tree but tracked on no branch, so one `git clean` loses it.
5. Change `server/routes.js:21` to honour `process.env.PORT`, then redeploy without `--port 443`, so the deployment stops depending on a flag recorded nowhere.
6. Delete the `GET /update/check/:version` and `GET /update/download/update-v:version.bin` routes (`server/routes.js:80-98`) and the `path`/`existsSync` imports they need.
7. Remove the `MONGODB_URI` secret binding from the service, and drop `roles/secretmanager.secretAccessor` from `solar-tracker-server@` once nothing reads a secret.
8. Write `server/DEPLOY.md` recording project, service, region, URL, service account, bucket, the full `gcloud run deploy` command, and the byte-identity check from the Design section.
9. Tag the commit that gets deployed as `deployed-server-<YYYY-MM-DD>` and push the tag, matching the `flashed-prod-*` convention the firmware side already uses.
10. Update `SPEC.md:32` (still "Node 20 / Express / Mongoose"), the §3.8 endpoint table at `SPEC.md:167-174` (still says `/data` "Writes a telemetry document to MongoDB"), FR-32 through FR-35 at `SPEC.md:175-180`, and the "Publishing an OTA update" recipe at `SPEC.md:336-339`.
11. Redeploy from `main` and prove the new source zip is byte-identical to `main:server/` by the same SHA comparison.
12. Confirm one real device sync after the redeploy: a fresh `raw/<today>/*.ndjson` object and a `Saved N rows` log line.
13. Record the measured cold-start latency in `server/DEPLOY.md` so PRDs 02 and 06 size their timeouts against a number rather than a guess.

Every requirement above rests on facts measured directly against the live project and this checkout during drafting. The one load-bearing claim inherited from the research pass, and *not* verified, is flagged in Design → "The chunked-encoding claim is wrong as stated".

## Design

### Branch topology, as it actually is

```
b98bc93  Merge PR #31 — firmware, Nov 2024        (merge base)
   │
   ├── main  +5 commits
   │     d3e968f  initial containerizable api      ← also commits 1,467 node_modules files
   │     a0da46e  mongoose → MongoDB
   │     991e593  merge PR #33
   │     50f962e, 2c8ac78  README
   │        │
   │        └── feature/telemetry-api  +1 commit
   │              ad4bd77  Store telemetry batches in Cloud Storage instead of MongoDB
   │              ★ DEPLOYED — revision test-image-00017-w2x
   │
   └── feature/telemetry  +18 commits, firmware only
         `server/` is git-ignored with the comment "server lives on feature/telemetry-api"
         contains test/native-suite (e3fa4bf, tag flashed-2026-09-09)
         HEAD 0367716; newest flash tag is flashed-prod-2026-09-10 → 71b63aa
```

`git rev-list --left-right --count main...feature/telemetry-api` returns `0  1`. `git rev-list --left-right --count main...feature/telemetry` returns `5  18`.

**Correction to the project's own notes.** `memory/next-session-agenda.md` says of these two branches "both committed, neither pushed", and lists two blockers: "the Cloud Run service account has no binding on the bucket" and "the service has not been redeployed". All three statements are now false. `origin/main`, `origin/feature/telemetry` and `origin/feature/telemetry-api` all match their local refs exactly; the bucket IAM policy grants `roles/storage.objectAdmin` to `solar-tracker-server@solar-tracker-442903.iam.gserviceaccount.com`; and revision 17 was deployed on 2026-09-10 and has been writing NDJSON ever since — `gs://solar-tracker-442903-telemetry/raw/` holds 267 objects. The end-to-end path works. What is missing is only the record of *which branch* it works from.

### Merge direction

| Option | What it means | Verdict |
| --- | --- | --- |
| **A. Fast-forward `main` to `feature/telemetry-api`, delete the side branch** | One `git merge --ff-only`, no conflict possible, `main` becomes the deploy branch, the MongoDB code and 1,467 committed `node_modules` files disappear in the same move (`ad4bd77` added `server/node_modules` to `.gitignore`) | **Recommended.** Cheapest, reversible, and it makes `main` mean what everyone assumes it means |
| B. Keep `feature/telemetry-api` as the server branch of record and document that | Zero git operations, one paragraph of docs | Rejected. Leaves `main` holding code that cannot serve the device, and leaves the next reader one wrong `git checkout` away from a broken deploy. Also loses the `node_modules` cleanup |
| C. Merge `server/` into `feature/telemetry` so firmware and server ship from one branch | A real merge; `feature/telemetry/.gitignore` currently ignores `server/` outright, so that line comes out and the trees have to be reconciled | Rejected *for now*. It is the right long-term shape — the telemetry contract spans both halves and PRD 07 will change both at once — but it is a firmware-branch decision with 18 commits of divergence behind it, and it should not gate PRDs 02, 06 and 07. Do A now; do C when the firmware branch merges to `main` |

### The commands

```bash
cd /Users/zachlegesse/Code/solar-tracker
git checkout main
git merge --ff-only feature/telemetry-api      # main: 2c8ac78 -> ad4bd77
git push origin main
git branch -d feature/telemetry-api
git push origin --delete feature/telemetry-api
```

Then, on `main`, the edits of requirements 4–10, and:

```bash
cd server
gcloud run deploy test-image \
  --source . \
  --project        solar-tracker-442903 \
  --region         us-central1 \
  --max-instances  1 \
  --memory         256Mi \
  --cpu            1 \
  --service-account solar-tracker-server@solar-tracker-442903.iam.gserviceaccount.com \
  --allow-unauthenticated \
  --clear-secrets \
  --set-env-vars   TELEMETRY_BUCKET=solar-tracker-442903-telemetry
```

`--allow-unauthenticated` is not optional: the RA4M1 offloads TLS to the ESP32 over the AT bridge and cannot mint an OAuth token, so `allUsers → roles/run.invoker` (the current binding) has to stay. `TELEMETRY_BUCKET` is redundant — `routes.js:23-24` already defaults to that bucket — but naming it in the command makes the command self-documenting. `--clear-secrets` is what satisfies requirement 7.

### The `--port 443` landmine

`server/routes.js:21` is `const PORT = 443;` and never reads `process.env.PORT`. The running revision therefore has `containerPort: 443` and a startup probe on `tcpSocket: 443`, both set by a `--port 443` flag on a deploy from 2026-09-10 that exists in no script and no document. A redeploy without that flag gets `PORT=8080` from Cloud Run, the app still binds 443, the TCP startup probe on 8080 fails, and the revision never takes traffic. Fix the cause:

```js
const PORT = Number(process.env.PORT) || 443;
```

This is also what lets `npm run serve` work locally without root. The failure mode while making the change is benign — Cloud Run keeps the previous revision serving 100% of traffic until a new one passes its startup probe, so a mistake here costs a failed deploy, not an outage.

### Proving what is deployed

This is the check that turns "we think `main` is what runs" into a fact, and it belongs in `server/DEPLOY.md`:

```bash
ZIP=$(gcloud run services describe test-image --region us-central1 \
        --project solar-tracker-442903 \
        --format='value(metadata.annotations."run.googleapis.com/build-source-location")')
gcloud storage cp "$ZIP" /tmp/deployed.zip && unzip -oq /tmp/deployed.zip -d /tmp/deployed
for f in Dockerfile .dockerignore .gcloudignore package.json package-lock.json routes.js routes.test.js; do
  a=$(git show main:server/$f | shasum | cut -d' ' -f1)
  b=$(shasum /tmp/deployed/$f | cut -d' ' -f1)
  [ "$a" = "$b" ] && echo "SAME  $f" || echo "DIFF  $f"
done
```

Run today against `feature/telemetry-api` it prints `SAME` for six files and cannot check `.gcloudignore`, because that file is in the zip but tracked nowhere. That gap is requirement 4.

### Dead OTA scaffolding, live in production

Two routes that PRD 02 will otherwise be tempted to reuse are still serving, and both are broken by design:

```
$ curl -s .../update/check/9
{"updateAvailable":false}

$ curl -sI .../update/download/update-v9.bin
HTTP/1.1 404 Not Found
```

and in Cloud Logging, at the moment of that second request:

```
Error serving file: ENOENT: no such file or directory, stat '/server/updates/update-v9.bin'
```

`server/updates/` does not exist in the image and never has. `SPEC.md` FR-33 (line 176) and the "Publishing an OTA update" recipe (lines 336-339) both describe this directory as if it worked. They also describe a `.bin`, where the ESP32 `+OTAUPDATE` path needs an LZSS-compressed `.ota` carrying a 20-byte header and the `UNOR4WIFI` magic. Delete the routes, fix the spec, and let PRD 02 start from the GCS design instead of from scaffolding.

Note for PRD 02 while deleting these: the telemetry bucket has `uniform_bucket_level_access: true`, so per-object ACLs are off and a world-readable `.ota` needs a bucket-level `allUsers → roles/storage.objectViewer` binding. `public_access_prevention` on the bucket is `inherited`, not `enforced`, so the organisation policy does permit it — which closes one of the research pass's open blockers. That is also the argument for PRD 02 using a *separate* bucket rather than making the telemetry bucket public.

### The chunked-encoding claim is wrong as stated

The research pass says: "DO NOT serve the `.ota` from the existing Cloud Run service. Cloud Run responds with `Transfer-Encoding: chunked` by default whenever the app does not set an explicit Content-Length" (evidence: `curl -sI --http1.1` against a `run.app` host, plus `Arduino_ESP32_OTA.cpp` L163-176). The second half of that sentence is the load-bearing part, and against *this* service it does not reproduce. Every HTTP/1.1 response measured here carried a `Content-Length` and no `Transfer-Encoding`:

```
GET /            → HTTP/1.1 200, Content-Length: 12
GET /time/est    → HTTP/1.1 200, Content-Length: 39
POST /data       → HTTP/1.1 200, Content-Length: 23
GET /update/...  → HTTP/1.1 404, Content-Length: 28
```

Express sets `Content-Length` on buffered `res.send`/`res.json` bodies and the Google Frontend passes it through. So the *reason* PRD 02 gives for not serving the image from Cloud Run needs restating. The **conclusion still holds** — serve the `.ota` from `storage.googleapis.com`, because that path was verified end to end (HTTP/1.1 200, `Content-Length`, `x-goog-stored-content-encoding: identity`, `num_redirects=0`, and a clean 403 with `Content-Length` for a missing object, which fails the OTA client's status check before any flash is touched) and because a static object needs no running service. But nobody has tested a ~100 KB `res.sendFile` through the Cloud Run frontend, which is the case that actually matters, and the largest body this service can produce today is 39 bytes. Do not write "Cloud Run always chunks" into PRD 02. Write "GCS is verified; Cloud Run is untested at file size and buys nothing."

### Cold start, measured

`maxScale: 1` with no `minScale` annotation, so the service scales to zero between syncs. Over the last 7 days of Cloud Run logs:

| Measurement | Value |
| --- | --- |
| `GET /time/est` requests | 434 |
| p50 latency | 0.0046 s |
| p90 latency | 2.38 s |
| max latency | 5.90 s |
| requests over 2 s | 86 of 434 (19.8%) |
| `Starting new instance. Reason: AUTOSCALING` events | 101 |
| `POST /data` requests | 229 |

A representative pair from 2026-09-23: at 03:48:22 a `/time/est` took **2.305 s** and triggered an instance start; twelve minutes later, at 04:00:02, the same request against the still-warm instance took **0.0064 s**. It is not literally every upload — it is every sync that follows a gap longer than Cloud Run's idle-instance lifetime, which at the current 30-minute fallback cadence is most of them.

Why this matters beyond aesthetics: `MODEM_TIMEOUT` is 10 s per AT command and there is no watchdog in this firmware. A 5.90 s clock-sync response consumes 59% of that single-command budget on a healthy day. Two consumers about to land make it worse — PRD 06 adds a second POST in the same window, and PRD 02 adds a version check. Record the number here; decide `min-instances` in whichever PRD first finds 10 s too close for comfort. (`min-instances=1` on a 1 vCPU / 256 MiB service is the expensive answer — the research pass priced a held-warm 1 vCPU / 512 MiB instance at roughly $45/month after the free tier, so this is a real trade, not a free win.)

### Two silent-drop behaviours PRDs 06 and 07 must know about

Verified by `curl` against the live service during drafting, and neither is a hypothesis:

```
POST /data  content-type: text/csv    body "not,a,valid,row"              → 200 {"saved":0,"skipped":1}
POST /data  content-type: text/plain  body "1758600000,21.5,50,ACTIVE,3,0,1,0" → 200 {"saved":0,"skipped":0}
```

The first is deliberate and documented in the code (`routes.js:41-44`): one truncated row from a power cut must not strand a day of good readings. The second is not deliberate. `express.text({ type: "text/csv" })` (`routes.js:19`) only populates `req.body` for that exact content type; anything else leaves it an object, `raw` becomes `""`, and the handler returns `200 {"saved":0,"skipped":0}` having written nothing. A whole batch vanishes, the device sees 200, and the device deletes the chunk. The firmware sends `"text/csv"` today (`src/SolarTracker/SolarTracker.cpp:295`), so this is latent — but it is exactly the shape of bug PRD 07 could introduce while changing the schema, and it is why `skipped` and `saved` both being 0 on a non-empty body should be a server-side `WARNING`.

### Leftovers on the running revision

| Finding | Source | Action |
| --- | --- | --- |
| `MONGODB_URI` secret still bound as an env var; the deployed code imports no mongoose | `spec.template.spec.containers[0].env` on revision 17; `feature/telemetry-api:server/routes.js` has no mongoose import | `--clear-secrets` (requirement 7). Two enabled secret versions from 2024-12-11 remain in Secret Manager; disable them once the binding is gone |
| Runtime service account holds project-wide `roles/run.developer` | `gcloud projects get-iam-policy` | Out of scope to fix here, but note it: the server can redeploy Cloud Run services. Flag for whoever next touches IAM |
| `Dockerfile` is `COPY . .` then `npm install`, with `node_modules` in `.dockerignore` | `feature/telemetry-api:server/Dockerfile` | Fine as-is on the merged tree. On `main` today there is no `.dockerignore` and 1,467 committed `node_modules` files, so a build from `main` copies them in and then installs over them — one more reason `main` cannot be deployed as it stands |

### The pattern to copy from the firmware side

The firmware half of this project already solved this problem: flashed commits get a tag and the tag gets pushed — `flashed-2026-09-09` → `e3fa4bf`, `flashed-prod-2026-09-10` → `71b63aa`, both on `origin`. The server half has no equivalent, which is why reconstructing revision 17's provenance took a zip download and seven SHA comparisons. Requirement 9 copies the convention.

Worth noticing while copying it: the convention already slipped once. The newest flash tag is `flashed-prod-2026-09-10` → `71b63aa`, but the I2C bus-clear recovery that fired eight times on 2026-09-22 08:37 exists only in `0367716` (`src/SolarTracker/SolarTracker.cpp:48`, the `Bus clear: sda_freed=` line), which carries no tag. So the device is running an untagged commit. A tagging habit is not enough on its own; the tag has to be a step in a written procedure, which is what `server/DEPLOY.md` is for.

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| The redeploy breaks `/time/est`, the clock stops syncing, tracking drifts, and somebody has to drive out → **makes a site visit more likely** | Low | High | Cloud Run holds the previous revision at 100% traffic until a new one passes its startup probe, so a bad deploy is a failed deploy, not an outage; requirement 12 confirms a real device sync before walking away; the `--port` fix removes the one flag whose loss would silently fail |
| The redeploy breaks `/data`, and telemetry is lost | Low | Low | The SD spool is the durable queue — 64 chunks × 4,096 bytes at a measured 33.4 bytes/row is roughly 50 days of buffer (research pass, measured on the card), and a chunk is deleted only after a 200. Hours of server downtime cost nothing |
| A deploy is made from `main` before requirement 5 lands, without `--port 443` | Medium | Low | Do requirements 4–7 as one commit, then deploy once. The failure is a revision that never serves |
| `server/.gcloudignore` is lost to a `git clean`/`git checkout` before requirement 4 lands, and the next deploy uploads `.git` | Medium | Low | Requirement 4 is the first file change to make. The deploy runs from `server/`, so there is no `.git` there to upload — the practical cost is a fatter source zip, not a leak |
| The fast-forward deletes 1,467 tracked files and the diff looks alarming enough to be reverted | Low | Medium | Say so in the push: `ad4bd77` added `server/node_modules` to `.gitignore`, `npm install` restores them, and `package-lock.json` is tracked |
| PRD 02 reads the research pass literally, believes Cloud Run always chunks, and later "discovers" the existing `/update/download` route looks usable | Medium | Medium | The Design section states the correction and the untested case explicitly; requirement 6 deletes the route so there is nothing to rediscover |
| Deleting `origin/feature/telemetry-api` orphans a commit somebody wanted | Very low | Low | `ad4bd77` becomes reachable from `main` by the fast-forward, and the deploy tag from requirement 9 pins it a second time. Do requirement 3 only after requirement 2 verifies the push |

## Acceptance criteria

- [ ] `git rev-parse main` returns `ad4bd77` or a descendant, and `git merge-base --is-ancestor ad4bd77 main` exits 0.
- [ ] `git rev-parse main` equals `git rev-parse origin/main`.
- [ ] `git branch -a --list '*telemetry-api*'` returns nothing.
- [ ] `git ls-files server/ | grep -c node_modules` returns 0 on `main`, and `git ls-files server/.gcloudignore` returns the path.
- [ ] `git show main:server/routes.js | grep -c 'process.env.PORT'` returns 1, and `grep -c 'update/check\|update/download'` returns 0.
- [ ] `cd server && npm test` passes on `main` as a non-root user with no `PORT` set.
- [ ] The byte-identity script in Design prints `SAME` for all seven files against `main`, including `.gcloudignore`.
- [ ] `gcloud run services describe test-image --region us-central1 --format='value(spec.template.spec.containers[0].env)'` shows no `MONGODB_URI`.
- [ ] `gcloud projects get-iam-policy solar-tracker-442903 --flatten='bindings[].members' --filter='bindings.members:solar-tracker-server@*'` no longer lists `roles/secretmanager.secretAccessor`.
- [ ] `curl -sI --http1.1 https://test-image-188091506191.us-central1.run.app/` returns `HTTP/1.1 200` with a `Content-Length`, after the redeploy.
- [ ] `curl -s .../update/check/1` returns a 404, not `{"updateAvailable":false}`.
- [ ] A new object appears under `gs://solar-tracker-442903-telemetry/raw/<today>/` from a real device sync after the redeploy, and Cloud Logging shows the matching `Saved N rows (0 skipped)` line.
- [ ] `git tag --list 'deployed-server-*'` returns a tag that `git ls-remote --tags origin` also lists, pointing at the deployed commit.
- [ ] `server/DEPLOY.md` exists on `main` and contains the deploy command, the byte-identity script, and the measured cold-start latencies; a reader who has never seen the project can redeploy from it alone.
- [ ] `sed -n '25,35p;165,182p;330,340p' SPEC.md | grep -ci 'mongo\|datalogs\|update-v'` returns 0, and §3.8's endpoint table lists exactly the routes present in `main:server/routes.js`.

## Open questions

- **The adversarial verification pass never ran.** Most of this document was measured directly against the live project and this checkout while drafting, so it is stronger than the research pass on its own. The inherited claim that is *not* verified and that matters downstream is the `Transfer-Encoding: chunked` reasoning against Cloud Run — my own measurements contradict it for small bodies and nobody has tested a ~100 KB `res.sendFile`. PRD 02's recommendation survives either way; its stated justification does not.
- **Should `main` also absorb `feature/telemetry`?** Option C above. The telemetry contract spans firmware and server, and PRD 07 changes both at once, so shipping them from one branch is the right end state. It needs a decision on whether `main` becomes the single trunk or stays server-only, and 18 commits of firmware divergence say that is its own piece of work.
- **Who owns `min-instances`?** Leaving it at 0 keeps the bill at effectively zero and costs up to 5.90 s on the first request of each sync, against a 10 s `MODEM_TIMEOUT` and no watchdog. Setting it to 1 is roughly $45/month after the free tier. This PRD records the number and declines to decide; PRD 02 or 06 should decide it when a second request lands in the same window.
- **Is a deploy from `main` supposed to be possible from Zach's home PC?** The tracker is plugged into the home PC (ssh host `home`), which has its own checkout and its own serial logging. If that checkout is where deploys happen, `server/DEPLOY.md` needs a line about which machine is authoritative and `gcloud config` there needs checking. Not verifiable from the Mac.
- **Disable or destroy the `MONGODB_URI` secret versions?** Two enabled versions from 2024-12-11 remain. Disabling is reversible and gives a month to notice a mistake; destroying is final. Zach's non-destructive constraint argues for disable-then-wait, but nothing reads them today.
