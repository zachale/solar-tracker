import { Logging } from "@google-cloud/logging";
import { Storage } from "@google-cloud/storage";

import { createApp } from "./app.js";
import { createGcsStore } from "./stores.js";
import { createLoggingSyncLog } from "./syncs.js";

// The cookie signing key. Without it anyone could mint a session, and a key made
// up at start-up would sign everyone out whenever Cloud Run started a new
// instance, so there is no default.
const sessionSecret = process.env.SESSION_SECRET;
if (!sessionSecret) {
  console.error("SESSION_SECRET is not set; refusing to start.");
  process.exit(1);
}

// Credentials come from the Cloud Run service account.
const storage = new Storage();
// The request log is read in the project named by GOOGLE_CLOUD_PROJECT, or else the
// one the library detects: on Cloud Run, the service's own.
const logging = new Logging({ projectId: process.env.GOOGLE_CLOUD_PROJECT });
const app = createApp({
  telemetryStore: createGcsStore(storage.bucket(process.env.TELEMETRY_BUCKET || "solar-tracker-442903-telemetry")),
  portalStore: createGcsStore(storage.bucket(process.env.PORTAL_BUCKET || "solar-tracker-442903-portal")),
  syncLog: createLoggingSyncLog(logging, { service: process.env.TRACKER_SERVICE || "test-image" }),
  sessionSecret,
  siteTz: process.env.SITE_TZ || "America/Toronto",
});

const port = Number(process.env.PORT) || 8080;
app.listen(port, () => {
  console.log(`Tracker portal API listening on port ${port}`);
});
