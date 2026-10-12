// Both buckets are reached through the same four calls, so the routes can be
// exercised against an in-memory copy in tests and in scripts/smoke.js:
//
//   list(prefix)        names of the objects under prefix, in name order
//   read(name)          the object's text, or null if there is no such object
//   write(name, text)   create or replace a JSON object
//   delete(name)        remove it; removing one that is already gone is not an error
//
// What a store may do is decided by the service account's IAM, not here: the
// portal can only read the telemetry bucket.

export function createGcsStore(bucket) {
  return {
    async list(prefix) {
      const [files] = await bucket.getFiles({ prefix });
      return files.map((file) => file.name);
    },
    async read(name) {
      try {
        const [contents] = await bucket.file(name).download();
        return contents.toString("utf8");
      } catch (error) {
        if (error.code === 404) return null;
        throw error;
      }
    },
    async write(name, text) {
      // A single request rather than a resumable upload: these objects are a few
      // hundred bytes and a resumable session costs an extra round trip.
      await bucket.file(name).save(text, { contentType: "application/json", resumable: false });
    },
    async delete(name) {
      await bucket.file(name).delete({ ignoreNotFound: true });
    },
  };
}

export function createMemoryStore(objects = {}) {
  const contents = new Map(Object.entries(objects));
  return {
    async list(prefix) {
      return [...contents.keys()].filter((name) => name.startsWith(prefix)).sort();
    },
    async read(name) {
      return contents.has(name) ? contents.get(name) : null;
    },
    async write(name, text) {
      contents.set(name, text);
    },
    async delete(name) {
      contents.delete(name);
    },
  };
}
