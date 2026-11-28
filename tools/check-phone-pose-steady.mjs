// SPDX-License-Identifier: MIT
// Fix 169 (tools/source-1.0.2/fixes/169-phone-pose-steady.patch; issue 74): the phone camera page
// sends its pose frames steadily, and one failed request no longer makes it re-pair.
//   node tools/check-phone-pose-steady.mjs <assembled tree>
//  (a) pairing.mjs: PoseSender (slots, the newest waiting frame, a waiting loss first, the counts),
//      LinkHealth (lost only after 3 unanswered in a row and 2.5 s without an answer), keepsSession;
//  (b) a model of the issue's link (20 frames a second detected; most answers in ~40-80 ms, one in
//      five stalled 0.5-2 s: a median of ~65 ms and a mean of ~300 ms, as the reports implied):
//      the 1.0.6 page (2 slots, a frame dropped when both are busy; ~6 frames a second, as
//      reported) against 169's (4 slots, the newest waiting, a stalled one given up after 600 ms);
//      and a link with one stall in twenty;
//  (c) the page's own sendPose, run with a fake fetch and clock: a stalled POST is given up and the
//      other slots carry on; the newest waiting frame goes, older ones are replaced; a loss goes
//      first; one failed request is not 'lost', a console that stops answering is; a 409 replay is
//      an answer;
//  (d) the page: it imports the new parts, polls the status with a timeout and tolerates one
//      failed poll while paired, keeps its session when it comes back with the same key, sends the
//      counts in its report only when it posts it; the report still fits the console's limit; the
//      script still parses.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {spawnSync} from 'node:child_process';

const tree = process.argv[2];
if (!tree) throw Error('usage: node check-phone-pose-steady.mjs <tree>');
const header = fs.readFileSync(path.join(tree, 'port/phone_camera_page.hpp'), 'utf8');
const between = (text, open, close) => {
  const a = text.indexOf(open), b = text.indexOf(close, a);
  assert.ok(a >= 0 && b > a, 'found ' + open);
  return text.slice(a + open.length, b);
};
const page = between(header, 'R"KINECTPAGE(', ')KINECTPAGE"');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'phone-pose-steady-'));
fs.writeFileSync(path.join(work, 'pairing.mjs'), between(header, 'R"PAIRINGJS(', ')PAIRINGJS"'));
fs.writeFileSync(path.join(work, 'latency.mjs'), between(header, 'R"LATENCYJS(', ')LATENCYJS"'));
const P = await import(pathToFileURL(path.join(work, 'pairing.mjs')).href);
const L = await import(pathToFileURL(path.join(work, 'latency.mjs')).href);

let failures = 0;
const check = async (what, fn) => {
  try { await fn(); } catch (e) { failures++; console.log('FAIL: ' + what + ': ' + (e.stack || e.message)); }
};

// (a) ------------------------------------------------------------------------------------------
await check('the constants', () => {
  assert.equal(P.POSE_SLOTS, 4);
  assert.equal(P.POSE_SLOTS_WITH_PICTURES, 2);
  assert.equal(P.FRAME_TIMEOUT_MS, 600);
  assert.equal(P.STATUS_TIMEOUT_MS, 5000);
  assert.equal(P.LOST_AFTER_FAILURES, 3);
  assert.equal(P.LOST_AFTER_MS, 2500);
  assert.equal(P.KEEP_SESSION_MS, 30000);
  // The old pure answers are unchanged (check-kinect-page.mjs tests them; the page decides when).
  assert.equal(P.afterFrame('paired', 0, ''), 'lost');
});
await check('PoseSender', () => {
  const s = new P.PoseSender(3);
  const a = {pose: {n: 1}}, b = {pose: {n: 2}}, c = {pose: {n: 3}}, d = {pose: {n: 4}}, e = {pose: {n: 5}};
  assert.ok(s.offer(a) && s.offer(b) && s.offer(c), 'three on their way');
  assert.equal(s.inFlight, 3);
  assert.equal(s.offer(d), false, 'the fourth waits');
  assert.equal(s.offer(e), false);
  assert.equal(s.done('answered', 40), e, 'the newest goes when a slot frees');
  assert.equal(s.inFlight, 2, 'done frees the slot; the next is offered again');
  assert.ok(s.offer(e));
  assert.equal(s.done('timeout', 800), null, 'nothing waits');
  const lossOld = {pose: null, target: {session: 1}};
  s.offer({pose: {n: 6}});
  assert.equal(s.offer({pose: {n: 7}}), false);
  assert.equal(s.offer(lossOld), false, 'a loss waits too');
  assert.equal(s.offer({pose: {n: 8}}), false);
  assert.equal(s.done('failed', 1200), lossOld, 'the loss first');
  assert.equal(s.done('answered', 30).pose.n, 8, 'then the newest pose');
  assert.deepEqual(s.take(), [5, 2, 1, 1, 2, 1200], '[sent, answered, failed, timed out, replaced, slowest]');
  assert.deepEqual(s.take(), [0, 0, 0, 0, 0, 0], 'only since the last take');
  const two = new P.PoseSender(2);
  assert.ok(two.offer(a) && two.offer(b) && !two.offer(c));
  two.slots = 3;  // the page sets it before each offer
  assert.ok(two.offer(d));
  assert.equal(two.done('answered', 1), c, 'the waiting frame');
  assert.equal(two.done('answered', 1), null);
  two.done('answered', 1); two.done('answered', 1); two.done('answered', 1);
  assert.equal(two.inFlight, 0, 'never below zero');
  assert.equal(new P.PoseSender().slots, P.POSE_SLOTS);
});
await check('LinkHealth', () => {
  const h = new P.LinkHealth();
  h.paired(1000);
  assert.equal(h.failure(1100), false, 'one failure');
  assert.equal(h.failure(1200), false);
  assert.equal(h.failure(1300), false, 'three in a row, but answered 0.3 s ago');
  h.answer(1400);
  for (let t = 1500; t < 3900; t += 100) assert.equal(h.failure(t), false, 'under 2.5 s since the answer at ' + t);
  assert.equal(h.failure(3900), true, 'lost: 2.5 s without an answer and many unanswered');
  assert.equal(h.failure(3950), false, 'counted once; the count starts again');
  const once = new P.LinkHealth();
  once.paired(0);
  assert.equal(once.failure(5000), false, 'a single failure long after the last answer is not lost');
  assert.equal(once.failure(5001), false);
  assert.equal(once.failure(5002), true);
  once.statusFailed++;
  once.resumed++;
  assert.deepEqual(once.take(), [1, 1, 1], '[status failed, lost, resumed]');
  assert.deepEqual(once.take(), [0, 0, 0]);
});
await check('keepsSession', () => {
  assert.equal(P.keepsSession('lost', 'abc123', 'abc123', 77, 1000, 5000), true);
  assert.equal(P.keepsSession('lost', 'abc123', 'abc123', 77, 1000, 31000), true, 'at 30 s');
  assert.equal(P.keepsSession('lost', 'abc123', 'abc123', 77, 1000, 31001), false, 'later: a new session');
  assert.equal(P.keepsSession('lost', 'abc123', 'def456', 77, 1000, 2000), false, 'another key: the app restarted');
  assert.equal(P.keepsSession('connecting', 'abc123', 'abc123', 77, 1000, 2000), false, 'only from lost');
  assert.equal(P.keepsSession('waiting_setting', 'abc123', 'abc123', 77, 1000, 2000), false);
  assert.equal(P.keepsSession('lost', 'abc123', 'abc123', 0, 1000, 2000), false, 'no session yet');
  assert.equal(P.keepsSession('lost', '', '', 77, 1000, 2000), false, 'no key yet');
});

// (b) ------------------------------------------------------------------------------------------
// One seeded run of the issue's link, the old page against the new one.
function random(seed) {
  let x = seed >>> 0;
  return () => { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x / 4294967296; };
}
function roundTrip(next, stalls) {
  // Most answers in 40-80 ms (the page's median was 56 ms); `stalls` of them stall 0.5-2 s.
  return next() < stalls ? 500 + next() * 1500 : 40 + next() * 40;
}
function simulate(model, stalls, seconds = 120) {
  const next = random(169);
  const end = seconds * 1000;
  const events = [];  // [time, kind, data]
  const push = (t, kind, data) => { events.push([t, kind, data]); events.sort((a, b) => a[0] - b[0]); };
  for (let t = 0; t < end; t += 50) push(t, 'detect', null);  // 20 frames a second
  const delivered = [];
  let inFlight = 0, waiting = null, handshake = 0;
  const sender = model === 'new' ? new P.PoseSender(P.POSE_SLOTS) : null;
  const send = (t, frame) => {
    const rtt = roundTrip(next, stalls) + (handshake > 0 ? (handshake--, 150) : 0);
    if (model === 'new' && rtt > P.FRAME_TIMEOUT_MS) { push(t + P.FRAME_TIMEOUT_MS, 'timeout', frame); return; }
    push(t + rtt / 2, 'arrive', frame);
    push(t + rtt, 'answer', frame);
  };
  while (events.length) {
    const [t, kind, frame] = events.shift();
    if (t > end + 3000) break;
    if (kind === 'detect') {
      if (t >= end) continue;
      const item = {pose: {at: t}};
      if (model === 'old') {
        if (inFlight >= 2) continue;  // dropped
        inFlight++;
        send(t, item);
      } else if (sender.offer(item)) send(t, item);
    } else if (kind === 'arrive') {
      delivered.push(t);
    } else if (kind === 'answer' || kind === 'timeout') {
      if (kind === 'timeout') handshake++;  // its connection is closed: the next one opens a new
      if (model === 'old') { inFlight--; continue; }
      const queued = sender.done(kind === 'answer' ? 'answered' : 'timeout', 0);
      if (queued && sender.offer(queued)) send(t, queued);
    }
  }
  delivered.sort((a, b) => a - b);
  let worst = 0, over = 0;
  for (let n = 1; n < delivered.length; n++) {
    const gap = delivered[n] - delivered[n - 1];
    worst = Math.max(worst, gap);
    if (gap >= 500) over++;
  }
  return {perSecond: delivered.length / seconds, worst, over};
}
const say = (name, r) => `${name} ${r.perSecond.toFixed(1)} frames/s, worst gap ${Math.round(r.worst)} ms, ` +
                         `${r.over} gaps of 500 ms or more in 2 min`;
await check('the issue\'s link, old page against new', () => {
  const old = simulate('old', 0.2), now = simulate('new', 0.2);
  console.log(`  one stall in five: ${say('1.0.6 page', old)}; ${say('fix 169', now)}`);
  assert.ok(old.perSecond >= 5 && old.perSecond <= 7.5, 'the model shows the reported ~6 a second (' + old.perSecond + ')');
  assert.ok(now.perSecond >= 2 * old.perSecond, 'at least twice the frames (' + now.perSecond + ')');
  assert.ok(now.over * 4 <= old.over, 'a quarter of the gaps that drop the body, or fewer');
  assert.ok(now.worst < old.worst);
});
await check('a link with one stall in twenty', () => {
  const old = simulate('old', 0.05), now = simulate('new', 0.05);
  console.log(`  one stall in twenty: ${say('1.0.6 page', old)}; ${say('fix 169', now)}`);
  assert.ok(now.perSecond >= 18.5, 'nearly every detected frame arrives (' + now.perSecond + ')');
  assert.equal(now.over, 0, 'no gap that drops the body');
});

// (c) ------------------------------------------------------------------------------------------
// The page's sendPose, as served, with a fake fetch and clock.
const sendPoseSource = between(page, '\nasync function sendPose(', '\nasync function loadModel(');
function harness() {
  let clock = 0;
  const timers = [];
  const requests = [];
  const log = [];
  const fakeSetTimeout = (fn, ms) => { const t = {at: clock + ms, fn, live: true}; timers.push(t); return t; };
  const fakeClearTimeout = t => { if (t) t.live = false; };
  // Each request waits for the test to answer it (answer / fail), or for its signal.
  const fetch = (url, options) => new Promise((resolve, reject) => {
    const request = {url, options, frame: options.body, settled: false};
    request.answer = (status = 200, json = {ok: true}) => {
      if (request.settled) return; request.settled = true;
      resolve({ok: status >= 200 && status < 300, status, json: async () => json});
    };
    request.fail = () => { if (request.settled) return; request.settled = true; reject(new TypeError('Failed to fetch')); };
    options.signal?.addEventListener('abort', () => {
      if (request.settled) return; request.settled = true;
      request.aborted = true;
      reject(new DOMException('The operation was aborted.', 'AbortError'));
    });
    requests.push(request);
  });
  const make = new Function('fetch', 'setTimeout', 'clearTimeout', 'performance', 'P', 'log', `
    let paired = true, motionSession = {session: 5, sequence: 0}, lastNetworkMs = 0;
    const state = {pairing: 'paired'};
    const sentTimes = [], onConsole = true, consoleKey = 'abc123';
    const pictureChoice = {value: 'off'}, pictures = {wanted: false, note() {}};
    const latencyWindow = {add() {}}, recentring = {note() {}};
    const sendPicture = () => {};
    const {PoseSender, LinkHealth, afterFrame, POSE_SLOTS, POSE_SLOTS_WITH_PICTURES, FRAME_TIMEOUT_MS} = P;
    const poses = new PoseSender(), consoleLink = new LinkHealth();
    consoleLink.paired(performance.now());
    const packFrame = (session, sequence, tracked, joints) => ({session, sequence, tracked, id: joints[0].x});
    const setPairing = next => { log.push([performance.now(), next]); state.pairing = next; if (next !== 'paired') paired = false; };
    async function sendPose(${sendPoseSource}
    return {sendPose, poses, consoleLink, state, motionSession, pictureChoice, pictures};
  `);
  const page = make(fetch, fakeSetTimeout, fakeClearTimeout, {now: () => clock}, P, log);
  const settle = async () => { for (let n = 0; n < 20; n++) await new Promise(r => setImmediate(r)); };
  const advance = async ms => {
    const end = clock + ms;
    for (;;) {
      const due = timers.filter(t => t.live && t.at <= end).sort((a, b) => a.at - b.at)[0];
      if (!due) break;
      clock = due.at; due.live = false; due.fn();
      await settle();
    }
    clock = end;
    await settle();
  };
  const pose = id => ({joints: Array.from({length: 20}, (_, n) => ({x: n ? 0 : id, y: 0, z: 0, confidence: 1}))});
  return {page, requests, log, advance, settle, pose, now: () => clock};
}
await check('a stalled POST is given up; the other slots carry on', async () => {
  const h = harness();
  let answered = 0;
  for (let n = 0; n < 40; n++) {
    void h.page.sendPose(h.pose(n));
    await h.settle();
    // Answer every request but the first after 30 ms; the first never answers.
    await h.advance(30);
    for (const r of h.requests.slice(1)) if (!r.settled) { r.answer(); answered++; }
    await h.settle();
    await h.advance(20);
  }
  assert.ok(h.requests[0].aborted, 'the stalled frame was given up');
  assert.ok(answered >= 38, 'the rest went: ' + answered);
  assert.deepEqual(h.log, [], 'no lost console');
  const counts = h.page.poses.take();
  assert.equal(counts[3], 1, 'one given up');
  assert.ok(counts[5] >= 600 && counts[5] < 700, 'its time');
});
await check('the newest waiting frame goes; a loss first', async () => {
  const h = harness();
  for (let n = 1; n <= 4; n++) void h.page.sendPose(h.pose(n));
  await h.settle();
  assert.equal(h.requests.length, 4, 'four slots');
  for (let n = 5; n <= 8; n++) void h.page.sendPose(h.pose(n));
  await h.settle();
  assert.equal(h.requests.length, 4, 'the rest wait');
  h.requests[0].answer();
  await h.settle();
  assert.equal(h.requests.length, 5);
  assert.equal(h.requests[4].frame.id, 8, 'the newest went');
  assert.ok(h.requests[4].frame.sequence > h.requests[3].frame.sequence, 'its sequence after the ones before');
  void h.page.sendPose(h.pose(9));
  void h.page.sendPose(null);  // the camera stopped: a loss
  await h.settle();
  h.requests[1].answer();
  await h.settle();
  assert.equal(h.requests[5].frame.tracked, false, 'the loss went');
  h.requests[2].answer();
  await h.settle();
  assert.equal(h.requests.length, 6, 'the pose before the loss is not sent after it');
  assert.equal(h.page.poses.take()[4], 3, 'three replaced');
});
await check('one failed request is not lost; a console that stops answering is', async () => {
  const h = harness();
  void h.page.sendPose(h.pose(1));
  await h.settle();
  h.requests[0].fail();
  await h.settle();
  assert.deepEqual(h.log, [], 'one failure: still paired');
  void h.page.sendPose(h.pose(2));
  await h.settle();
  h.requests[1].answer();
  await h.settle();
  // Now every request fails at once (the console went away).
  const answeredAt = h.now();
  for (let n = 3; n < 200 && !h.log.length; n++) {
    await h.advance(50);
    void h.page.sendPose(h.pose(n));
    await h.settle();
    h.requests.at(-1).fail();
    await h.settle();
  }
  assert.equal(h.log.length, 1);
  assert.equal(h.log[0][1], 'lost');
  assert.ok(h.log[0][0] - answeredAt >= P.LOST_AFTER_MS && h.log[0][0] - answeredAt < P.LOST_AFTER_MS + 100,
            'lost 2.5 s after the last answer: ' + (h.log[0][0] - answeredAt));
  assert.equal(h.page.consoleLink.take()[1], 1, 'counted');
});
await check('a 409 replay is an answer; 403 and disabled still move the state', async () => {
  const h = harness();
  void h.page.sendPose(h.pose(1));
  await h.settle();
  h.requests[0].answer(409, {error: 'replay'});
  await h.settle();
  assert.deepEqual(h.log, []);
  assert.equal(h.page.poses.take()[1], 1, 'answered');
  void h.page.sendPose(h.pose(2));
  await h.settle();
  h.requests[1].answer(403, {error: 'key'});
  await h.settle();
  assert.deepEqual(h.log.map(e => e[1]), ['wrong_key'], 'a new key at once');
});
await check('with camera pictures on, two slots', async () => {
  const h = harness();
  h.page.pictureChoice.value = 'on';
  h.page.pictures.wanted = true;
  for (let n = 1; n <= 4; n++) void h.page.sendPose(h.pose(n));
  await h.settle();
  assert.equal(h.requests.length, 2);
});

// (d) ------------------------------------------------------------------------------------------
await check('the page', () => {
  assert.ok(page.includes("import {PoseSender, LinkHealth, keepsSession, POSE_SLOTS, POSE_SLOTS_WITH_PICTURES, FRAME_TIMEOUT_MS,\n" +
                          "  STATUS_TIMEOUT_MS} from './pairing.mjs?v=56';  // fix 169"));
  assert.ok(page.includes('const poses = new PoseSender(), consoleLink = new LinkHealth();'));
  assert.ok(!/\binFlight >= 2\b/.test(page) && !page.includes('pendingLost'), 'the old two-slot drop is gone');
  // The status poll: a timeout, one failure tolerated while paired, any answer counts.
  assert.ok(page.includes("const abort = new AbortController(), timer = setTimeout(() => abort.abort(), STATUS_TIMEOUT_MS);"));
  assert.ok(page.includes("await fetch('/api/motion/status', {headers: {'X-Key': typed}, cache: 'no-store', signal: abort.signal});"));
  const ask = between(page, 'async function ask(quiet) {', '\n}\n');
  const tolerate = ask.indexOf("  if (http && http !== 502) consoleLink.answer(performance.now());\n" +
                               "  else if (state.pairing === 'paired') {\n" +
                               "    consoleLink.statusFailed++;\n" +
                               "    if (!consoleLink.failure(performance.now())) return;\n  }");
  const decide = ask.indexOf('const next = afterStatus(state.pairing, http, json);');
  assert.ok(tolerate > 0 && decide > tolerate, 'one failed poll while paired is tolerated before the state moves');
  assert.ok(ask.indexOf('asking = false;') < tolerate, 'the next poll can run');
  assert.ok(ask.includes('if (keepsSession(state.pairing, consoleKey, typed, motionSession.session, lostAt, performance.now())) {'));
  assert.ok(ask.includes('consoleLink.paired(performance.now());'));
  assert.ok(page.includes("if (next === 'lost' && state.pairing !== 'lost') lostAt = performance.now();  // fix 169"));
  // The counts go only with a report that is posted.
  const report = between(page, 'function reportLatency(now) {', '\n}\n');
  const post = report.indexOf('if (onConsole && paired && consoleKey) {');
  const counts = report.indexOf('report.link = [...poses.take(), ...consoleLink.take()];');
  const fetched = report.indexOf("fetch('/api/motion/latency'");
  assert.ok(post > 0 && post < counts && counts < fetched);
  // The frame POST has the abort signal, on the console and through the relay.
  assert.equal((sendPoseSource.match(/signal: abort\.signal/g) || []).length, 2);
  const script = between(page, '<script type="module">', '</script>');
  fs.writeFileSync(path.join(work, 'page.mjs'), script);
  const parsed = spawnSync(process.execPath, ['--check', path.join(work, 'page.mjs')], {encoding: 'utf8'});
  assert.equal(parsed.status, 0, parsed.stderr);
});
await check('the report still fits the console\'s limit', () => {
  const w = new L.StageWindow();
  w.reset(0);
  for (let n = 0; n < 60; n++) { w.frame(n * 33); for (const s of L.STAGES) w.add(s, 12345.6); w.reason('hips_hidden'); }
  for (let n = 0; n < 4; n++) w.calibration({cause: 'recentre_long_name', width: 1920, height: 1080});
  const report = w.report(2000, 3);
  Object.assign(report, {recentred: 99999, picture_on: 1, picture_sent: 99999, picture_skipped: 99999, picture_failed: 99999,
                         picture_wanted: 1, picture_capture_ms: 99999, picture_pack_ms: 99999, picture_upload_ms: 99999});
  report.link = [99999, 99999, 99999, 99999, 99999, 99999, 99999, 99999, 99999];
  assert.ok(JSON.stringify(report).length < 2048, 'size ' + JSON.stringify(report).length);
});

fs.rmSync(work, {recursive: true, force: true});
if (failures) { console.log(`check-phone-pose-steady: ${failures} failure(s)`); process.exit(1); }
console.log('check-phone-pose-steady: ok');
