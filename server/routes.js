import express from "express";
import path from "path";
import { networkInterfaces } from "os";
import { existsSync } from "fs";

import { fileURLToPath } from "url";
import { dirname } from "path";
import { Storage } from "@google-cloud/storage";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const app = express();

app.use(express.json());
// The tracker uploads its spool as CSV rather than JSON. At roughly 36 bytes per
// row against 144 rows a day, CSV is small enough to send in one request from a
// board with 32 KB of RAM, where the equivalent JSON would not be.
app.use(express.text({ type: "text/csv", limit: "1mb" }));

const PORT = 443;

const TELEMETRY_BUCKET =
  process.env.TELEMETRY_BUCKET || "solar-tracker-442903-telemetry";
const storage = new Storage();

// Field order written by Telemetry::formatRow() in the firmware.
const TELEMETRY_FIELDS = [
  "time",
  "temperature",
  "extended_percent",
  "status",
  "wind_peak",
  "settings",
  "wind_sensor",
  "away",
];

const STATUSES = ["ACTIVE", "NIGHT", "SAFE", "AWAY"];

// The tracker is switched off by hand, so a power cut part way through a write
// leaves a truncated final row on the card. Rows are validated individually and
// bad ones are dropped, because rejecting the whole batch would strand a day of
// good readings behind one bad line.
export function parseRow(line) {
  const parts = line.trim().split(",");
  if (parts.length !== TELEMETRY_FIELDS.length) return null;

  const [time, temperature, extended, status, windPeak, settings, wind, away] =
    parts;

  const seconds = Number(time);
  if (!Number.isInteger(seconds) || seconds <= 0) return null;
  if (!STATUSES.includes(status)) return null;

  const numbers = [temperature, extended, windPeak].map(Number);
  if (numbers.some((n) => !Number.isFinite(n))) return null;

  const flags = [settings, wind, away];
  if (!flags.every((f) => f === "0" || f === "1")) return null;

  return {
    time: new Date(seconds * 1000).toISOString(),
    temperature: numbers[0],
    extended_percent: numbers[1],
    status,
    wind_peak: numbers[2],
    button_panel: {
      settings: settings === "1",
      wind_sensor: wind === "1",
      away: away === "1",
    },
  };
}

app.get("/", (req, res) => {
  res.send("Hello World!");
});

app.get("/update/download/update-v:version.bin", (req, res) => {
  const { version } = req.params;
  const filePath = path.join(__dirname, "updates", `update-v${version}.bin`);

  console.log(`Serving file: ${filePath}`);

  res.sendFile(filePath, (err) => {
    if (err) {
      console.error(`Error serving file: ${err.message}`);
      res.status(404).send({ message: "File not found" });
    }
  });
});

app.get("/update/check/:version", (req, res) => {
  const { version } = req.params;
  const filePath = path.join(__dirname, "updates", `update-v${version}.bin`);
  res.send({ updateAvailable: existsSync(filePath) });
});

app.get("/time/est", (req, res) => {
  const estTime = new Date().toLocaleString("en-US", {
    timeZone: "America/New_York",
  });
  const estDate = new Date(estTime);
  res.send({ datetime: estDate.toISOString() });
});

app.post("/data", async (req, res) => {
  const raw = typeof req.body === "string" ? req.body : "";
  if (!raw.trim()) {
    // Nothing to store. Answer 200 anyway so the tracker clears a spool that
    // holds no usable rows instead of retrying it forever.
    return res.status(200).send({ saved: 0, skipped: 0 });
  }

  const lines = raw.split("\n").filter((line) => line.trim() !== "");
  const records = [];
  let skipped = 0;
  for (const line of lines) {
    const record = parseRow(line);
    if (record) records.push(record);
    else skipped++;
  }

  if (records.length === 0) {
    console.log(`Rejected every row of a ${lines.length} row batch`);
    return res.status(200).send({ saved: 0, skipped });
  }

  // One object per upload. Cloud Storage objects are immutable, so appending to a
  // daily file would mean a read, a rewrite and a race; a new object per batch
  // has none of that and BigQuery reads the whole prefix as one external table.
  const now = new Date();
  const day = now.toISOString().slice(0, 10);
  const stamp = now.toISOString().replace(/[:.]/g, "-");
  const objectName = `raw/${day}/${stamp}.ndjson`;
  const ndjson = records.map((r) => JSON.stringify(r)).join("\n") + "\n";

  try {
    await storage
      .bucket(TELEMETRY_BUCKET)
      .file(objectName)
      .save(ndjson, { contentType: "application/x-ndjson" });
  } catch (error) {
    console.error(`Failed writing gs://${TELEMETRY_BUCKET}/${objectName}:`, error);
    return res.status(500).send({ message: "Error saving data" });
  }

  console.log(`Saved ${records.length} rows (${skipped} skipped) to ${objectName}`);
  res.status(200).send({ saved: records.length, skipped, object: objectName });
});

function getLocalIp() {
  const interfaces = networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]) {
      if (iface.family === "IPv4" && !iface.internal) {
        return iface.address;
      }
    }
  }
  return "localhost";
}

if (process.env.NODE_ENV !== "test") {
  app.listen(PORT, "0.0.0.0", () => {
    const localIp = getLocalIp();
    console.log(`Server is running on http://${localIp}:${PORT}`);
  });
}
