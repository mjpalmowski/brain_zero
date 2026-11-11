import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {Blob} from 'node:buffer';
import vm from 'node:vm';

const header = readFileSync(new URL('../src/multi_page.h', import.meta.url), 'utf8');
const script = header.match(/<script>([\s\S]*)<\/script>/)[1];
const endpoint = 'https://brain-zero.com/api/compatibility';
const receipt = '0123456789abcdef0123456789abcdef';
const profile = {profile: {schema_firmware: 2623563, nominal_voltage: 48,
  settings: Array.from({length: 192}, () => [0, 0, 0, 0, 0, 0])}, grid: '0102'};
const data = {active: false, inspection: false, file: true, serial: 'PRIVATE-SERIAL', firmware: 2623563,
  setup: {status: 'ready', model: 'MultiPlus-II 48/5000/70-50 230 V', can_upload: true},
  settings: [[2, 2, 5750]], file_problem: '', progress: 100};
const upload = {state: 'empty', busy: false, check: {settings: [], assistants: false, done: false},
  read: {active: false}, bus: {master: true}};

function fixture(options = {}) {
  const elements = new Map(), calls = [], timers = new Map(), downloads = [];
  const settings = {...data, ...options.settings, setup: {...data.setup, ...options.settings?.setup}};
  const backup = new Blob([new Uint8Array([0x00, 0x82, 0xFE, 0xFF, 0x56, 0x45])], {type: 'application/octet-stream'});
  let nextTimer = 0;
  function element(id) {
    if (!elements.has(id)) elements.set(id, {id, value: '', textContent: '', innerHTML: '', disabled: false,
      hidden: false, listeners: {}, addEventListener(type, listener) {this.listeners[type] = listener;},
      querySelector() {return null;}, replaceChildren() {}, focus() {this.focused = true;}});
    return elements.get(id);
  }
  const response = (body, status = 200) => ({ok: status >= 200 && status < 300, status,
    async json() {return structuredClone(body);}, async text() {return JSON.stringify(body);}});
  const context = vm.createContext({console, AbortController, TypeError, TextEncoder,
    recordDownload(blob, name) {downloads.push({blob, name});},
    document: {getElementById: element, hidden: false, addEventListener() {}},
    window: {addEventListener() {}},
    setTimeout(callback, delay) {timers.set(++nextTimer, {callback, delay}); return nextTimer;},
    clearTimeout(id) {timers.delete(id);},
    async fetch(path, init = {}) {
      calls.push({path, init});
      if (path === '/api/settings') return response(settings);
      if (path === '/api/upload') return response(upload);
      if (path === '/api/settings/read') {
        if (options.readFailure) return response({ok: false, error: 'Read refused'}, 409);
        return response({ok: true});
      }
      if (path === '/api/compatibility.json') return response(options.profile ?? profile);
      if (path === '/api/settings.rvsc') return {ok: true, status: 200, async blob() {return backup;}};
      if (path === endpoint) return options.submit ? options.submit(init, response) :
        response({ok: true, status: 'queued', receipt}, 202);
      throw Error('Unexpected request: ' + path);
    },
  });
  vm.runInContext(script, context);
  vm.runInContext('save=(blob,name)=>recordDownload(blob,name);data=' + JSON.stringify(settings) +
    ';up=' + JSON.stringify(upload) + ';connected=true;render();', context);
  const click = () => element('submit').listeners.click();
  const clickDownload = () => element('download').listeners.click();
  const settle = async () => {for (let index = 0; index < 15; ++index) await new Promise(setImmediate);};
  return {context, element, calls, timers, downloads, backup, click, clickDownload, settle};
}

assert(!header.includes('window.open('), 'Submission must stay inside the app');
assert(!header.includes('postMessage('), 'No external-page handshake remains');
assert(!header.includes('/compatibility/"'), 'No external submission-page link remains');

{
  const f = fixture();
  assert.equal(f.element('submit-model').value, data.setup.model, 'Prefill the detected model');
  f.click(); await f.settle();
  const posted = f.calls.find(call => call.path === endpoint);
  assert(posted, 'Submission sends an HTTPS API request');
  const envelope = JSON.parse(posted.init.body);
  assert.deepEqual(envelope, {model: data.setup.model, consent: true, profile});
  assert(!posted.init.body.includes(data.serial), 'Serial number is excluded');
  assert(!posted.init.body.includes('5750'), 'Saved settings are excluded');
  assert.equal(posted.init.method, 'POST');
  assert.equal(posted.init.credentials, 'omit');
  assert.equal(posted.init.mode, 'cors');
  assert.equal(posted.init.redirect, 'error');
  assert.equal(posted.init.headers['Content-Type'], 'application/json');
  assert(f.calls.findIndex(call => call.path === '/api/settings/read') <
    f.calls.findIndex(call => call.path === '/api/compatibility.json'), 'Read a fresh profile first');
  assert.equal(f.element('submit-msg').className, 'notice good');
  assert(f.element('submit-msg').textContent.includes(receipt));
  assert.equal(f.element('submit').disabled, false);
  assert.equal(f.element('submit-model').disabled, false);
}

for (const invalidModel of ['  ', 'x'.repeat(161), 'invalid\x01model']) {
  const f = fixture(); f.element('submit-model').value = invalidModel;
  f.click(); await f.settle();
  assert.equal(f.calls.length, 0, 'Missing model cannot start a physical read or network upload');
  assert.equal(f.element('submit-msg').className, 'notice bad');
}

{
  const f = fixture({submit: () => ({status: 202, async text() {return '<html>Unavailable</html>';}})});
  f.click(); await f.settle();
  assert.equal(f.element('submit-msg').className, 'notice bad', 'Malformed JSON cannot claim success');
  assert.equal(f.element('submit').disabled, false);
}

{
  const f = fixture({readFailure: true});
  f.click(); await f.settle();
  assert(!f.calls.some(call => call.path === endpoint), 'Read failure never uploads stale profile');
  assert.equal(f.element('submit').disabled, false, 'Read failure allows retry');
}

{
  const f = fixture({profile: {...profile, serial: 'LEAK'}});
  f.click(); await f.settle();
  assert(!f.calls.some(call => call.path === endpoint), 'Reject unexpected raw top-level fields');
  assert.equal(f.element('submit').disabled, false);
}

for (const result of [
  {status: 200, body: {ok: true, status: 'queued', receipt}},
  {status: 202, body: {ok: true, status: 'queued', receipt: 'bad'}},
  {status: 202, body: {ok: true, status: 'completed', receipt}},
  {status: 429, body: {ok: false, error: 'Too many'}},
]) {
  const f = fixture({submit: (_init, response) => response(result.body, result.status)});
  f.click(); await f.settle();
  assert.equal(f.element('submit-msg').className, 'notice bad', 'Reject malformed or non-202 receipts');
  assert.equal(f.element('submit').disabled, false, 'Server rejection allows retry');
}

{
  let attempts = 0;
  const f = fixture({submit: (_init, response) => {
    if (++attempts === 1) throw new TypeError('Network failed');
    return response({ok: true, status: 'queued', receipt}, 202);
  }});
  f.click(); await f.settle();
  assert.equal(f.element('submit-msg').className, 'notice bad');
  assert.equal(f.element('submit').disabled, false);
  f.click(); await f.settle();
  assert.equal(attempts, 2);
  assert.equal(f.element('submit-msg').className, 'notice good', 'Retry can succeed after a network failure');
}

{
  const f = fixture({submit: init => new Promise((_resolve, reject) => {
    init.signal.addEventListener('abort', () => {
      const error = new Error('Aborted'); error.name = 'AbortError'; reject(error);
    });
  })});
  f.click(); await f.settle();
  assert.equal(f.element('submit').disabled, true, 'No duplicate submissions while sending');
  assert.equal(f.element('submit-model').disabled, true);
  assert.equal(f.element('submit').textContent, 'Submitting…');
  const timeout = [...f.timers.values()].find(timer => timer.delay === 30000);
  assert(timeout, 'The HTTPS request has a bounded timeout');
  timeout.callback(); await f.settle();
  assert.equal(f.element('submit-msg').className, 'notice bad');
  assert(f.element('submit-msg').textContent.startsWith('Submission timed out.'));
  assert.equal(f.element('submit').disabled, false, 'Timeout allows retry');
  assert(![...f.timers.values()].some(timer => timer.delay === 30000), 'Request timeout is cleaned up');
}

// A backup is a read-only workflow. Its UI must not inherit upload/ESS firmware
// qualification, including when the model family itself is not recognized.
for (const inverter of [
  {firmware: 2605558, nominal_voltage: 12, model: 'MultiPlus-II 12/3000/120-32'},
  {firmware: 2611556, nominal_voltage: 24, model: 'MultiPlus-II 24/3000/70-32'},
  {firmware: 2623570, nominal_voltage: 48, model: 'MultiPlus-II 48/5000/70-50'},
  {firmware: 2780563, nominal_voltage: 0, model: '', status: 'unsupported'},
]) {
  const f = fixture({settings: {firmware: inverter.firmware,
    setup: {...inverter, can_upload: false, ess_setpoint_ram: 0}}});
  assert.equal(f.element('download').disabled, false, `Allow backup for firmware ${inverter.firmware}`);
  assert.equal(f.element('upload-section').hidden, true, 'Unqualified uploads stay hidden');
  assert.equal(f.element('upfile').disabled, true, 'Unqualified uploads stay disabled');
  assert.equal(f.element('upwrite').disabled, true, 'Unqualified writes stay disabled');
  f.clickDownload(); await f.settle();
  const read = f.calls.findIndex(call => call.path === '/api/settings/read');
  const file = f.calls.findIndex(call => call.path === '/api/settings.rvsc');
  assert(read >= 0 && file > read, 'Download starts a fresh settings read before fetching the RVSC');
  assert.equal(f.calls[read].init.method, 'POST');
  assert.equal(f.downloads.length, 1, 'A complete read saves one backup');
  assert.deepEqual(new Uint8Array(await f.downloads[0].blob.arrayBuffer()),
    new Uint8Array(await f.backup.arrayBuffer()), 'Save the exact RVSC response bytes');
  assert.match(f.downloads[0].name, /^multi-settings-PRIVATE-SERIAL-.*\.rvsc$/);
  assert.equal(f.element('download-msg').className, 'notice good');
  assert.equal(f.element('download').disabled, false, 'Backup can be repeated');
  assert.equal(f.element('upload-section').hidden, true, 'Completing a backup does not qualify uploads');
  assert.equal(f.element('upwrite').disabled, true, 'Completing a backup does not qualify writes');
  assert(!f.calls.some(call => call.path === endpoint || call.path === '/api/upload/write'),
    'Local backup download never submits a profile or writes inverter settings');
}

{
  const f = fixture({settings: {file: false, file_problem: 'some settings could not be read',
    firmware: 2780563, setup: {status: 'unsupported', can_upload: false}}});
  f.clickDownload(); await f.settle();
  assert(f.calls.some(call => call.path === '/api/settings/read'), 'Incomplete reads can be retried afresh');
  assert(!f.calls.some(call => call.path === '/api/settings.rvsc'), 'Incomplete read never fetches a backup');
  assert.equal(f.downloads.length, 0, 'Incomplete read never saves a partial backup');
  assert.equal(f.element('download-msg').className, 'notice bad');
  assert(f.element('download-msg').textContent.includes('some settings could not be read'));
}

{
  const f = fixture({readFailure: true, settings: {firmware: 2780563,
    setup: {status: 'unsupported', can_upload: false}}});
  f.clickDownload(); await f.settle();
  assert(!f.calls.some(call => call.path === '/api/settings.rvsc'), 'Refused read never downloads stale backup');
  assert.equal(f.downloads.length, 0, 'Refused read never saves a backup');
  assert.equal(f.element('download-msg').className, 'notice bad');
  assert.equal(f.element('download').disabled, false, 'Refused read permits retry');
}

console.log('Direct in-app submission and unqualified-firmware backup checks passed');
