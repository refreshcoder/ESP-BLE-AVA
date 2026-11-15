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

// 2) dictionaries
const dictSrc = script.slice(script.indexOf("const I18N = {"), script.indexOf("const LANGS"));
const I18N = eval("(" + dictSrc.replace("const I18N =", "") .trim().replace(/;$/, "") + ")");
const en = Object.keys(I18N.en), zh = Object.keys(I18N.zh);
if (en.length !== zh.length) bad("key count en=" + en.length + " zh=" + zh.length);
en.filter(k => !(k in I18N.zh)).forEach(k => bad("missing zh: " + k));
zh.filter(k => !(k in I18N.en)).forEach(k => bad("missing en: " + k));
console.log("ok  keys en=" + en.length + " zh=" + zh.length);

// 3) every referenced key exists
const refs = new Set();
for (const m of html.matchAll(/data-i18n(?:-ph|-html)?="([^"]+)"/g)) refs.add(m[1]);
for (const m of script.matchAll(/\bt\(\s*"([a-z0-9_]+)"\s*\)/g)) refs.add(m[1]);
for (const m of script.matchAll(/\btf\(\s*"([a-z0-9_]+)"/g)) refs.add(m[1]);
for (const m of script.matchAll(/\bL\(\s*"([a-z0-9_]+)"/g)) refs.add(m[1]);
for (const m of script.matchAll(/\b(?:MODE_KEY|LED_KEY)\s*=\s*\{([^}]+)\}/g))
  for (const k of m[1].matchAll(/:\s*"([a-z0-9_]+)"/g)) refs.add(k[1]);
[...refs].filter(k => !(k in I18N.en)).forEach(k => bad("unknown key referenced: " + k));
console.log("ok  referenced keys: " + refs.size);

// 4) unused keys (informational)
[...en].filter(k => !refs.has(k)).forEach(k => console.log("warn unused key: " + k));

// 5) ids preserved vs git HEAD ('s-en' etc. are deliberately retired)
const RETIRED = new Set(["s-en", "quick", "btn-freeze"]);
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
