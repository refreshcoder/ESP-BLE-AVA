// Unit tests for the SPA's AD-structure parser (adMfg / companyName in main/www/index.html).
// The SPA parses 0xFF blocks itself so records captured before the firmware learned to do
// it (and records imported from an old JSON export) still show vendor data. Keep it honest.
const fs = require("fs");

const html = fs.readFileSync("main/www/index.html", "utf8");
const start = html.indexOf("function adMfg");
const end = html.indexOf("/* manufacturer data line");
if (start < 0 || end < 0 || end <= start) {
  console.log("FAIL cannot locate adMfg/companyName in main/www/index.html");
  process.exit(1);
}
const { adMfg, companyName } = new Function(html.slice(start, end) + "\nreturn { adMfg, companyName };")();

let fail = 0;
const eq = (name, got, want) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  console.log((ok ? "ok   " : "FAIL ") + name + (ok ? "" : "  got " + JSON.stringify(got) + " want " + JSON.stringify(want)));
  if (!ok) fail++;
};

// iBeacon: flags + 0xFF(company 0x004C) + 02 15 + uuid + major + minor + ref_rssi
const ibeacon = "0201061aff4c000215e2c56db5dffb48d2b060d0f5a71096e000010001c5";
eq("ibeacon company", adMfg(ibeacon).company, 0x004C);
eq("ibeacon payload starts 0215", adMfg(ibeacon).payload.slice(0, 4), "0215");
eq("ibeacon payload length", adMfg(ibeacon).payload.length / 2, 23);

// company id with an empty payload
eq("company only id", adMfg("02010603ff4c00").company, 0x004C);
eq("company only payload", adMfg("02010603ff4c00").payload, "");

// a name and nothing else
eq("no manufacturer block", adMfg("0201060b0948656c6c6f576f726c64"), null);

// AD length claims more bytes than the buffer holds
eq("truncated block", adMfg("0201061aff4c00"), null);

// little-endian company id, short payload
eq("little-endian id", adMfg("02010605ff3412aabb").company, 0x1234);
eq("short payload", adMfg("02010605ff3412aabb").payload, "aabb");

// a block after other AD structures, and a zero-length terminator
eq("second block", adMfg("02010605094142430005ff4c000102").company, 0x004C);

// degenerate inputs must not throw
eq("empty", adMfg(""), null);
eq("null", adMfg(null), null);
eq("too short", adMfg("02"), null);

eq("vendor known", companyName(0x004C), "Apple");
eq("vendor unknown", companyName(0x1234), "0x1234");

console.log(fail ? "\n" + fail + " test(s) failed" : "\nall parser tests passed");
process.exit(fail ? 1 : 0);