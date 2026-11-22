// SPDX-License-Identifier: MIT
// Fix 106 (tools/source-1.0.2/fixes/106-kinect-tracker.patch): the phone's Kinect tracker the console
// serves (port/phone_camera_tracker.hpp) and the page's report of frames without a body.
//   node tools/check-kinect-tracker.mjs <assembled tree>
// Kinect Sports (issue 15) lost its player at "Pick main player" while the player stood in the
// picture: the package's tracker dropped the whole body when a hip was unsure or below the picture,
// and a new picture size threw it to the 2.5 m start distance.
//  (a) a body close to the phone, hips unsure or below the picture, is kept: shoulders and arms as
//      seen, hips and legs inferred (confidence 0.49), its last place kept; without both shoulders
//      there is still no body;
//  (b) calibration goes on through a new picture size (twice the size, turned), a re-centre starts
//      from the distance the body was at, the distance goes below 1 m; restarts are reported;
//  (c) the page's window counts the reasons and restarts into the report, which the console's
//      parser (phone_latency.hpp, checked by check-kinect-tracker.cpp) reads; the page uses them.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const tree = process.argv[2] || '.';
const here = path.dirname(fileURLToPath(import.meta.url));
const fixtures = path.join(here, 'source-1.0.2', 'fixtures', 'kinect-1.0.2');
let failures = 0;
const check = (what, fn) => {
  try { fn(); } catch (e) { failures++; console.log('FAIL: ' + what + ': ' + e.message); }
};
const between = (text, open, close) => {
  const a = text.indexOf(open), b = text.indexOf(close, a);
  assert.ok(a >= 0 && b > a, 'no ' + open);
  return text.slice(a + open.length, b);
};
const trackerHeader = fs.readFileSync(path.join(tree, 'port/phone_camera_tracker.hpp'), 'utf8');
const pageHeader = fs.readFileSync(path.join(tree, 'port/phone_camera_page.hpp'), 'utf8');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'check-kinect-tracker-'));
fs.writeFileSync(path.join(work, 'kinect_tracker.mjs'), between(trackerHeader, 'R"KINECTTRACKER(', ')KINECTTRACKER"'));
fs.writeFileSync(path.join(work, 'latency.mjs'), between(pageHeader, 'R"LATENCYJS(', ')LATENCYJS"'));
for (const name of ['pose.mjs', 'demo_body.mjs']) fs.copyFileSync(path.join(fixtures, name), path.join(work, name));
const T = await import(pathToFileURL(path.join(work, 'kinect_tracker.mjs')).href);
const D = await import(pathToFileURL(path.join(work, 'demo_body.mjs')).href);
const L = await import(pathToFileURL(path.join(work, 'latency.mjs')).href);

// A standing body at `distance`, `hidden` landmarks unsure; `below` moves the hips' picture point
// below the frame (a player close to a phone at chest height).
const frame = (distance, {hidden = [], width = 640, height = 480, focal = 520, below = false} = {}) => {
  const f = D.observe(D.STAND, {x: 0, y: 0, distance}, {width, height, focal, jitter: .002, hidden});
  if (below) for (const n of [23, 24, 25, 26, 27, 28, 29, 30, 31, 32]) f.image[n].y = 1.15 + (n - 23) * .02;
  return f;
};
// Calibrated at `distance`: 1.5 s of a still body (the tracker needs 15 frames over 0.9 s).
const calibrated = (distance = 2.5, options = {}) => {
  const tracker = new T.KinectTracker(options.tracker);
  let t = 1000, last;
  for (; t < 2500; t += 33) last = tracker.update(...Object.values(frame(distance, options)), t, options.width ?? 640, options.height ?? 480);
  assert.ok(last.status.calibrated, 'not calibrated');
  return {tracker, t, last};
};
const hipZ = result => result.pose.joints[0].z;  // the console's 2.5 m is z = 0
const distanceOf = result => T.BODY_DISTANCE - hipZ(result);

check('(a) a body with both hips unsure is kept, hips and legs inferred', () => {
  const {tracker, t} = calibrated();
  let r;
  for (let n = 0; n < 10; n++) r = tracker.update(...Object.values(frame(2.5, {hidden: [23, 24, 25, 26, 27, 28]})), t + n * 33, 640, 480);
  assert.ok(r.pose, 'the body was dropped: ' + r.status.reason);
  assert.equal(r.status.reason, 'hips_inferred');
  const j = r.pose.joints;
  for (const n of [0, 12, 13, 14, 16, 17, 18]) assert.ok(j[n].confidence <= .49, 'joint ' + n + ' not inferred: ' + j[n].confidence);
  for (const n of [4, 5, 6, 8, 9, 10]) assert.ok(j[n].confidence >= .5, 'arm joint ' + n + ' lost: ' + j[n].confidence);
});
check('(a) one hip unsure is enough to infer, not to drop', () => {
  const {tracker, t} = calibrated();
  const r = tracker.update(...Object.values(frame(2.5, {hidden: [24]})), t, 640, 480);
  assert.ok(r.pose && r.status.reason === 'hips_inferred');
});
check('(a) hips below the picture for 2 s: the body stays, at its last place', () => {
  const {tracker, t, last} = calibrated(2.0);
  const before = distanceOf(last);
  let r;
  for (let n = 0; n < 60; n++) r = tracker.update(...Object.values(frame(2.0, {below: true})), t + n * 33, 640, 480);
  assert.ok(r.pose, 'dropped after ' + 60 * 33 + ' ms: ' + r.status.reason);
  assert.equal(r.status.reason, 'hips_hidden');  // the page shows "step back"
  assert.ok(Math.abs(distanceOf(r) - before) < .15, 'moved from ' + before + ' to ' + distanceOf(r));
});
check('(a) without both shoulders there is no body', () => {
  const {tracker, t} = calibrated();
  const r = tracker.update(...Object.values(frame(2.5, {hidden: [11]})), t, 640, 480);
  assert.equal(r.pose, null);
  assert.equal(r.status.reason, 'no_body');
});
check('(a) a normal frame is placed as before', () => {
  const {last} = calibrated(2.5);
  assert.ok(last.pose && ['tracking', 'calibrating'].includes(last.status.reason));
  assert.ok(Math.abs(distanceOf(last)) - 2.5 < .2);
});
check('(b) twice the picture size: no new calibration, the distance stays', () => {
  const {tracker, t, last} = calibrated(2.5);
  tracker.takeEvents();
  let r;
  for (let n = 0; n < 20; n++) r = tracker.update(...Object.values(frame(2.5, {width: 1280, height: 960, focal: 1040})), t + n * 33, 1280, 960);
  assert.ok(r.status.calibrated, 'calibration restarted');
  assert.ok(Math.abs(distanceOf(r) - distanceOf(last)) < .15, distanceOf(last) + ' -> ' + distanceOf(r));
  const events = tracker.takeEvents();
  assert.deepEqual(events.map(e => e.cause), ['frame']);
  assert.deepEqual([events[0].width, events[0].height], [1280, 960]);
});
check('(b) the phone turned (640x480 to 480x640): still calibrated, same distance', () => {
  const {tracker, t, last} = calibrated(2.5);
  let r;
  for (let n = 0; n < 20; n++) r = tracker.update(...Object.values(frame(2.5, {width: 480, height: 640})), t + n * 33, 480, 640);
  assert.ok(r.status.calibrated);
  assert.ok(Math.abs(distanceOf(r) - distanceOf(last)) < .15, distanceOf(last) + ' -> ' + distanceOf(r));
});
check('(b) a re-centre starts from where the body was, not from 2.5 m', () => {
  const {tracker, t} = calibrated(2.5);
  let r, n = 0;
  for (; n < 60; n++) r = tracker.update(...Object.values(frame(1.5)), t + n * 33, 640, 480);  // stepped in
  const near = distanceOf(r);
  assert.ok(near < 1.8, 'did not follow the step in: ' + near);
  tracker.takeEvents();
  tracker.resetCalibration('recentre');
  r = tracker.update(...Object.values(frame(1.5)), t + n * 33, 640, 480);
  assert.ok(!r.status.calibrated && r.pose);
  assert.ok(Math.abs(distanceOf(r) - near) < .1, 'jumped from ' + near + ' to ' + distanceOf(r));
  for (n++; n < 120; n++) r = tracker.update(...Object.values(frame(1.5)), t + n * 33, 640, 480);
  assert.ok(r.status.calibrated && Math.abs(distanceOf(r) - near) < .15, 'calibrated at ' + distanceOf(r));
  assert.deepEqual(tracker.takeEvents().map(e => e.cause), ['recentre']);
});
check('(b) a typed start distance is taken as it is', () => {
  const tracker = new T.KinectTracker();
  tracker.setStartDistance(3);
  let r;
  for (let t = 1000; t < 2500; t += 33) r = tracker.update(...Object.values(frame(2.5)), t, 640, 480);
  assert.ok(r.status.calibrated && Math.abs(distanceOf(r) - 3) < .15, String(distanceOf(r)));
});
check('(b) closer than 1 m is no longer pinned at 1 m', () => {
  const {tracker, t} = calibrated(2.5);
  let r;
  for (let n = 0; n < 60; n++) r = tracker.update(...Object.values(frame(.8)), t + n * 33, 640, 480);
  assert.ok(r.pose && distanceOf(r) < .95 && distanceOf(r) >= .5, String(distanceOf(r)));
});
check('(c) the reasons and restarts reach the report', () => {
  assert.equal(L.frameReason(true, {}, null, null), 'stale');
  assert.equal(L.frameReason(false, {world: null, image: []}, null, null), 'no_landmarks');
  assert.equal(L.frameReason(false, {world: [], image: []}, null, {status: {reason: 'no_body'}}), 'no_body');
  assert.equal(L.frameReason(false, {world: [], image: []}, null, {status: {reason: 'calibrating'}}), 'no_body');
  assert.equal(L.frameReason(false, {world: [], image: []}, {}, {status: {reason: 'hips_inferred'}}), 'hips_inferred');
  assert.equal(L.frameReason(false, {world: [], image: []}, {}, {status: {reason: 'tracking'}}), null);
  const w = new L.StageWindow();
  w.reset(0);
  for (let n = 0; n < 40; n++) w.reason('no_body');
  w.reason('hips_hidden'); w.reason(null); w.reason('nonsense');
  for (let n = 0; n < 6; n++) w.calibration({cause: n ? 'frame' : 'recentre"<x>', width: 640, height: 480});
  const report = w.report(2000, 1);
  assert.equal(report.reasons.no_body, 40);
  assert.equal(report.reasons.hips_hidden, 1);
  assert.equal(report.restarts, 'recentrex 640x480, frame 640x480, frame 640x480, frame 640x480');  // 4 at most, cleaned
  const json = JSON.stringify(report);
  assert.ok(json.length < 1024, 'report too long: ' + json.length);
  fs.writeFileSync(path.join(work, 'report.json'), json);
  const next = w.report(4000, 0);
  assert.equal(next.reasons.no_body, 0);
  assert.equal(next.restarts, '');
});
check('(c) the page uses the title\'s tracker, the reasons and the restart causes', () => {
  const page = between(pageHeader, 'R"KINECTPAGE(', ')KINECTPAGE"');
  assert.ok(page.includes("from './kinect_tracker.mjs?v=kinect106';"));
  // Fix 107 imports RecentreState from the same module (a newer ?v).
  assert.ok(/frameReason(, RecentreState)?\} from '\.\/latency\.mjs\?v=10[67]';/.test(page));
  assert.ok(page.includes('latencyWindow.reason(frameReason(stale, seen, latest, tracked));'));
  assert.ok(page.includes('for (const event of tracker.takeEvents()) latencyWindow.calibration(event);'));
  for (const cause of ['mode', 'recentre', 'camera']) assert.ok(page.includes("tracker.resetCalibration('" + cause + "')"), cause);
  assert.ok(!/tracker\.resetCalibration\(\)/.test(page), 'a reset without its cause');
  assert.ok(page.includes("s.reason === 'hips_hidden' || s.reason === 'hips_inferred' ? T('hips_hidden')"));
});

fs.rmSync(work, {recursive: true, force: true});
if (failures) { console.log(failures + ' failure(s)'); process.exit(1); }
console.log('check-kinect-tracker.mjs: all passed');
