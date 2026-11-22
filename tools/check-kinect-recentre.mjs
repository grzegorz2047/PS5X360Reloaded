// SPDX-License-Identifier: MIT
// Fix 107 (tools/source-1.0.2/fixes/107-kinect-emergency-pose.patch): re-centring the phone camera
// from the pad. The phone page (port/phone_camera_page.hpp) learns of a request in the console's
// answers to its frames and in the status, counts down, runs its own Re-centre once and reports it.
//   node tools/check-kinect-recentre.mjs <assembled tree>
//  (a) RecentreState: a new number starts a countdown from the console's milliseconds (3 s when
//      none, 10 s at most); the same or an older number does nothing; the seconds shown; it fires
//      once, at the end, and remembers the number it ran;
//  (b) the page: both answers go to it, it re-centres with the cause "pad" when it fires, the
//      countdown replaces the calibration line, the report carries "recentred", and the countdown
//      text is in the page's four languages with its {0}.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const tree = process.argv[2] || '.';
let failures = 0;
const check = (what, fn) => {
  try { fn(); } catch (e) { failures++; console.log('FAIL: ' + what + ': ' + e.message); }
};
const between = (text, open, close) => {
  const a = text.indexOf(open), b = text.indexOf(close, a);
  assert.ok(a >= 0 && b > a, 'no ' + open);
  return text.slice(a + open.length, b);
};
const header = fs.readFileSync(path.join(tree, 'port/phone_camera_page.hpp'), 'utf8');
const page = between(header, 'R"KINECTPAGE(', ')KINECTPAGE"');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'check-kinect-recentre-'));
fs.writeFileSync(path.join(work, 'latency.mjs'), between(header, 'R"LATENCYJS(', ')LATENCYJS"'));
const L = await import(pathToFileURL(path.join(work, 'latency.mjs')).href);

check('(a) a request counts down from the console\'s time and fires once', () => {
  const r = new L.RecentreState();
  assert.equal(r.remaining(0), 0);
  assert.equal(r.fire(0), false);
  assert.equal(r.note({ok: true}, 0), false);                      // an answer without a request
  assert.equal(r.note(null, 0), false);
  assert.equal(r.note({recentre: 1, recentre_in_ms: 2500}, 1000), true);
  assert.equal(r.remaining(1000), 3);                               // 2.5 s: "3"
  assert.equal(r.remaining(2600), 1);
  assert.equal(r.fire(3400), false);
  assert.equal(r.note({recentre: 1, recentre_in_ms: 2500}, 3400), false);  // the same request again
  assert.equal(r.fire(3500), true);
  assert.equal(r.done, 1);
  assert.equal(r.fire(3600), false);                                // once
  assert.equal(r.remaining(3600), 0);
  assert.equal(r.note({recentre: 1, recentre_in_ms: 0}, 4000), false);    // still the old one
  assert.equal(r.note({recentre: 2}, 5000), true);                  // no countdown given: 3 s
  assert.equal(r.fire(7999), false);
  assert.equal(r.fire(8000), true);
  assert.equal(r.done, 2);
  assert.equal(r.note({recentre: 3, recentre_in_ms: 999999}, 0), true);   // at most 10 s
  assert.equal(r.fire(9999), false);
  assert.equal(r.fire(10000), true);
  assert.equal(r.note({recentre: 4, recentre_in_ms: -5}, 0), true); // late: at once
  assert.equal(r.fire(0), true);
  assert.equal(r.note({recentre: 'x'}, 0), false);
  assert.equal(r.note({recentre: 2.5}, 0), false);
});
check('(b) the page takes both answers, re-centres with the cause "pad", shows and reports it', () => {
  // Fix 119 imports the camera picture's helpers too and reads the answer once for both.
  assert.ok(page.includes("RecentreState} from './latency.mjs?v=107';") ||
            page.includes("RecentreState,\n  PictureGate, packPicture, pictureCrop, PICTURE_WIDTH, PICTURE_HEIGHT} from './latency.mjs?v=119';"));
  assert.ok(page.includes('const recentring = new RecentreState();  // fix 107'));
  assert.ok(page.includes('if (json) recentring.note(json, performance.now());'));
  const oldRead = 'if (onConsole) recentring.note(await response.json()';
  const newRead = 'const answer = await response.json().catch(() => null);\n      recentring.note(answer, performance.now());';
  assert.ok(page.includes('if (onConsole) recentring.note(await response.json().catch(() => null), performance.now());') ||
            page.includes('    if (onConsole) {\n      ' + newRead));
  assert.ok(page.includes("if (recentring.fire(performance.now())) { tracker.resetCalibration('pad'); gestures.reset(); }"));
  assert.ok(page.includes("if (left) { calibrationText.textContent = T('recentre_in', left); return; }"));
  assert.ok(page.includes('report.recentred = recentring.done;'));
  // The frame's answer is read after the frame was taken (an error answer returned before it).
  const send = page.includes(oldRead) ? page.indexOf(oldRead) : page.indexOf(newRead);
  assert.ok(send > page.indexOf('if (!response.ok) {') && send > page.indexOf("latencyWindow.add('network'"));
});
check('(b) the countdown in the page\'s four languages', () => {
  const TEXT = eval('(' + between(page, 'const TEXT = ', ';\nconst lang = ') + ')');
  for (const code of ['en', 'pt', 'es', 'pl']) {
    assert.ok(typeof TEXT[code].recentre_in === 'string' && TEXT[code].recentre_in.includes('{0}'), code);
  }
  assert.equal(Object.keys(TEXT).length, 4);
});

fs.rmSync(work, {recursive: true, force: true});
if (failures) { console.log(failures + ' failure(s)'); process.exit(1); }
console.log('check-kinect-recentre.mjs: all passed');
