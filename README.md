## TLDR
This repository contains C++ firmware and a JavaScript API developed for a custom solar tracker

### Goal
- Increase solar panel output by 25% by enabling the client's solar panels to track the position of the sun in harsh off-grid conditions

![Completed Solar Tracker](https://github.com/user-attachments/assets/931917b8-5275-4b0c-b8bf-8001cea58635)

## Project State
### Completed
- Robust C++ firmware to enable continuous automated deployment in a remote environment
- Custom over-the-air updating firmware to enable remote technical support
- An Express-based JavaScript API to handle data ingestion, update serving, and time serving


## Portal
A small read-only API that serves the tracker's telemetry to a few signed-in people, behind the
tracker page on zachlegesse.ca. Sign-in is by phone number on an allowlist, with each request
approved by hand; no passwords and no SMS. The service is in [`portal/`](portal/README.md), with
admin scripts in `tools/portal/`. The page itself lives in the
[zachale/portfolio](https://github.com/zachale/portfolio) repo under `src/app/tracker`. See
[portal/README.md](portal/README.md) for the architecture, endpoints, setup and deployment.
