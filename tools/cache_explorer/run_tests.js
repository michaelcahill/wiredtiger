#!/usr/bin/env node
// Headless tests for the cache explorer HTML.
//
// Usage: node run_tests.js [FILE.html ...]
//        (default: ./cache_explorer.html)
//
// Loads each generated page in jsdom with real d3 (inlined in the page) and a
// stubbed canvas 2D context, then exercises every view: default state, source
// radio switching, scatter + log scales, time series, page grid, reserved
// read_gen colors, tooltips, and the all-special-snapshot edge case.
// Exits non-zero if any check fails or a file throws.
"use strict";

let JSDOM;
try {
  ({ JSDOM } = require("jsdom"));
} catch {
  console.error("error: jsdom is required: npm install jsdom");
  process.exit(2);
}

const files = process.argv.slice(2);
if (!files.length) files.push("cache_explorer.html");

function testFile(file) {
  const fs = require("fs");
  const html = fs.readFileSync(file, "utf8");

  const dom = new JSDOM(html, { pretendToBeVisual: true, runScripts: "outside-only" });
  const { window } = dom;
  global.window = window;
  global.document = window.document;
  global.devicePixelRatio = 1;

  // canvas 2D stub: record nothing, answer the queries the app/d3 make
  const noop = () => {};
  const ctxStub = new Proxy({}, {
    get: (t, p) => (p === "measureText" ? () => ({ width: 40 })
      : (p === "createLinearGradient" ? () => ({ addColorStop: noop }) : noop)),
    set: () => true,
  });
  window.HTMLCanvasElement.prototype.getContext = () => ctxStub;

  // jsdom reports 0 client sizes; give the canvas a real size before the app reads it
  const cv = window.document.getElementById("cv");
  Object.defineProperty(cv, "clientWidth", { value: 1200 });
  Object.defineProperty(cv, "clientHeight", { value: 700 });

  // fire once like a browser's first layout pass (also catches init-order bugs)
  window.ResizeObserver = class { constructor(cb) { this.cb = cb; } observe() { this.cb(); } };
  global.ResizeObserver = window.ResizeObserver;
  global.Option = window.Option;

  // run all inline scripts in order: inlined d3 first, then the app
  for (const s of [...window.document.querySelectorAll("script")]) eval.call(window, s.textContent);

  const doc = window.document;
  const $ = id => doc.getElementById(id);
  const fire = el => el.dispatchEvent(new window.Event("change"));
  const mouse = (x, y) => cv.dispatchEvent(new window.MouseEvent("mousemove", { clientX: x, clientY: y }));
  const assert = (cond, msg) => { if (!cond) throw new Error(`check failed: ${msg}`); };

  // --- defaults: grid view, busiest non-internal source selected ---
  const stats = () => $("stats").textContent;
  assert($("mode").value === "grid", "initial view is the page grid");
  // option groups must match the initial mode without any user interaction
  assert($("grid-opts").style.display !== "none", "grid controls visible on load");
  assert($("scatter-opts").style.display === "none", "scatter controls hidden on load");
  assert($("ts-opts").style.display === "none", "time series controls hidden on load");
  const radios = [...doc.querySelectorAll('input[name="source"]')];
  assert(radios.length >= 1, "one radio per source");
  const checked = radios.filter(r => r.checked);
  assert(checked.length === 1, "exactly one source selected");
  const [src] = checked.map(r => r.value);
  assert(!src.startsWith("WiredTiger") || radios.every(r => r.value.startsWith("WiredTiger")),
    "default is a non-internal table when one exists");
  assert(/pages|no pages/.test(stats()), `grid stats rendered (${stats()})`);

  // --- source radio drives the grid ---
  const pick = v => { const r = radios.find(r => r.value === v); r.checked = true; fire(r); };
  const alt = radios.find(r => r.value !== src);
  if (alt) {
    pick(alt.value);
    assert(stats().includes(alt.value), `grid follows radio switch (${stats()})`);
    pick(src);
    assert(stats().includes(src), "grid follows radio switch back");
  }

  // --- scatter uses the selected source ---
  $("mode").value = "scatter"; fire($("mode"));
  assert(/points/.test(stats()), `scatter renders (${stats()})`);
  assert(doc.querySelectorAll("#overlay .axis text").length > 0, "d3 axes generated");
  if (alt) {
    pick(alt.value);
    assert(/points|no data/.test(stats()), `scatter follows radio switch (${stats()})`);
    pick(src);
  }

  // --- log scales ---
  $("logx").checked = true; fire($("logx"));
  assert(!/NaN|Infinity/.test(stats()), "log X renders without NaN");
  $("logy").checked = true; fire($("logy"));
  assert(!/NaN|Infinity/.test(stats()), "log Y renders without NaN");
  $("logx").checked = false; fire($("logx"));
  $("logy").checked = false; fire($("logy"));

  // --- time series uses the selected source ---
  $("mode").value = "timeseries"; fire($("mode"));
  assert(/snapshots/.test(stats()), `time series renders (${stats()})`);
  mouse(600, 350); // may or may not hit a point; must never produce garbage
  const tipText = $("tooltip").textContent || "";
  assert(!tipText.includes("null") && !tipText.includes("undefined"), "no garbage tooltip");

  // --- page grid + reserved read_gen colors ---
  $("mode").value = "grid"; fire($("mode"));
  $("cfa").value = "read_gen"; fire($("cfa"));
  const fills = [...doc.querySelectorAll("#overlay rect")].map(r => r.getAttribute("fill")).filter(Boolean);
  assert(fills.includes("#8a93a3"), "read_gen 0 swatch (not set)");
  assert(fills.some(f => f.startsWith("hsl(140")), "read_gen 1 swatch (evict soon)");
  assert(fills.some(f => f.startsWith("hsl(285")), "read_gen 2 swatch (won't need)");
  const labels = [...doc.querySelectorAll("#overlay text")].map(t => t.textContent);
  assert(labels.some(t => t.includes("≥3")), "legend marks the ≥3 scale");

  // --- discrete pin_reason colours: one distinct fixed colour per value ---
  $("cfa").value = "pin_reason"; fire($("cfa"));
  const pinFills = [...doc.querySelectorAll("#overlay rect")].map(r => r.getAttribute("fill")).filter(Boolean);
  // d3's UMD build attaches to the host global, not jsdom's window
  // one swatch per named reason; colours come from the page's PIN_REASON_COLORS in name-table order
  const nNames = (html.match(/PIN_REASON_NAMES = \{[^;]*\}/)[0].match(/: "[^"]*"/g) || []).length;
  const pinColours = global.d3.schemeTableau10.concat(global.d3.schemeSet2.slice(2, 4));
  for (const c of pinColours.slice(0, nNames))
    assert(pinFills.includes(c), `pin_reason swatch uses its fixed colour (${c})`);
  const pinLabels = [...doc.querySelectorAll("#overlay text")].map(t => t.textContent);
  assert(pinLabels.some(t => t.includes("evictable")), "pin reason legend names render");
  // every btree has exactly one root page, so a real dump always shows the root reason
  assert(pinLabels.some(t => t.includes("root")), "pin reason legend names the root page");
  $("cfa").value = "size"; fire($("cfa"));

  // --- dirty filter ---
  $("cfa").value = "size"; fire($("cfa"));
  const bothStats = stats();
  for (const [val, label] of [["1", "dirty"], ["0", "clean"]]) {
    $("gdirty").value = val; fire($("gdirty"));
    assert(/of/.test(stats()), `${label} filter shows a subset (${stats()})`);
  }
  $("gdirty").value = ""; fire($("gdirty"));
  assert(stats() === bothStats, "filter back to both restores full count");

  // --- page type filter (all / internal / leaf) ---
  for (const val of ["internal", "leaf"]) {
    $("gtype").value = val; fire($("gtype"));
    assert(/pages/.test(stats()), `${val} type filter renders (${stats()})`);
  }
  $("gtype").value = ""; fire($("gtype"));
  assert(stats() === bothStats, "type filter back to all restores full count");

  // --- two colour fields: split cell (colour 1 top-left / colour 2 bottom-right) + slider ---
  $("cfb").value = "dirty_bytes"; fire($("cfb"));
  assert($("balwrap").style.display !== "none", "balance slider visible with two fields");
  const dualLabels = [...doc.querySelectorAll("#overlay text")].map(t => t.textContent);
  assert(dualLabels.some(t => t.includes("colour 1 (top-left)")), "legend labels colour 1");
  assert(dualLabels.some(t => t.includes("colour 2 (bottom-right)")), "legend labels colour 2");
  $("cbal").value = "20"; $("cbal").dispatchEvent(new window.Event("input"));
  assert(/pages/.test(stats()), `dual-field grid renders at low balance (${stats()})`);
  $("cbal").value = "80"; $("cbal").dispatchEvent(new window.Event("input"));
  assert(/pages/.test(stats()), `dual-field grid renders at high balance (${stats()})`);
  // reserved read_gen states still take their fixed colors on either side of the gradient
  $("cfa").value = "read_gen"; fire($("cfa"));
  assert(/pages/.test(stats()), `dual-field grid with read_gen renders (${stats()})`);
  $("cfa").value = "size"; fire($("cfa"));
  $("cfb").value = ""; fire($("cfb"));
  assert($("balwrap").style.display === "none", "balance slider hidden with one field");

  mouse(300, 200);
  if ($("tooltip").style.display !== "none")
    assert(!tipHasGarbage(), "grid tooltip well-formed");

  // --- a snapshot with only reserved read_gen values must not throw ---
  if (alt) {
    pick(alt.value);
    $("gs").value = "0"; fire($("gs"));
    assert(/pages/.test(stats()), `all-special edge renders (${stats()})`);
    pick(src);
  }

  function tipHasGarbage() {
    const t = $("tooltip").textContent || "";
    return t.includes("null") || t.includes("undefined");
  }

  console.log(`${file}: PASS (default source ${src}, ${stats()})`);
}

let failed = 0;
for (const f of files) {
  try {
    testFile(f);
  } catch (e) {
    failed++;
    console.error(`${f}: FAIL — ${e.message}`);
  }
}
process.exit(failed ? 1 : 0);
