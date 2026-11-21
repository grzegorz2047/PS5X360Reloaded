// SPDX-License-Identifier: MIT
// Fix 66 (tools/source-1.0.2/fixes/66-phone-latency.patch): the phone camera page's delay by stage.
// node tools/check-phone-latency.mjs <assembled tree>
//  (a) latency.mjs as the console serves it (port/phone_camera_page.hpp, kLatencyModule):
//      percentiles by nearest rank, the hand joints' One Euro lag from the filters' cutoff
//      (1 / (2 pi (minCutoff + beta |speed|))), the window's report (medians, p95, counts, fps,
//      only since the previous report) and the page's values ('–' for a stage without samples);
//  (b) the page: it imports latency.mjs, times the camera, detection, tracking-to-send and
//      network stages and the smoothing lag, shows the line under the banner and posts the report
//      to /api/motion/latency with its key while paired; its script still parses.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {execFileSync} from 'node:child_process';

const tree = process.argv[2];
if (!tree) throw Error('usage: node check-phone-latency.mjs <tree>');
const header = fs.readFileSync(path.join(tree, 'port/phone_camera_page.hpp'), 'utf8');
const between = (open, close) => {
  const a = header.indexOf(open), b = header.indexOf(close, a);
  assert.ok(a >= 0 && b > a, 'found ' + open);
  return header.slice(a + open.length, b);
};
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'phone-latency-'));
const modulePath = path.join(dir, 'latency.mjs');
fs.writeFileSync(modulePath, between('R"LATENCYJS(', ')LATENCYJS"'));
const L = await import(pathToFileURL(modulePath).href);

// (a)
assert.equal(L.percentile([], 50), NaN);
assert.equal(L.percentile([5, 1, 3, 2, 4], 50), 3);
assert.equal(L.percentile(Array.from({length: 100}, (_, n) => n + 1), 95), 95);
assert.deepEqual(L.STAGES, ['camera', 'detect', 'track', 'network', 'smoothing']);
assert.deepEqual(L.HAND_JOINTS, [7, 11]);

const filters = new Map();
assert.ok(Number.isNaN(L.smoothingLagMs(filters)), 'no filters yet: no lag');
filters.set('7x', {minCutoff: 1.5, beta: 1.6, dx: 0});
const still = L.smoothingLagMs(filters);
assert.ok(Math.abs(still - 1000 / (2 * Math.PI * 1.5)) < 1e-9, 'a still hand: about 106 ms');
filters.set('7x', {minCutoff: 1.5, beta: 1.6, dx: 2});
const moving = L.smoothingLagMs(filters);
assert.ok(Math.abs(moving - 1000 / (2 * Math.PI * (1.5 + 3.2))) < 1e-9 && moving < still, 'a moving hand lags less');
filters.set('11y', {minCutoff: 1.5, beta: 1.6, dx: 0});
assert.ok(Math.abs(L.smoothingLagMs(filters) - still) < 1e-9, 'the slowest-following hand filter');
filters.set('3x', {minCutoff: .1, beta: 0, dx: 0});
assert.ok(Math.abs(L.smoothingLagMs(filters) - still) < 1e-9, 'only the hands');

const w = new L.StageWindow();
w.reset(1000);
for (let n = 0; n < 60; ++n) {
  w.frame(1000 + n * 33);
  w.add('detect', 20 + (n % 10));
  w.add('camera', 10);
  if (n % 2) w.add('track', 3);
}
w.add('detect', -1);
w.add('detect', NaN);
w.add('unknown', 5);
const r = w.report(3000, 1);
assert.equal(r.frames, 60);
assert.equal(r.fps, 30);
assert.equal(r.queue, 1);
assert.deepEqual(r.stages.detect, [24, 29, 60]);
assert.deepEqual(r.stages.camera, [10, 10, 60]);
assert.deepEqual(r.stages.track, [3, 3, 30]);
assert.deepEqual(r.stages.network, [-1, -1, 0], 'no samples: -1');
assert.deepEqual(L.summaryValues(r), ['10', '24', '3', '–', '–']);
const next = w.report(5000, 0);
assert.equal(next.frames, 0, 'only since the previous report');
assert.equal(next.stages.detect[2], 0);
assert.ok(JSON.stringify(r).length < 2048, 'the report fits the console limit');

// (b)
const page = between('R"KINECTPAGE(', ')KINECTPAGE"');
assert.ok(page.includes("import {StageWindow, smoothingLagMs, summaryValues, REPORT_MS} from './latency.mjs?v=66';") ||
            page.includes("import {StageWindow, smoothingLagMs, summaryValues, REPORT_MS, frameReason} from './latency.mjs?v=102';"))  // fix 102;
for (const part of [
  "latencyWindow.add('detect', detectedAt - capturedAt);",
  "if (!demo) latencyWindow.add('camera', capturedAt - frameCaptured);",
  "if (latest) latencyWindow.add('track', performance.now() - detectedAt);",
  "if (pose && onConsole) latencyWindow.add('network', (performance.now() - sentAt) / 2);",
  "latencyWindow.add('smoothing', smoothingLagMs(tracker.filters));",
  "frameCaptured = meta.captureTime ?? meta.presentationTime ?? now;",
  "status.after(latencyLine);",
  "if (onConsole && paired && consoleKey)",
  "fetch('/api/motion/latency', {method: 'POST', headers: {'X-Key': consoleKey, 'Content-Type': 'application/json'},",
]) assert.ok(page.includes(part), 'page: ' + part);
const TEXT = JSON.parse(page.slice(page.indexOf('const TEXT = ') + 13, page.indexOf(';\n', page.indexOf('const TEXT = '))));
for (const code of ['en', 'pt', 'es', 'pl'])
  assert.ok(/\{0\}.*\{1\}.*\{2\}.*\{3\}.*\{4\}/.test(TEXT[code].latency), 'the line in ' + code);
const script = page.slice(page.indexOf('<script type="module">') + 22, page.indexOf('</script>', page.indexOf('<script type="module">')));
const scriptPath = path.join(dir, 'page.mjs');
fs.writeFileSync(scriptPath, script);
execFileSync(process.execPath, ['--check', scriptPath]);

fs.rmSync(dir, {recursive: true, force: true});
console.log('check-phone-latency: ok');
