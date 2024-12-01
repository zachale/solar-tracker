import express from "express";
import path from "path";
import { networkInterfaces } from "os";
import { existsSync } from "fs";

import { fileURLToPath } from "url";
import { dirname } from "path";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const app = express();
const PORT = 443;

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

app.listen(PORT, "0.0.0.0", () => {
  const localIp = getLocalIp();
  console.log(`Server is running on http://${localIp}:${PORT}`);
});
