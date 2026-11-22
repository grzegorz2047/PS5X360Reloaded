// SPDX-License-Identifier: MIT
// Fix 72 (tools/source-1.0.2/fixes/72-phone-page-version.patch): an open phone camera page reloads
// once when the app on the console was updated under it. On 1.0.4_candidate_28 a phone tab opened
// under an older build reconnected by itself (fix 56) and kept running its old script, so fix 66's
// delay lines never appeared.
// node tools/check-page-version.mjs <assembled tree>
//  (a) port/phone_camera_page.hpp: kPageVersion is the first 16 hex digits of the SHA-256 of the
//      page, pairing.mjs and latency.mjs as served; the page holds __PAGE_VERSION__ once;
//  (b) reloadIfUpdated as the page has it: no reload without a version, for its own version, or
//      when the page was not given one; one reload for another version, keeping a typed key; not
//      again for the same version (no loop); the status poll calls it with the answer's header;
//      a kept key is restored at load; the page's script still parses.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';

const tree = process.argv[2];
if (!tree) throw Error('usage: node check-page-version.mjs <tree>');
const header = fs.readFileSync(path.join(tree, 'port/phone_camera_page.hpp'), 'utf8');
const between = (open, close) => {
  const a = header.indexOf(open), b = header.indexOf(close, a);
  assert.ok(a >= 0 && b > a, 'found ' + open);
  return header.slice(a + open.length, b);
};
const page = between('R"KINECTPAGE(', ')KINECTPAGE"');
const pairing = between('R"PAIRINGJS(', ')PAIRINGJS"');
const latency = between('R"LATENCYJS(', ')LATENCYJS"');

// (a)
const version = crypto.createHash('sha256').update(page).update(pairing).update(latency).digest('hex').slice(0, 16);
const declared = /inline constexpr char kPageVersion\[\] = "([0-9a-f]{16})";/.exec(header);
assert.ok(declared, 'kPageVersion declared');
assert.equal(declared[1], version, 'kPageVersion is the hash of what is served');
assert.equal((page.match(/__PAGE_VERSION__/g) || []).length, 1, 'the page holds its placeholder once');

// (b)
const begin = page.indexOf('// fix 72 begin'), end = page.indexOf('// fix 72 end');
assert.ok(begin >= 0 && end > begin, 'the reload code');
const make = served_as => new Function(page.slice(begin, end).replace('__PAGE_VERSION__', served_as) +
  '\nreturn reloadIfUpdated;')();
const store = () => {
  const m = new Map();
  return {getItem: k => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)), m};
};
{
  const reloadIfUpdated = make('v1');
  const s = store();
  let reloads = 0;
  const reload = () => ++reloads;
  assert.equal(reloadIfUpdated(null, 'abc', s, reload), false, 'no version: nothing');
  assert.equal(reloadIfUpdated('v1', 'abc', s, reload), false, 'its own version: nothing');
  assert.equal(reloadIfUpdated('v2', 'abc', s, reload), true, 'another version: a reload');
  assert.equal(reloads, 1);
  assert.equal(s.getItem('ps5x360-key'), 'abc', 'the typed key is kept');
  assert.equal(reloadIfUpdated('v2', 'abc', s, reload), false, 'not again for the same version');
  assert.equal(reloads, 1);
  assert.equal(reloadIfUpdated('v3', '', s, reload), true, 'a newer version again, once');
  assert.equal(reloads, 2);
}
{
  const reloadIfUpdated = make('__PAGE_VERSION__');
  let reloads = 0;
  assert.equal(reloadIfUpdated('v2', 'abc', store(), () => ++reloads), false, 'a page given no version never reloads');
  assert.equal(reloads, 0);
}
assert.ok(page.includes("if (reloadIfUpdated(response.headers.get('X-Page-Version'), typed, sessionStorage, () => location.reload())) return;"),
  'the status poll passes the answer\'s version');
assert.ok(page.includes("const kept = sessionStorage.getItem('ps5x360-key');") &&
  page.includes("if (kept && !$('key').value.trim()) $('key').value = kept;"), 'a kept key is restored at load');
const script = page.slice(page.indexOf('<script type="module">') + 22, page.indexOf('</script>', page.indexOf('<script type="module">')));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'page-version-'));
const scriptPath = path.join(dir, 'page.mjs');
fs.writeFileSync(scriptPath, script);
execFileSync(process.execPath, ['--check', scriptPath]);
fs.rmSync(dir, {recursive: true, force: true});
console.log('check-page-version: ok');
