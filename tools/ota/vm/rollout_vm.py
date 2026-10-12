# /// script
# requires-python = ">=3.11"
# dependencies = ["google-auth>=2.30", "requests>=2.32"]
# ///
"""Staged OTA rollout for the field tracker, run hourly by a systemd user timer on the VM.

Each run looks at the cloud only: the manifest the tracker reads, the clock syncs the
tracker makes (from v12.1 on they name its firmware as /time/est?fw=<version>), and the
telemetry batches it uploads. When the version the manifest names has installed, been
seen running for three hours and passed its own check, the run points the manifest at
the next version. Anything unexpected stops the rollout and emails Zach; nothing is
published again until he runs `rollout_vm.py resume`.

The images are already in the bucket. This only ever writes manifest.txt, and the
service account it runs as can write nothing else. When the last version passes, it
emails a summary, deletes its own service account and turns its timer off.

    uv run rollout_vm.py check [--dry-run]   one check, as the timer runs it
    uv run rollout_vm.py status              print the saved state
    uv run rollout_vm.py resume              clear a stop after Zach has looked at it
    uv run rollout_vm.py test-email          send one email to check the mail setup

A dry run reads everything and decides, but writes no state, publishes nothing, sends
nothing and deletes nothing.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import re
import smtplib
import subprocess
import sys
import time
from datetime import datetime, timedelta, timezone
from email.message import EmailMessage
from pathlib import Path
from urllib.parse import quote
from zoneinfo import ZoneInfo

import google.auth.transport.requests
import requests
from google.oauth2 import service_account

CONFIG_DIR = Path.home() / ".config" / "ota-rollout"
STATE_DIR = Path.home() / ".local" / "state" / "ota-rollout"
STATE_FILE = STATE_DIR / "state.json"
LOG_FILE = STATE_DIR / "rollout.log"
LOCK_FILE = STATE_DIR / "lock"
PASSWORD_FILE = CONFIG_DIR / "gmail-app-password"
SITE = ZoneInfo("America/New_York")
SCOPES = ["https://www.googleapis.com/auth/cloud-platform"]

SOAK = timedelta(hours=3)            # of rows actually received from the new version
INSTALL_GRACE = timedelta(hours=2)   # of site daytime, from publish to the first tagged sync
BATCH_GAP = timedelta(minutes=45)    # longest daytime wait for a telemetry batch
SAFE_GAP = timedelta(hours=3)        # the same, while SAFE holds uploads back by design
STAGE_DEADLINE = timedelta(hours=40)  # from install to passing its own check
MAX_FAILURES = 3                     # runs in a row that could not complete
# The tracker syncs through the night since v12.3 ended the 17:00 move that used to hang
# until morning (seen 2026-10-12: syncs every 15 min after 17:00), so it is reachable, and
# checked, around the clock.
PUBLISH_HOURS = range(0, 14)         # site hours a version may go out: from midnight, so its
                                     # 3 h soak ends while someone could still look
DAY_START, DAY_END = 0, 24           # site hours the tracker is expected to upload
MORNING_CATCHUP = (0, 0)             # no overnight catch-up to wait for any more
STATUSES = {"ACTIVE", "NIGHT", "SAFE", "AWAY"}

DRY_RUN = False


# ---- small helpers ---------------------------------------------------------------------

def log(msg: str) -> None:
    line = f"{datetime.now(timezone.utc):%Y-%m-%dT%H:%M:%SZ} {'(dry run) ' if DRY_RUN else ''}{msg}"
    print(line)
    if DRY_RUN:
        return
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    with LOG_FILE.open("a") as f:
        f.write(line + "\n")


def parse_utc(s: str) -> datetime:
    return datetime.fromisoformat(s.replace("Z", "+00:00")).astimezone(timezone.utc)


def iso(t: datetime) -> str:
    return t.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def site(t: datetime) -> datetime:
    return t.astimezone(SITE)


def version_key(v: str) -> tuple[int, ...]:
    # "v12" (an untagged sync) < "v12.1" < ... ; anything unparsable sorts first.
    m = re.fullmatch(r"v(\d+(?:\.\d+)*)", v)
    return tuple(int(x) for x in m.group(1).split(".")) if m else (-1,)


class Stop(Exception):
    """Something needs Zach's judgement: stop the rollout and tell him."""


# ---- config, state, credentials --------------------------------------------------------

def load_config() -> dict:
    cfg = json.loads((CONFIG_DIR / "config.json").read_text())
    cfg["token"] = (CONFIG_DIR / "ota-token").read_text().strip()
    return cfg


def load_state() -> dict:
    return json.loads(STATE_FILE.read_text())


def save_state(state: dict) -> None:
    if DRY_RUN:
        return
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2) + "\n")
    tmp.replace(STATE_FILE)


def session() -> tuple[requests.Session, str]:
    creds = service_account.Credentials.from_service_account_file(
        str(CONFIG_DIR / "sa-key.json"), scopes=SCOPES)
    creds.refresh(google.auth.transport.requests.Request())
    s = requests.Session()
    s.headers["Authorization"] = f"Bearer {creds.token}"
    return s, creds.service_account_email


def mask(cfg: dict | None, text: str) -> str:
    return text.replace(cfg["token"], "<token>") if cfg and cfg.get("token") else text


# ---- what the cloud says ---------------------------------------------------------------

def manifest_object_url(cfg: dict) -> str:
    return (f"https://storage.googleapis.com/storage/v1/b/{cfg['ota_bucket']}/o/"
            f"{quote(cfg['token'] + '/manifest.txt', safe='')}")


def read_manifest(s: requests.Session, cfg: dict) -> tuple[str, str]:
    """The manifest's line and its generation, read through the API so a write can be
    made conditional on nobody having changed it since."""
    meta = s.get(manifest_object_url(cfg), timeout=30)
    meta.raise_for_status()
    gen = meta.json()["generation"]
    body = s.get(manifest_object_url(cfg), params={"alt": "media", "generation": gen}, timeout=30)
    body.raise_for_status()
    return body.text.strip(), gen


def public_manifest(cfg: dict) -> str:
    url = f"https://storage.googleapis.com/{cfg['ota_bucket']}/{cfg['token']}/manifest.txt"
    r = requests.get(url, timeout=20, headers={"Cache-Control": "no-cache"})
    r.raise_for_status()
    return r.text.strip()


def clock_syncs(s: requests.Session, cfg: dict, since: datetime) -> list[tuple[datetime, str, dict]]:
    """Every /time/est request since `since`, oldest first, with the firmware it named and
    the rest of its query (from v12.6: cal = calibrations since boot, pos = 1 when the boot
    took its position over from the last firmware)."""
    flt = (f'resource.type="cloud_run_revision" AND resource.labels.service_name="{cfg["service"]}" '
           f'AND httpRequest.requestUrl:"/time/est" AND timestamp>="{iso(since)}"')
    body = {"resourceNames": [f"projects/{cfg['project']}"], "filter": flt,
            "orderBy": "timestamp asc", "pageSize": 1000}
    out = []
    while True:
        r = s.post("https://logging.googleapis.com/v2/entries:list", json=body, timeout=60)
        r.raise_for_status()
        data = r.json()
        for e in data.get("entries", []):
            url = e.get("httpRequest", {}).get("requestUrl", "")
            query = dict(re.findall(r"[?&]([a-z]+)=([^&\s]+)", url))
            out.append((parse_utc(e["timestamp"]), query.pop("fw", "v12"), query))
        if not data.get("nextPageToken"):
            return out
        body["pageToken"] = data["nextPageToken"]


def batches(s: requests.Session, cfg: dict, since: datetime) -> list[tuple[datetime, list[dict]]]:
    """Telemetry batches that landed since `since`, oldest first, each with its rows."""
    out = []
    day = since.date()
    url = f"https://storage.googleapis.com/storage/v1/b/{cfg['telemetry_bucket']}/o"
    while day <= datetime.now(timezone.utc).date():
        params = {"prefix": f"raw/{day}/", "fields": "items(name),nextPageToken"}
        while True:
            r = s.get(url, params=params, timeout=60)
            r.raise_for_status()
            data = r.json()
            for item in data.get("items", []):
                name = item["name"]
                m = re.search(r"(\d{4}-\d\d-\d\dT\d\d-\d\d-\d\d)", name)
                if not m or not name.endswith(".ndjson"):
                    continue
                landed = datetime.strptime(m.group(1), "%Y-%m-%dT%H-%M-%S").replace(tzinfo=timezone.utc)
                if landed < since:
                    continue
                body = s.get(f"{url}/{quote(name, safe='')}", params={"alt": "media"}, timeout=60)
                body.raise_for_status()
                rows = [json.loads(line) for line in body.text.splitlines() if line.strip()]
                out.append((landed, rows))
            if not data.get("nextPageToken"):
                break
            params["pageToken"] = data["nextPageToken"]
        day += timedelta(days=1)
    return sorted(out, key=lambda b: b[0])


def row_site_time(row: dict) -> datetime:
    # Rows carry the tracker's own clock, which is site time with a misleading "Z".
    return datetime.strptime(row["time"][:19], "%Y-%m-%dT%H:%M:%S").replace(tzinfo=SITE)


# ---- acting --------------------------------------------------------------------------

def email(cfg: dict, subject: str, body: str) -> None:
    """Raises if the email cannot be sent; callers decide what that means."""
    if DRY_RUN:
        log(f"would email: {subject}")
        return
    msg = EmailMessage()
    msg["From"] = cfg["email_from"]
    msg["To"] = cfg["email_to"]
    msg["Subject"] = f"Solar tracker rollout: {subject}"
    msg.set_content(mask(cfg, body))
    with smtplib.SMTP_SSL("smtp.gmail.com", 465, timeout=30) as smtp:
        smtp.login(cfg["email_from"], PASSWORD_FILE.read_text().strip().replace(" ", ""))
        smtp.send_message(msg)
    log(f"emailed: {subject}")


def publish(s: requests.Session, cfg: dict, state: dict, version: str, generation: str,
            now: datetime) -> None:
    stage = next(st for st in cfg["stages"] if st["version"] == version)
    line = stage["manifest"]
    base = f"https://storage.googleapis.com/{cfg['ota_bucket']}/{cfg['token']}"
    head = requests.head(f"{base}/{version}.ota", timeout=20)
    if head.status_code != 200 or int(head.headers.get("Content-Length", -1)) != stage["ota_bytes"]:
        raise Stop(f"{version}.ota is not in the bucket as built (HTTP {head.status_code}, "
                   f"{head.headers.get('Content-Length')} bytes, expected {stage['ota_bytes']}); "
                   f"manifest left on {state['current']['version']}")
    if DRY_RUN:
        log(f"would point the manifest at '{line}'")
        return
    # Written down first, so a write whose answer is lost is recognised next run.
    state["publishing"] = {"version": version, "line": line, "at": iso(now)}
    save_state(state)
    boundary = "ota-rollout-boundary"
    meta = json.dumps({"name": f"{cfg['token']}/manifest.txt",
                       "cacheControl": "no-store", "contentType": "text/plain"})
    payload = (f"--{boundary}\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{meta}\r\n"
               f"--{boundary}\r\nContent-Type: text/plain\r\n\r\n{line}\n\r\n--{boundary}--\r\n")
    r = s.post(f"https://storage.googleapis.com/upload/storage/v1/b/{cfg['ota_bucket']}/o",
               params={"uploadType": "multipart", "ifGenerationMatch": generation},
               data=payload.encode(),
               headers={"Content-Type": f"multipart/related; boundary={boundary}"}, timeout=60)
    if r.status_code == 412:
        state.pop("publishing", None)
        save_state(state)
        raise Stop(f"the manifest changed while {version} was being published; left it alone")
    if r.status_code != 200:
        raise Stop(f"writing the manifest for {version} failed: HTTP {r.status_code} "
                   f"{mask(cfg, r.text)[:300]}")
    adopt(state, version, now)
    for _ in range(10):
        try:
            if public_manifest(cfg) == line:
                break
        except requests.RequestException:
            pass
        time.sleep(3)
    else:
        raise Stop(f"wrote the manifest for {version} but the public URL does not serve '{line}'")
    log(f"published {version}: manifest now '{line}'")


def adopt(state: dict, version: str, at: datetime) -> None:
    state.pop("publishing", None)
    state["history"].append({"version": version, "published_at": iso(at)})
    state["current"] = {"version": version, "published_at": iso(at)}
    save_state(state)


def finish(s: requests.Session, cfg: dict, state: dict, sa_email: str) -> None:
    if not state.get("done_emailed"):
        lines = [f"- {h['version']}: published {h.get('published_at', '?')}, installed "
                 f"{h.get('installed_at', '?')}. {h.get('evidence', '')}" for h in state["history"]]
        body = ("The staged rollout is done. Every version installed, ran for 3 h and passed its "
                "check.\n\n" + "\n".join(lines) +
                f"\n\nv12.3 night test: {state.get('night_test', 'not recorded')}. "
            f"Overnight silence: {state.get('silence_note', 'not recorded')}\n\n"
                "The homing fix in v12.13 only shows at the weekly recalibration, about 7 days after "
                "the last boot.\n\nThe VM's service account deletes itself after this email and the "
                "timer turns off. Leftover IAM bindings that name the deleted account grant nothing; "
                "tools/ota/vm/cleanup.sh on the Mac removes them, and the custom self-delete role.")
        email(cfg, f"done, {state['current']['version']} passed", body)  # raises if unsent: retried next run
        state["done_emailed"] = iso(datetime.now(timezone.utc))
        save_state(state)
    if DRY_RUN:
        log("would delete the service account and stop the timer")
        return
    r = s.delete(f"https://iam.googleapis.com/v1/projects/-/serviceAccounts/{sa_email}", timeout=30)
    if r.status_code not in (200, 404):
        raise Stop(f"rollout done, but deleting the service account failed: HTTP {r.status_code} "
                   f"{r.text[:200]}. Its key is still on the VM; run tools/ota/vm/cleanup.sh")
    log(f"deleted service account {sa_email}")
    (CONFIG_DIR / "sa-key.json").unlink(missing_ok=True)
    state["finished"] = iso(datetime.now(timezone.utc))
    save_state(state)
    subprocess.run(["systemctl", "--user", "disable", "--now", "ota-rollout.timer"], check=False)
    log("rollout finished; timer disabled")


# ---- one check -----------------------------------------------------------------------

def check(cfg: dict, state: dict) -> None:
    now = datetime.now(timezone.utc)
    site_now = site(now)
    order = [st["version"] for st in cfg["stages"]]
    s, sa_email = session()

    if state.get("done_emailed"):
        finish(s, cfg, state, sa_email)     # an earlier run sent the email, then failed
        return

    manifest, generation = read_manifest(s, cfg)
    named = manifest.split()[0] if manifest else ""
    pending = state.get("publishing")
    if pending and manifest == pending["line"]:
        adopt(state, pending["version"], parse_utc(pending["at"]))
        log(f"the manifest names {pending['version']}: the last run's write went through")
    elif pending:
        state.pop("publishing")
        save_state(state)
    cur = state["current"]
    version, published_at = cur["version"], parse_utc(cur["published_at"])
    if named != version:
        raise Stop(f"the manifest names '{named}' but the rollout expects {version}; "
                   "someone else changed it")

    syncs = clock_syncs(s, cfg, published_at - timedelta(hours=1))
    tagged = [t for t, fw, _ in syncs if fw == version and t >= published_at]
    if not tagged:
        waited = daytime_between(published_at, now)
        if waited > INSTALL_GRACE:
            raise Stop(f"{version} was published {iso(published_at)} and after "
                       f"{waited} of site daytime the tracker still has not synced as it")
        log(f"{version}: published {iso(published_at)}, not installed yet")
        return
    installed_at = tagged[0]
    if cur.get("installed_at") != iso(installed_at):
        cur["installed_at"] = iso(installed_at)
        save_state(state)

    older = [(t, fw) for t, fw, _ in syncs if t > installed_at and version_key(fw) < version_key(version)]
    if older:
        t, fw = older[0]
        raise Stop(f"rollback: the tracker synced as {fw} at {iso(t)}, after installing "
                   f"{version} at {iso(installed_at)}")

    got = batches(s, cfg, installed_at - timedelta(hours=1))
    installed_site = site(installed_at)
    rows = sorted((r for _, rs in got for r in rs if row_site_time(r) >= installed_site),
                  key=lambda r: r["time"])

    # Fresh data is required for anything to pass: at 06:13 the night's catch-up may not
    # have landed yet, and a pass must never rest on what was seen the evening before.
    fresh = bool(got) and now - got[-1][0] <= BATCH_GAP
    in_day = DAY_START <= site_now.hour < DAY_END
    past_catchup = (site_now.hour, site_now.minute) >= MORNING_CATCHUP
    if in_day and past_catchup and not fresh:
        last = iso(got[-1][0]) if got else "none in the window"
        held_by_safe = bool(rows) and rows[-1]["status"] == "SAFE"
        if not (held_by_safe and got and now - got[-1][0] <= SAFE_GAP):
            raise Stop(f"no telemetry batch for over {SAFE_GAP if held_by_safe else BATCH_GAP} "
                       f"in site daytime (last: {last})")
        log(f"no batch since {last}, but the tracker was in SAFE, which holds uploads back")

    bad = [r for r in rows if r.get("status") not in STATUSES]
    if bad:
        raise Stop(f"unknown status in a row since {version} installed: {bad[0]}")
    # From v12.6 an away boot without a handed-over position holds still and reports -1
    # until away ends: expected, not stuck.
    away_unknown_ok = version == "v12.6" and all(
        r["status"] == "AWAY" for r in rows[-3:])
    if len(rows) >= 3 and all(r["extended_percent"] == -1 for r in rows[-3:]) and not away_unknown_ok:
        raise Stop(f"panel position stuck at -1 (uncalibrated) in the last 3 rows: {rows[-1]['time']}")
    if version_key(version) >= version_key(cfg.get("temperature_fix", "v12.8")):
        hot = [r for r in rows if r["temperature"] >= 128]
        if hot:
            raise Stop(f"temperature {hot[0]['temperature']} at {hot[0]['time']} with {version} running")

    # Soak counts only what has been seen: the newest row from the new firmware.
    seen = (row_site_time(rows[-1]) - installed_site) if rows else timedelta()
    since_install = [(t, q) for t, fw, q in syncs if fw == version and t >= installed_at]
    passed, evidence = stage_check(version, rows, got, state, since_install, installed_site)
    log(f"{version}: installed {iso(installed_at)}, seen running {seen}, {len(rows)} rows since, "
        f"check {'passed' if passed else 'not yet'}: {evidence}")
    if not passed:
        if now - installed_at > STAGE_DEADLINE:
            raise Stop(f"{version} installed {iso(installed_at)} and has not passed its check in "
                       f"{STAGE_DEADLINE}: {evidence}")
        return
    if seen < SOAK or not fresh:
        return

    for h in state["history"]:
        if h["version"] == version:
            h.update({"installed_at": iso(installed_at), "evidence": evidence})
    save_state(state)

    nxt = order.index(version) + 1
    if nxt == len(order):
        finish(s, cfg, state, sa_email)
        return
    if site_now.hour not in PUBLISH_HOURS:
        log(f"{version} passed; {order[nxt]} waits for {PUBLISH_HOURS.start}:00–"
            f"{PUBLISH_HOURS.stop}:00 site time")
        return
    publish(s, cfg, state, order[nxt], generation, now)
    try:
        email(cfg, f"{version} passed, {order[nxt]} published",
              f"{version} passed: {evidence}\n\n{order[nxt]} is now in the manifest "
              f"({iso(now)}). The tracker picks it up at its next 5-minute check.")
    except Exception as e:  # the publish stands; the stop path will report mail trouble
        log(f"progress email not sent: {e!r}")


def stage_check(version: str, rows: list[dict], got, state: dict, syncs, installed_site):
    """(passed, evidence) for the version's own change."""
    if version == "v12.3":
        # The night test needs the tracker in ACTIVE at 17:00; it is in away for the
        # foreseeable future, so it is deferred (Zach, 2026-10-12) and every later version
        # carries the change. Here: 3 h of normal rows.
        state["night_test"] = "deferred while the tracker is in away"
        if len(rows) >= 36:
            return True, f"{len(rows)} normal rows since install; night test deferred while away"
        return False, f"{len(rows)} rows since install, want 36"

    if version == "v12.6":
        return away_calibration_check(rows, got, syncs, installed_site)

    if version == "v12.10":
        return boot_calibration_check(rows, syncs)

    if version in ("v12.11", "v12.12", "v12.13"):
        note = handoff_check(version, rows, got, syncs, installed_site)
        if version == "v12.12":
            temps = [r["temperature"] for r in rows]
            if temps and not all(-41 <= t <= 70 for t in temps):
                raise Stop(f"v12.12: a temperature outside -41..70 °C: {[t for t in temps if not -41 <= t <= 70][:3]}")
            note += f"; {len(temps)} readings from {min(temps, default='-')} to {max(temps, default='-')} °C"
        if len(rows) >= 36:
            return True, f"{len(rows)} normal rows since install; {note}"
        return False, f"{len(rows)} rows since install, want 36; {note}"

    if version == "v12.2":
        frac = sorted({r["wind_peak"] for r in rows
                       if r["button_panel"]["wind_sensor"] and r["wind_peak"] != int(r["wind_peak"])})
        if frac:
            return True, f"wind_peak with tenths, e.g. {frac[:5]}"
        return False, "no wind_peak with a fraction yet"

    if version == "v12.3":
        return night_check(rows, got, state)

    if version in ("v12.4", "v12.8"):
        temps = [r["temperature"] for r in rows]
        if temps and all(-41 <= t <= 70 for t in temps):
            return True, f"{len(temps)} readings from {min(temps)} to {max(temps)} °C, none at 128 or above"
        return False, "no readings yet" if not temps else "a reading outside -41..70 °C"

    if version in ("v12.5", "v12.9"):
        if len(rows) >= 30:
            return True, f"{len(rows)} normal rows since install"
        return False, f"{len(rows)} rows since install, want 30"

    raise Stop(f"no check defined for {version}")


def boot_calibration_check(rows: list[dict], syncs):
    """v12.10: the update from v12.6 has no position to hand over, so the boot calibrates
    once, even in away, and the panel then reports the away position instead of -1."""
    if not syncs:
        return False, "no tagged sync yet"
    cals = [int(q.get("cal", 0)) for _, q in syncs]
    if max(cals) > 1:
        raise Stop(f"v12.10 calibrated {max(cals)} times since boot; the boot calibration should be the only one")
    known = [r for r in rows if r["extended_percent"] != -1]
    if cals[-1] == 1 and rows and rows[-1]["extended_percent"] == -1 and len(rows) >= 3:
        raise Stop("v12.10 calibrated but the panel still reports -1")
    if not known:
        return False, f"cal={cals[-1]}, no row with a known position yet"
    where = known[-1]["extended_percent"]
    if len(rows) < 36:
        return False, f"{len(rows)} rows since install, want 36; calibrated once, now at {where}%"
    return True, f"calibrated once at boot (cal=1), then held at {where}% ({known[-1]['status']})"


def handoff_check(version: str, rows: list[dict], got, syncs, installed_site) -> str:
    """From v12.11: the update from a calibrated version hands the position over, so the
    boot neither calibrates nor moves."""
    if not syncs:
        return "no tagged sync yet"
    first = syncs[0][1]
    if first.get("pos") != "1":
        raise Stop(f"{version} booted without the position handed over (first sync {first})")
    cals = [int(q.get("cal", 0)) for _, q in syncs]
    if any(r["status"] == "AWAY" for r in rows) and any(cals):
        raise Stop(f"{version} calibrated while away (cal={max(cals)})")
    before = [r for _, rs in got for r in rs if row_site_time(r) < installed_site]
    after = [r for r in rows if r["status"] == before[-1]["status"]] if before else []
    if before and before[-1]["status"] in ("AWAY", "NIGHT") and after:
        held = before[-1]["extended_percent"]
        moved = [r for r in after if abs(r["extended_percent"] - held) > 1]
        if moved:
            raise Stop(f"{version}: position {moved[0]['extended_percent']}% at {moved[0]['time']}, "
                       f"was {held}% before the update")
    return f"position handed over (pos=1), no calibration (cal={max(cals)})"


def away_calibration_check(rows: list[dict], got, syncs, installed_site):
    """v12.6: an OTA restart in away takes the position over and nothing calibrates."""
    if not syncs:
        return False, "no tagged sync yet"
    first = syncs[0][1]
    away = [r for r in rows if r["status"] == "AWAY"]
    cals = [int(q.get("cal", 0)) for _, q in syncs]
    if away and any(cals):
        raise Stop(f"v12.6 calibrated while away: cal={max(cals)} in its syncs")
    if first.get("pos") != "1":
        # The update from v12.3 cannot hand the position over (v12.3 never writes it), so
        # in away v12.6 holds still with the position unknown until away ends.
        if away and all(r["extended_percent"] == -1 for r in away):
            if len(rows) < 36:
                return False, f"{len(rows)} rows since install, want 36; holding still, cal=0 so far"
            return True, ("held still in away with the position unknown (v12.3 could not hand it "
                          "over) and never calibrated (cal=0); it calibrates once away ends")
        raise Stop(f"v12.6 booted without a handed-over position (first sync {first}) and did not "
                   "hold still with it unknown")
    before = [r for _, rs in got for r in rs
              if row_site_time(r) < installed_site and r["status"] == "AWAY"]
    if away and before:
        held = before[-1]["extended_percent"]
        moved = [r for r in away if abs(r["extended_percent"] - held) > 1]
        if moved:
            raise Stop(f"v12.6: away position {moved[0]['extended_percent']}% at {moved[0]['time']}, "
                       f"was {held}% before the update")
    if len(rows) < 36:
        return False, f"{len(rows)} rows since install, want 36; position handed over, cal=0 so far"
    where = (f"held at {away[-1]['extended_percent']}% in away" if away
             else "away ended; calibrations since boot " + str(max(cals)))
    return True, f"position handed over at boot (pos=1), no calibration (cal=0), {where}"


def night_check(rows: list[dict], got, state: dict):
    """v12.3: through a whole night, the panel holds the last scheduled position."""
    # Each night: the ACTIVE row just before it, the NIGHT rows up to the first SAFE or
    # AWAY (a SAFE exit at night moves the panel for reasons of its own), and an ACTIVE row
    # after it, which shows the whole night's rows have arrived.
    i = 0
    while i < len(rows):
        if rows[i]["status"] != "NIGHT" or i == 0 or rows[i - 1]["status"] != "ACTIVE":
            i += 1
            continue
        last_pos = rows[i - 1]["extended_percent"]
        j = i
        while j < len(rows) and rows[j]["status"] == "NIGHT":
            j += 1
        night = rows[i:j]
        complete = j < len(rows) and rows[j]["status"] == "ACTIVE"
        interrupted = j < len(rows) and rows[j]["status"] in ("SAFE", "AWAY")
        at_full = [r for r in night if r["extended_percent"] == 100 and last_pos < 99]
        if at_full:
            raise Stop(f"v12.3 failed its check: NIGHT row at 100% at {at_full[0]['time']} after "
                       f"the last scheduled position {last_pos}%; the evening drive to full "
                       "extension is still happening")
        off = [r for r in night if abs(r["extended_percent"] - last_pos) > 1]
        if off:
            raise Stop(f"v12.3: NIGHT position {off[0]['extended_percent']}% at {off[0]['time']} "
                       f"is not the last scheduled position {last_pos}%")
        if complete and len(night) >= 12:
            night_uploads = [landed for landed, _ in got
                             if site(landed).hour >= 18 or site(landed).hour < 5]
            state["silence_note"] = (
                f"gone: {len(night_uploads)} batches landed between 18:00 and 05:00 site time"
                if night_uploads else "unchanged: nothing landed between 18:00 and 05:00 site time")
            return True, (f"{len(night)} NIGHT rows held at {last_pos}% (the last scheduled "
                          f"position), none at 100%; overnight silence {state['silence_note']}")
        if interrupted:
            i = j          # a SAFE or AWAY spell cut the night short; judge the next one
            continue
        return False, f"{len(night)} NIGHT rows so far; waiting for the whole night"
    return False, "no night with an ACTIVE row before it since install yet"


def daytime_between(a: datetime, b: datetime) -> timedelta:
    """How much of [a, b] fell in site daytime, when the tracker can be reached."""
    total, t = timedelta(), a
    step = timedelta(minutes=5)
    while t < b:
        if DAY_START <= site(t).hour < DAY_END:
            total += min(step, b - t)
        t += step
    return total


def report_stop(cfg: dict, state: dict) -> None:
    """Email a stop until the email actually goes out."""
    if not state.get("stopped") or state.get("stop_emailed"):
        return
    email(cfg, "stopped, needs you",
          f"{state['stopped']}\n\nNothing more is published until you look. On the VM:\n"
          f"  cd ~/ota-rollout && ~/.local/bin/uv run rollout_vm.py status\n"
          f"  ~/.local/bin/uv run rollout_vm.py resume    (once it is fine to go on)")
    state["stop_emailed"] = iso(datetime.now(timezone.utc))
    save_state(state)


def main() -> int:
    global DRY_RUN
    ap = argparse.ArgumentParser(description="Staged OTA rollout check")
    ap.add_argument("command", choices=["check", "status", "resume", "test-email"])
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    DRY_RUN = args.dry_run
    STATE_DIR.mkdir(parents=True, exist_ok=True)

    lock = LOCK_FILE.open("w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("another check is running")
        return 0

    state = load_state()
    if args.command == "status":
        print(json.dumps(state, indent=2))
        return 0
    if args.command == "resume":
        for k in ("stopped", "stop_emailed", "failures"):
            state.pop(k, None)
        save_state(state)
        log("resumed by hand")
        return 0
    cfg = load_config()
    if args.command == "test-email":
        email(cfg, "test", "The VM can send rollout emails.")
        return 0

    # Without mail, a stop would go unreported: refuse to run at all.
    if not DRY_RUN and not (PASSWORD_FILE.exists() and PASSWORD_FILE.read_text().strip()):
        log(f"refusing to run: no {PASSWORD_FILE}, so a stop could not be reported")
        return 1
    if state.get("finished"):
        log("nothing to do: finished")
        return 0
    if state.get("stopped"):
        try:
            report_stop(cfg, state)
        except Exception as e:
            log(f"stop email still not sent: {e!r}")
            return 1
        log(f"nothing to do: stopped: {state['stopped']}")
        return 0

    try:
        check(cfg, state)
        if state.pop("failures", None):
            save_state(state)
    except Stop as e:
        stop(cfg, state, mask(cfg, str(e)))
    except Exception as e:  # network, API or bad data: retry next hour, stop if it persists
        state["failures"] = state.get("failures", 0) + 1
        save_state(state)
        log(f"check failed ({state['failures']} in a row): {mask(cfg, repr(e))[:500]}")
        if state["failures"] >= MAX_FAILURES:
            stop(cfg, state, f"{state['failures']} checks in a row could not complete; the last: "
                             f"{mask(cfg, repr(e))[:500]}")
        return 1
    return 0


def stop(cfg: dict, state: dict, reason: str) -> None:
    log(f"STOPPED: {reason}")
    if DRY_RUN:
        return
    state["stopped"] = reason
    state.pop("stop_emailed", None)
    save_state(state)
    try:
        report_stop(cfg, state)
    except Exception as e:
        log(f"stop email not sent, will retry every run: {e!r}")


if __name__ == "__main__":
    sys.exit(main())
