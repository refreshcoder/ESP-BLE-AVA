// Static checks for main/www/index.html i18n
const fs = require("fs");
const { execFileSync } = require("child_process");

const file = "main/www/index.html";
const html = fs.readFileSync(file, "utf8");
let fail = 0;
const bad = m => { console.log("FAIL " + m); fail++; };

// 1) JS syntax
const script = html.slice(html.indexOf("<script>") + 8, html.lastIndexOf("</script>"));
const tmp = require("os").tmpdir() + "/bgw_check.js";
fs.writeFileSync(tmp, script);
try { execFileSync(process.execPath, ["--check", tmp], { stdio: "pipe" }); console.log("ok  script parses"); }
catch (e) { bad("script syntax: " + e.stderr.toString()); }

// 2) dictionary: one `key: [en, zh]` pair per entry
const dictStart = script.indexOf("const T = {");
const dictEnd = script.indexOf("\n};", dictStart);
if (dictStart < 0 || dictEnd < 0) { bad("no `const T = {` dictionary found"); process.exit(1); }
const T = eval("(" + script.slice(dictStart + "const T = ".length, dictEnd + 2) + ")");
const en = Object.keys(T), I18N = { en: T };
en.forEach(k => {
  const v = T[k];
  if (!Array.isArray(v) || v.length !== 2) bad("key " + k + " is not an [en, zh] pair");
  else if (typeof v[0] !== "string" || typeof v[1] !== "string") bad("key " + k + " has a non-string translation");
});
console.log("ok  keys total=" + en.length);

// 3) every referenced key exists
const refs = new Set();
for (const m of html.matchAll(/data-i18n(?:-ph|-html)?="([^"]+)"/g)) refs.add(m[1]);
for (const m of script.matchAll(/\bt\(\s*"([a-z0-9_]+)"\s*\)/g)) refs.add(m[1]);
[...refs].filter(k => !(k in I18N.en)).forEach(k => bad("unknown key referenced: " + k));
console.log("ok  referenced keys: " + refs.size);

// 4) unused keys (informational)
[...en].filter(k => !refs.has(k)).forEach(k => console.log("warn unused key: " + k));

// 5) ids preserved vs git HEAD ('s-en' etc. are deliberately retired)
const RETIRED = new Set(["s-en", "quick", "btn-freeze",
  // retired by the three-tab redesign: per-mode buttons and per-list tables gave way to the
  // segmented switch, the mode panel and one shared card list
  "m-sniff", "m-clone", "m-idle", "mode-title", "led-hint", "status-body", "pow-warn",
  "cap-stat", "cap-body", "cap-clear", "cap-limit", "cap-hint", "cap-meta",
  "pin-body", "pin-clear", "hist-body", "hist-clear", "hist-count",
  "prof-list", "prof-form", "form-title", "f-name", "f-kind", "f-uuid", "f-major",
  "f-minor", "f-ref", "f-raw", "f-rsp", "f-itvl", "f-tx", "f-scannable", "f-enabled",
  "f-use-rand", "f-rand", "f-save", "f-cancel", "f-new", "f-del", "f-seq",
  "log-body", "log-seq", "tab-capture", "tab-profiles", "tab-log", "l-msg-ok",
  "btn-freeze2", "hide-hint"]);
const oldHtml = execFileSync("git", ["show", "HEAD:" + file], { encoding: "utf8" });
const ids = s => new Set([...s.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]));
const a = ids(oldHtml), b = ids(html);
[...a].filter(x => !b.has(x) && !RETIRED.has(x)).forEach(x => bad("removed id: " + x));
[...b].filter(x => !a.has(x)).forEach(x => console.log("info new id: " + x));

// 6) every $("id") in script exists in html (hint-limit is optional by design)
for (const m of script.matchAll(/\$\("([^"]+)"\)/g))
  if (!b.has(m[1]) && m[1] !== "hint-limit") bad("$() on missing id: " + m[1]);

// 7) gzip still works (the CMake step needs it)
const zlib = require("zlib");
console.log("ok  gzip " + zlib.gzipSync(html, { level: 9 }).length + " bytes");

console.log(fail ? "\n" + fail + " FAILURE(S)" : "\nall static checks passed");
process.exit(fail ? 1 : 0);
