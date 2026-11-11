'use strict';

const MAX_PROFILE_BYTES = 256 * 1024;
const element = id => document.getElementById(id);
const form = element('submission-form');
const button = element('submit');
let profile = null;
let sending = false;
let fileGeneration = 0;
let manualSelection = false;
let stopBridge = () => {};

function message(text, error = false) {
  element('message').textContent = text;
  element('message').classList.toggle('error', error);
}

function update() {
  button.disabled = !profile || sending;
}

function acceptProfile(candidate) {
  if (!candidate || typeof candidate !== 'object' || Array.isArray(candidate) ||
      Object.keys(candidate).sort().join(',') !== 'grid,profile' ||
      !candidate.profile || !Array.isArray(candidate.profile.settings) ||
      candidate.profile.settings.length !== 192 || typeof candidate.grid !== 'string' ||
      new TextEncoder().encode(JSON.stringify(candidate)).length > MAX_PROFILE_BYTES) {
    throw new Error('Choose the compatibility.json downloaded from brain_zero.');
  }
  profile = candidate;
  element('firmware').textContent = String(candidate.profile.schema_firmware);
  element('voltage').textContent = String(candidate.profile.nominal_voltage) + ' V';
  element('setting-count').textContent = String(candidate.profile.settings.length);
  element('profile-summary').hidden = false;
  element('handoff').hidden = true;
  message('Profile ready. Add your inverter model and submit.');
  update();
}

element('profile-file').addEventListener('change', async event => {
  const generation = ++fileGeneration;
  manualSelection = true;
  stopBridge();
  element('handoff').hidden = true;
  profile = null;
  element('profile-summary').hidden = true;
  update();
  try {
    const file = event.target.files[0];
    if (!file) return;
    if (!file.name.toLowerCase().endsWith('.json') || file.size > MAX_PROFILE_BYTES) {
      throw new Error('Choose a compatibility.json file up to 256 KiB.');
    }
    const text = await file.text();
    if (generation !== fileGeneration) return;
    acceptProfile(JSON.parse(text));
  } catch (error) {
    if (generation !== fileGeneration) return;
    message(error.message || 'The compatibility file could not be read.', true);
  }
});

// The controller initiates this transfer. HTTPS pages cannot fetch a controller
// over local HTTP, so data arrives through a nonce-bound opener handshake.
const nonce = new URLSearchParams(location.hash.slice(1)).get('from-controller');
if (/^[0-9a-f]{32}$/.test(nonce || '') && window.opener) {
  element('handoff').hidden = false;
  const opener = window.opener;
  let received = false;
  const ready = () => {
    if (!received && !opener.closed) opener.postMessage({type: 'brain-zero-profile-ready', nonce}, '*');
  };
  const timer = setInterval(ready, 1000);
  stopBridge = () => {
    received = true;
    clearInterval(timer);
  };
  setTimeout(() => {
    clearInterval(timer);
    if (!received) {
      element('handoff').textContent = 'You can also select the downloaded compatibility.json below.';
    }
  }, 5 * 60 * 1000);
  window.addEventListener('message', event => {
    if (received || manualSelection || sending || event.source !== opener || event.data?.type !== 'brain-zero-profile' ||
        event.data.nonce !== nonce) return;
    try {
      acceptProfile(event.data.profile);
      received = true;
      clearInterval(timer);
      opener.postMessage({type: 'brain-zero-profile-received', nonce}, event.origin);
      history.replaceState(null, '', location.pathname);
    } catch (error) {
      message(error.message, true);
    }
  });
  ready();
}

form.addEventListener('submit', async event => {
  event.preventDefault();
  if (sending || !profile || !form.reportValidity()) return;
  sending = true;
  update();
  message('Submitting your compatibility profile…');
  try {
    const response = await fetch('/api/compatibility', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        model: element('model').value.trim(),
        email: element('email').value.trim(),
        consent: element('consent').checked,
        profile,
      }),
      signal: AbortSignal.timeout(20000),
    });
    if (response.status === 429) throw new Error('Too many uploads. Please try again in a minute.');
    if (!response.headers.get('content-type')?.includes('application/json')) {
      throw new Error('The upload service is unavailable. Please try again shortly.');
    }
    const result = await response.json();
    if (!response.ok || !result.ok) throw new Error(result.error || 'Your profile could not be submitted.');
    if (!/^[a-f0-9]{32}$/.test(result.receipt)) throw new Error('The server did not return a submission reference.');
    element('receipt-id').textContent = result.receipt;
    form.hidden = true;
    element('receipt').hidden = false;
  } catch (error) {
    message(error.name === 'TimeoutError'
      ? 'The upload timed out. Please try again.'
      : error.message || 'Your profile could not be submitted. Please try again.', true);
  } finally {
    sending = false;
    update();
  }
});
