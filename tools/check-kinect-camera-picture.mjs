// SPDX-License-Identifier: MIT
// Fix 119 (tools/source-1.0.2/fixes/119-kinect-colour-camera.patch): the phone page's camera
// picture for the game. The page (port/phone_camera_page.hpp) sends a small picture of its camera
// to the console only with its own switch on and the console's setting on.
//   node tools/check-kinect-camera-picture.mjs <assembled tree>
//  (a) packPicture: "XPC1", width, height, sequence, then full-range YCbCr 4:2:0 (red, white,
//      black, a 2x2 block averaged), 28816 bytes for 160x120, as the console's ParsePicture takes;
//  (b) pictureCrop: the middle 4:3 of landscape, portrait and 4:3 videos, nothing without a size;
//  (c) PictureGate: nothing until the console wants it ("picture":1 in an answer), nor with the
//      switch off, while one is on its way, sooner than 100 ms, while 2 pose frames wait or half a
//      round trip is over 150 ms (counted as skipped); a 409 stops it until the console asks again;
//  (d) the page: the switch is off by default and remembered, its notice follows it (off, on, on
//      here but off on the PS5) in the page's four languages, the texts that said no video leaves
//      the phone name the switch, the picture is posted to /api/motion/picture with the key only
//      from the console's page, after a frame's answer, from an in-memory canvas.
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
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'check-kinect-camera-picture-'));
fs.writeFileSync(path.join(work, 'latency.mjs'), between(header, 'R"LATENCYJS(', ')LATENCYJS"'));
const L = await import(pathToFileURL(path.join(work, 'latency.mjs')).href);

const solid = (w, h, r, g, b) => {
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let n = 0; n < w * h; n++) rgba.set([r, g, b, 255], 4 * n);
  return rgba;
};
check('(a) the picture as the console takes it', () => {
  assert.equal(L.PICTURE_WIDTH, 160);
  assert.equal(L.PICTURE_HEIGHT, 120);
  const bytes = new Uint8Array(L.packPicture(solid(160, 120, 255, 0, 0), 160, 120, 0x01020304));
  assert.equal(bytes.length, 16 + 160 * 120 * 3 / 2);
  assert.equal(String.fromCharCode(...bytes.slice(0, 4)), 'XPC1');
  const view = new DataView(bytes.buffer);
  assert.equal(view.getUint16(4, true), 160);
  assert.equal(view.getUint16(6, true), 120);
  assert.equal(view.getUint32(8, true), 0x01020304);
  assert.equal(view.getUint32(12, true), 0);
  const luma = 160 * 120, chroma = luma / 4;
  assert.equal(bytes[16], 76);                       // red: Y 76, Cb 85, Cr 255
  assert.equal(bytes[16 + luma], 85);
  assert.equal(bytes[16 + luma + chroma], 255);
  assert.equal(bytes[16 + luma - 1], 76);
  assert.equal(bytes[bytes.length - 1], 255);
  const white = new Uint8Array(L.packPicture(solid(16, 16, 255, 255, 255), 16, 16, 1));
  assert.deepEqual([white[16], white[16 + 256], white[16 + 256 + 64]], [255, 128, 128]);
  const black = new Uint8Array(L.packPicture(solid(16, 16, 0, 0, 0), 16, 16, 1));
  assert.deepEqual([black[16], black[16 + 256], black[16 + 256 + 64]], [0, 128, 128]);
  // A 2x2 block of white and black on the left, red on the right: Cb and Cr average the block.
  const mixed = solid(4, 2, 0, 0, 0);
  mixed.set([255, 255, 255, 255], 0); mixed.set([255, 255, 255, 255], 4 * 5);
  mixed.set([255, 0, 0, 255], 8); mixed.set([255, 0, 0, 255], 12); mixed.set([255, 0, 0, 255], 24); mixed.set([255, 0, 0, 255], 28);
  const m = new Uint8Array(L.packPicture(mixed, 4, 2, 1));
  assert.deepEqual([...m.slice(16, 24)], [255, 0, 76, 76, 0, 255, 76, 76]);
  assert.deepEqual([m[24], m[25], m[26], m[27]], [128, 85, 128, 255]);
  assert.equal(L.packPicture(solid(4, 2, 1, 2, 3), 4, 2, 2 ** 32 + 5).byteLength, 16 + 8 + 4);
  assert.equal(new DataView(L.packPicture(solid(4, 2, 1, 2, 3), 4, 2, 2 ** 32 + 5)).getUint32(8, true), 5);
});
check('(b) the middle 4:3 of the camera', () => {
  assert.deepEqual(L.pictureCrop(1280, 720), {x: 160, y: 0, width: 960, height: 720});
  assert.deepEqual(L.pictureCrop(720, 1280), {x: 0, y: 370, width: 720, height: 540});
  assert.deepEqual(L.pictureCrop(640, 480), {x: 0, y: 0, width: 640, height: 480});
  assert.equal(L.pictureCrop(0, 0), null);
  assert.equal(L.pictureCrop(undefined, 480), null);
});
check('(c) when a picture goes', () => {
  // due(now, switch, half a pose frame's round trip). Fix 119f: the network's delay alone; frames in
  // flight and dropped pose frames no longer hold pictures back.
  // Fix 119g: 8 a second over 60 ms (125 ms), two on their way at most. Fix 119i: 20 ms apart at
  // most; the page sends one picture per new camera frame, so 30 a second at most in practice.
  const g = new L.PictureGate();
  assert.equal(L.PICTURE_MS, 20);
  assert.equal(L.PICTURE_SLOW_NETWORK_MS, 60);
  assert.equal(L.PICTURE_SLOW_MS, 125);
  assert.equal(L.PICTURE_IN_FLIGHT, 3);  // fix 119k
  assert.equal(g.inFlight, 0);
  assert.equal(g.due(1000, true, 10), false);            // the console has not asked
  g.note({ok: true});                                    // an older console: no "picture"
  assert.equal(g.wanted, false);
  g.note({ok: true, picture: 1});
  assert.equal(g.due(1000, false, 10), false);           // the page's switch off
  assert.equal(g.due(1000, true, 10), true);
  assert.equal(g.due(1015, true, 10), false);            // under 20 ms
  g.inFlight = 1;
  assert.equal(g.due(1100, true, 10), true);             // one on its way: a second may go
  g.inFlight = 2;
  assert.equal(g.due(1150, true, 10), true);             // fix 119k: two on their way, a third may go
  g.inFlight = 3;
  assert.equal(g.due(1200, true, 10), false);            // three on their way
  g.inFlight = 0;
  assert.equal(g.due(1300, true, 21), true);             // the console's 21 ms
  assert.equal(g.due(1400, true, 61), false);            // slower network: 125 ms apart
  assert.equal(g.skipped, 0);                            // not a skip, only the pace
  assert.equal(g.due(1425, true, 61), true);
  assert.equal(g.due(1600, true, 151), false);           // a slow network
  assert.equal(g.skipped, 1);
  assert.equal(g.due(1610, true, 10), false);            // and the next try 20 ms later
  assert.equal(g.due(1700, true, 150), false);           // 150 ms: 125 ms apart
  assert.equal(g.due(1725, true, 150), true);
  // The gate alone: 50 a second when tried every 10 ms; the page's tries come after pose answers
  // and only for a new camera frame (checked in (d)).
  let sent = 0;
  for (let t = 3000; t < 4000; t += 10) if (g.due(t, true, 21)) sent++;
  assert.equal(sent, 50);
  sent = 0;
  for (let t = 5000; t < 6000; t += 33) if (g.due(t, true, 21)) sent++;
  assert.equal(sent, 31);                                // a 30 fps camera's frames: each one
  // Fix 119i: the JPEG picture's bytes.
  const jpeg = new Uint8Array([0xFF, 0xD8, 1, 2, 3, 0xFF, 0xD9]);
  const packed = new Uint8Array(L.packJpegPicture(jpeg, 160, 120, 0x0A0B0C0D));
  assert.equal(packed.length, 16 + 7);
  assert.equal(String.fromCharCode(...packed.slice(0, 4)), 'XPJ1');
  const pv = new DataView(packed.buffer);
  assert.deepEqual([pv.getUint16(4, true), pv.getUint16(6, true), pv.getUint32(8, true), pv.getUint32(12, true)],
                   [160, 120, 0x0A0B0C0D, 7]);
  assert.deepEqual([...packed.slice(16)], [...jpeg]);
  assert.equal(L.PICTURE_JPEG_QUALITY, 0.7);
  assert.equal(g.jpeg, true);                            // tried first on every page
  // The times: averages since the last report, in whole ms; none timed, nothing.
  assert.deepEqual(g.takeTimes(), {});
  g.time(10, 2, 30);
  g.time(14, 3, 41);
  assert.deepEqual(g.takeTimes(), {picture_capture_ms: 12, picture_pack_ms: 3, picture_upload_ms: 36});
  assert.deepEqual(g.takeTimes(), {});
  assert.equal(g.due(1500, true, 10), false);            // (older times: not due)
  assert.equal(g.sending(1500), false);                  // none taken yet
  g.answer(204, 1600);
  assert.equal(g.sent, 1);
  assert.equal(g.sending(1600), true);                   // the page's "Sending" line
  assert.equal(g.sending(3599), true);
  assert.equal(g.sending(3600), false);                  // 2 s without one
  g.answer(400, 3700);                                   // refused: not sent, failed (fix 119d)
  assert.equal(g.sent, 1);
  assert.equal(g.failed, 1);
  g.answer(403, 3750);                                   // a wrong key
  assert.equal(g.failed, 2);
  assert.equal(g.wanted, true);
  g.answer(204, 3800);
  g.answer(409, 3900);                                   // the console's setting is off
  assert.equal(g.wanted, false);
  assert.equal(g.sending(3900), false);                  // the line goes at once
  assert.equal(g.due(5000, true, 10), false);            // not wanted after the 409
  g.note({picture: 0});
  assert.equal(g.wanted, false);
  g.note(null);
  g.note({picture: '1'});
  assert.equal(g.wanted, true);
  assert.equal(L.PICTURE_MAX_NETWORK_MS, 150);
});
check('(d) the page', () => {
  assert.ok(page.includes('<select id="picture"><option value="off" data-t="picture_off" selected></option><option value="on" data-t="picture_on"></option></select>'));
  assert.ok(page.includes('<p class="muted" id="picture_note" role="status"></p>'));
  assert.ok(page.includes("[sides, 'sides'], [pictureChoice, 'picture']])"));
  assert.ok(page.includes('sides: sides.value, picture: pictureChoice.value})'));
  assert.ok(page.includes("const pictureChoice = $('picture'), pictureNote = $('picture_note'), pictureLive = $('picture_live');  // fix 119"));
  assert.ok(page.indexOf("const pictureChoice = $('picture')") < page.indexOf('applyMotionSettings();'));  // remember() needs it
  assert.ok(page.includes("const text = pictureChoice.value !== 'on' ? T('picture_note_off')\n" +
                          "             : onConsole && paired && !pictures.wanted ? T('picture_note_console_off') : T('picture_note_on');"));
  assert.ok(page.includes("if (!onConsole || !paired || !consoleKey || demo || !stream || video.readyState < 2) return;"));
  // Fix 119f: the network's delay alone (119 counted the frame just answered as waiting; 119d's
  // dropped pose frames are the page's normal pacing at 45-50 fps and skipped over half of them).
  assert.ok(page.includes("if (!pictures.due(performance.now(), pictureChoice.value === 'on', lastNetworkMs)) return;"));
  assert.ok(page.includes('let lastNetworkMs = 0, lastPictureFrame = -1;'));
  assert.ok(page.includes('if (inFlight >= 2) { if (!pose) pendingLost = target; return; }'));  // as before 119
  assert.ok(!page.includes('lastPoseDrop'));
  assert.ok(!/pictures\.due\([^)]*inFlight/.test(page), 'frames in flight no longer hold pictures back');
  assert.ok(page.includes("  } catch {\n    pictures.failed++;  // fix 119d"));
  assert.ok(page.includes("await fetch('/api/motion/picture', {method: 'POST', headers: {'X-Key': consoleKey, 'Content-Type': 'application/octet-stream'},"));
  // Fix 119i: JPEG from the small canvas where the browser makes it, else the YCbCr picture (and
  // not JPEG again on this page); one picture per new camera frame.
  assert.ok(page.includes("pictureCanvas.toBlob(done, 'image/jpeg', PICTURE_JPEG_QUALITY);"));
  assert.ok(page.includes("if (jpeg && jpeg.type === 'image/jpeg') {\n      body = packJpegPicture(new Uint8Array(await jpeg.arrayBuffer()), PICTURE_WIDTH, PICTURE_HEIGHT, ++pictures.sequence);"));
  assert.ok(page.includes('pictures.jpeg = false;  // not again on this page'));
  assert.ok(page.includes('body = packPicture(pixels, PICTURE_WIDTH, PICTURE_HEIGHT, ++pictures.sequence);'));
  assert.ok(page.includes('if (video.currentTime === lastPictureFrame) return;'));
  assert.ok(page.indexOf('if (video.currentTime === lastPictureFrame) return;') < page.indexOf('if (!pictures.due(') &&
            page.indexOf('lastPictureFrame = video.currentTime;') > page.indexOf('if (!pictures.due('));
  assert.ok(page.includes("import {packJpegPicture, PICTURE_JPEG_QUALITY} from './latency.mjs?v=119';  // fix 119i"));
  // Fix 119g: two on their way at most, each stage timed, the averages in the 2 s report.
  assert.ok(page.includes('pictures.inFlight++;  // fix 119g') && page.includes('    pictures.inFlight--;\n'));
  assert.ok(page.includes('pictures.time(captured - started, packed - captured, answered - packed);'));
  assert.ok(page.indexOf('const captured = performance.now();') > page.indexOf('pictureContext.drawImage(video, crop.x, crop.y') &&
            page.indexOf('const answered = performance.now();') > page.indexOf("await fetch('/api/motion/picture'"));
  assert.ok(page.includes('Object.assign(report, pictures.takeTimes());  // fix 119g'));
  assert.ok(page.includes("const pictureCanvas = document.createElement('canvas');"));  // not in the page, not kept
  assert.equal((page.match(/\/api\/motion\/picture/g) || []).length, 1);
  assert.ok(!/localStorage[^\n]*pixels|sessionStorage[^\n]*pixels/.test(page));
  // After a frame's answer, which carries "picture"; the status too.
  const answer = page.indexOf('pictures.note(answer);');
  assert.ok(answer > page.indexOf('lastNetworkMs = (performance.now() - sentAt) / 2;  // fix 119') && answer < page.indexOf('void sendPicture();'));
  assert.ok(page.includes('if (json) { pictures.note(json); showPictureNote(); }  // fix 119'));
  const TEXT = eval('(' + between(page, 'const TEXT = ', ';\nconst lang = ') + ')');
  assert.equal(Object.keys(TEXT).length, 4);
  for (const code of ['en', 'pt', 'es', 'pl']) {
    for (const key of ['picture_label', 'picture_off', 'picture_on', 'picture_note_off', 'picture_note_on', 'picture_note_console_off', 'picture_live'])
      assert.ok(typeof TEXT[code][key] === 'string' && TEXT[code][key].length > 3, code + ' ' + key);
    assert.ok(TEXT[code].picture_note_on.includes('160×120'), code);
    assert.ok(TEXT[code].picture_live.startsWith('● '), code);
    assert.ok(TEXT[code].intro.includes(TEXT[code].picture_label), code + ' intro names the switch');
    assert.ok(TEXT[code].picture_note_console_off.includes(TEXT[code].picture_label), code);
  }
  assert.equal(TEXT.en.picture_label, 'Camera picture for the game');  // the console setting's name
  // 1.0.5-work-fix119b: the latency report (every 2 s) tells the console the switch and counts.
  assert.ok(page.includes("  report.picture_on = pictureChoice.value === 'on' ? 1 : 0;\n" +
                          "  report.picture_sent = pictures.sent;\n  report.picture_skipped = pictures.skipped;\n" +
                          "  report.picture_failed = pictures.failed;"));
  assert.ok(page.includes('  report.picture_wanted = pictures.wanted ? 1 : 0;'));
  assert.ok(page.indexOf('report.picture_on =') > page.indexOf('report.recentred = recentring.done;') &&
            page.indexOf('report.picture_on =') < page.indexOf("fetch('/api/motion/latency'"));
  // While pictures go, a line under the camera picture, outside the (folded) options.
  assert.ok(page.includes('<p id="picture_live" role="status" hidden></p><p id="calibration" role="status"></p>'));
  assert.ok(page.indexOf('<p id="picture_live"') < page.indexOf('<details id="settings">'));
  assert.ok(page.includes("const sending = pictureChoice.value === 'on' && pictures.sending(performance.now());"));
  // 1.0.5-work-fix119c: the same line hints at the switch while the PS5 asks and it is off; a tap
  // opens the options at the switch and does not turn it on.
  assert.ok(page.includes("const hint = !sending && pictureChoice.value !== 'on' && onConsole && paired && pictures.wanted;"));
  assert.ok(page.includes("const live = sending ? T('picture_live') : hint ? T('picture_hint') : '';"));
  assert.ok(page.includes('pictureLive.hidden = !live;\n  pictureLive.classList.toggle(\'hint\', hint);'));
  const tap = between(page, 'pictureLive.onclick = () => {', '};');
  assert.ok(tap.includes("if (!pictureLive.classList.contains('hint')) return;") && tap.includes("$('settings').open = true;") &&
            tap.includes('pictureChoice.focus();'));
  assert.ok(!tap.includes('pictureChoice.value ='), 'the tap does not turn the picture on');
  assert.ok(page.includes('async function sendPicture() {\n  showPictureNote();'));
  assert.ok(page.includes('pictures.answer(response.status, answered);'));
  assert.ok(page.includes('#picture_live{color:var(--warn);font-weight:700;margin:6px 0}'));
  assert.ok(page.includes('#picture_live.hint{color:var(--text);font-weight:600;cursor:pointer;text-decoration:underline}'));
  for (const code of ['en', 'pt', 'es', 'pl']) {
    assert.ok(TEXT[code].picture_hint.includes(TEXT[code].picture_label), code + ' hint names the switch');
    assert.ok(TEXT[code].picture_hint.toLowerCase().includes(TEXT[code].options.toLowerCase().slice(0, 10)), code + ' and where it is');
  }
});

fs.rmSync(work, {recursive: true, force: true});
if (failures) { console.log(failures + ' failure(s)'); process.exit(1); }
console.log('check-kinect-camera-picture.mjs: all passed');
