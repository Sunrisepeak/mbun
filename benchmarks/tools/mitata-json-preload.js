import process from "node:process";

// Bun 1.3.x console.log truncates mitata's large JSON payload. mitata prefers
// globalThis.print when it exists, so stream the complete payload to stdout.
globalThis.print = (...values) => {
  process.stdout.write(values.map(String).join(" ") + "\n");
};
