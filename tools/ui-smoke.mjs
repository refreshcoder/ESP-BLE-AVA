/*
 * Smoke test for the SPA: renders main/www/index.html in jsdom against canned API
 * responses and drives every tab (status / data / settings), the filters, the sort,
 * the profile dialog, the mode switch and the language toggle.
 *
 *   cd tools && npm i jsdom && node ui-smoke.mjs
 *
 * Not wired into CI on purpose: it needs a node_modules tree, and the firmware build
 * does not. Run it before touching index.html.
 */
import fs from "node:fs";
import { JSDOM } from "jsdom";

const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");

const STATUS = { ok:true, ver:"2.2.1", uptime_s:3725, heap:174760, min_heap:169888, active:1, count:2,
  rotation:false, rotation_s:30, scan_itvl_ms:300, scan_win_ms:60, scan_filter:true, scan_full_duty:true,
  scan_clear_rounds:true, scan_duration_s:120, reset_reason:"sw", boots:140, brownouts:1, adv_starts:3,
  tx_dbm:9, scan_pkts:8856, scan_running_ms:64000, scan_filtered:7959, led:"sniff", board:"XIAOESP32-C6",
  storage:"9/128", mode:"sniff", adv:false, scan:true, detected:3, ap:true, sta:0, log_seq:12, ip:"192.168.4.1" };

const REC = (a, extra) => Object.assign({ addr:a, addr_type:1, adv_type:0, name:"",
  raw:"02011617ff0001b5000209641da700000090c21065ab011000000003033cfe",
  raw_len:31, rsp_len:0, mfg_company:256, mfg_vendor:"TomTom", mfg:"b5000209641da700000090c21065ab0110000000", mfg_src:0,
  svc:"0xFE3C", svc_name:"DingTalk", ibeacon:false, rssi:-47, rssi_max:-45, rssi_avg:-48, packets:1460, itvl_ms:82,
  dwell_ms:120000, boot_seq:139, uptime_s:3725 }, extra||{});

const HIST = { ok:true, available:true, count:3, items:[
  REC("faa89ac1ab65"),
  REC("4d9fec80fbf5", { mfg_company:76, mfg_vendor:"Apple", svc:"", svc_name:null, ibeacon:false, rssi:-82,
    packets:9, raw:"0aff4c001006241a0f", raw_len:9, mfg:"" }),
  REC("081287a778b0", { name:"OnePlus Buds Ace 2", mfg_company:undefined, mfg_vendor:undefined, svc:"",
    svc_name:null, rssi:-71, packets:120, mfg:undefined, raw:"02011a0d094f6e65506c75732042756473", raw_len:15 })
]};
const PINS = { ok:true, available:true, count:1, slots:32, items:[ Object.assign(REC("faa89ac1ab65"), { id:0 }) ]};
const CFG = { ok:true, ver:1, count:2, active:1, seq_gen:0, mode:1,
  rotation:{ enabled:false, seconds:30 },
  scan:{ itvl_ms:300, win_ms:60, filter_noise:true, full_duty:true, clear_each_round:true, duration_s:120 },
  ap:{ ssid:"BeaconGW-B42A", pass:"beaconb42a" },
  profiles:[
    { id:0, name:"copy-old", enabled:true, kind:1, raw:"0201060aff4c0002",
      uuid:"00000000000000000000000000000000", major:0, minor:0, ref_rssi:-59, radio_dbm:9,
      adv_itvl_ms:100, scannable:false, use_rand_addr:false, use_seq:0 },
    { id:1, name:"DingTalk M1Pro", enabled:true, kind:1,
      raw:"02011617ff0001b5000209641da700000090c21065ab011000000003033cfe",
      uuid:"00000000000000000000000000000000", major:0, minor:0, ref_rssi:-59, radio_dbm:9,
      adv_itvl_ms:100, scannable:false, use_rand_addr:false, use_seq:0 }
  ]};
const LOG = { ok:true, n:2, last_seq:12, items:[
  { seq:11, up_ms:120000, type:"ACT", rssi:0, msg:"btn 1x: mode=clone" },
  { seq:12, up_ms:125000, type:"CFG", rssi:0, msg:"profile 1 deleted" } ]};

const calls = [];
const routes = { "/api/status":STATUS, "/api/history":HIST, "/api/pins":PINS, "/api/profiles":CFG,
                 "/api/log?since=0&limit=30":LOG };

const dom = new JSDOM(html, { runScripts:"dangerously", url:"http://192.168.4.1/", pretendToBeVisual:true,
  beforeParse(w) {
    w.localStorage.setItem("bgw_token", "tok");
    w.fetch = (p, o) => {
      calls.push(((o && o.method) || "GET") + " " + p + (o && o.body ? " " + o.body : ""));
      const body = routes[p] !== undefined ? routes[p] : { ok:true };
      return Promise.resolve({ ok:true, status:200, text:() => Promise.resolve(JSON.stringify(body)) });
    };
    w.navigator.clipboard = { writeText(){ return Promise.resolve(); } };
    w.confirm = () => true;
  }});
const { window } = dom;
const $ = id => window.document.getElementById(id);
const wait = ms => new Promise(r => setTimeout(r, ms));

const fails = [];
const ok = (cond, msg) => { if (!cond) fails.push(msg); };

await wait(150);
ok($("login").style.display !== "flex", "unlocked: login hidden");
ok($("tab-status").classList.contains("active"), "status tab active");
ok($("p-mode").textContent.length > 0, "mode pill filled: " + $("p-mode").textContent);
ok(/8856|147/.test($("mode-panel").textContent), "sniff panel counters: " + $("mode-panel").textContent.replace(/\s+/g," ").slice(0,140));
ok(!!$("mode-panel").querySelector(".bar"), "progress bar present");
ok($("runtime").children.length >= 16, "runtime rows: " + $("runtime").children.length);
ok($("pow-card").style.display !== "none", "brownout card shown");
ok($("mode-seg").querySelector('[data-mode="sniff"]').classList.contains("on"), "sniff highlighted");

$("log-toggle").click(); await wait(80);
ok($("log-list").children.length === 2, "log rows: " + $("log-list").children.length);
ok($("log-list").textContent.indexOf("btn 1x") >= 0, "log text rendered");

window.document.querySelector('nav button[data-tab="data"]').click(); await wait(150);
ok($("tab-data").classList.contains("active"), "data tab active");
let recs = $("list").querySelectorAll(".rec");
ok(recs.length === 3, "archive cards: " + recs.length);
ok(!!recs[0].querySelector(".tag.ding"), "dingtalk badge on first card");
ok(recs[0].querySelector(".rssi").textContent === "-47", "rssi badge");
ok(recs[1].textContent.indexOf("Apple") >= 0, "apple vendor shown");
ok(recs[2].querySelector(".tag").textContent === "BLE", "name-only device tagged BLE: " + recs[2].querySelector(".tag").textContent);

recs[0].querySelector(".h").click(); await wait(60);
ok(recs[0].classList.contains("open"), "card expands");
ok(recs[0].querySelector(".hex").textContent.indexOf("02 01 16") >= 0, "raw hex grouped");
ok(recs[0].querySelector(".kv").textContent.indexOf("random") >= 0, "addr type shown");

const chip = f => window.document.querySelector('.chip[data-filter="' + f + '"]');
chip("pub").click(); await wait(40);
ok($("list").querySelectorAll(".rec").length === 0, "public filter hides random: " + $("list").querySelectorAll(".rec").length);
chip("pub").click(); await wait(40);
ok($("list").querySelectorAll(".rec").length === 3, "filter toggles back");
chip("full").click(); await wait(40);
ok($("list").querySelectorAll(".rec").length === 2, "full-window filter: " + $("list").querySelectorAll(".rec").length);
chip("full").click(); await wait(20);
chip("ding").click(); await wait(40);
ok($("list").querySelectorAll(".rec").length === 1, "dingtalk filter: " + $("list").querySelectorAll(".rec").length);
chip("ding").click(); await wait(40);

const kw = $("kw"); kw.value = "oneplus"; kw.dispatchEvent(new window.Event("input")); await wait(40);
ok($("list").querySelectorAll(".rec").length === 1, "keyword filter: " + $("list").querySelectorAll(".rec").length);
kw.value = ""; kw.dispatchEvent(new window.Event("input")); await wait(40);
const sort = $("sort"); sort.value = "rssi"; sort.dispatchEvent(new window.Event("change")); await wait(40);
ok($("list").querySelectorAll(".rec")[0].querySelector(".rssi").textContent === "-47", "rssi sort");
sort.value = "pkt"; sort.dispatchEvent(new window.Event("change")); await wait(40);
ok($("list").querySelectorAll(".rec")[0].querySelector(".rssi").textContent === "-47", "pkt sort");

window.document.querySelector('#sub-seg button[data-sub="pins"]').click(); await wait(150);
ok($("list").querySelectorAll(".rec").length === 1, "pins list: " + $("list").querySelectorAll(".rec").length);
ok($("tools").style.display === "flex", "tools visible for pins");

window.document.querySelector('#sub-seg button[data-sub="profiles"]').click(); await wait(150);
const prows = $("list").querySelectorAll(".rec");
ok(prows.length === 2, "profile cards: " + prows.length);
ok(prows[1].textContent.indexOf("DingTalk M1Pro") >= 0, "profile name");
ok(!!prows[1].querySelector(".tag.ding"), "active profile badge");
ok($("btn-new").style.display !== "none", "new button visible");
ok($("tools").style.display === "none", "tools hidden on profiles");

prows[1].querySelector('[data-pact="edit"]').click(); await wait(60);
ok($("dlg").classList.contains("show"), "dialog opens");
ok($("d-name").value === "DingTalk M1Pro", "dialog prefill name");
ok($("d-raw").value.length === 62, "dialog prefill raw (" + $("d-raw").value.length + ")");
$("d-raw").value = "zz"; $("d-save").click(); await wait(60);
ok($("dlg").classList.contains("show"), "bad hex keeps dialog open");

/* the advertised address: only the JSON that reaches the API matters */
ok($("d-addr").disabled, "address input disabled while it is the device's own");
$("d-addrmode").value = "rand"; $("d-addrmode").onchange();
ok(!$("d-addr").disabled, "address input enabled for a static random address");
$("d-raw").value = CFG.profiles[1].raw; $("d-addr").value = "";
$("d-save").click(); await wait(60);
ok($("dlg").classList.contains("show"), "random without an address keeps the dialog open");
$("d-addr").value = "faa89ac1ab65"; $("d-save").click(); await wait(150);
ok(!$("dlg").classList.contains("show"), "good save closes dialog");
ok(calls.some(c => c.indexOf("PUT /api/profiles/1") === 0), "PUT sent");
const randPut = calls.filter(c => c.indexOf("PUT /api/profiles/1") === 0).pop() || "";
ok(/"use_rand_addr":true/.test(randPut) && randPut.indexOf("faa89ac1ab65") > 0,
   "rand_addr reaches the API (" + randPut + ")");

$("btn-new").click(); await wait(60);
ok($("dlg").classList.contains("show"), "new dialog opens");
$("d-name").value = "x";
$("d-kind").querySelector('[data-kind="0"]').click(); await wait(30);
ok($("d-ibox").style.display !== "none" && $("d-rawbox").style.display === "none", "ibeacon fields swap in");
$("d-cancel").click();

window.document.querySelector('nav button[data-tab="settings"]').click(); await wait(200);
ok($("s-itvl").value === "300" && $("s-win").value === "60", "scan fields loaded");
ok($("s-dur").value === "120", "duration loaded");
ok($("s-ssid").value === "BeaconGW-B42A", "ap ssid loaded");
ok($("s-rot-s").value === "30", "rotation loaded");
$("s-itvl").value = "400"; $("save-scan").click(); await wait(100);
ok(calls.some(c => c.indexOf("POST /api/scan") === 0), "scan save posts");
ok($("maint-kv").textContent.indexOf("2.2.1") >= 0, "maint kv version");

window.document.querySelector('nav button[data-tab="status"]').click(); await wait(80);
const callMark = calls.length;
$("mode-seg").querySelector('[data-mode="idle"]').click(); await wait(150);
ok(calls.slice(callMark).some(c => c.indexOf("POST /api/mode") === 0), "mode switch posts");

const lang = $("lang"); lang.value = "zh"; lang.dispatchEvent(new window.Event("change")); await wait(80);
ok(window.document.querySelector('nav button[data-tab="status"]').textContent === "状态",
   "zh translation: " + window.document.querySelector('nav button[data-tab="status"]').textContent);
ok($("kw").placeholder.indexOf("搜索") >= 0, "placeholder translated");

console.log(fails.length ? "FAILURES:\n - " + fails.join("\n - ") : "ALL CHECKS PASSED");
console.log("endpoints touched:", [...new Set(calls.map(c => c.split(" ")[1].split("?")[0]))].join(" "));
process.exit(fails.length ? 1 : 0);