import {sanitizeRvsc} from './sanitizer.js';

const PROFILE_FIELDS = ['format', 'model', 'unit_firmware', 'schema_firmware', 'descriptor_firmware',
  'nominal_voltage', 'descriptor_sha256', 'version_reply', 'unit_info', 'schema_version', 'unit_version',
  'grid_bytes', 'assistant_bytes', 'flag_access', 'setting_fields', 'settings'];

function exactKeys(value, fields) {
  return value && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).length === fields.length && fields.every(field => Object.hasOwn(value, field));
}

export function makeSubmission(bundle, model, consent) {
  if (!exactKeys(bundle, ['profile', 'grid']) || !exactKeys(bundle.profile, PROFILE_FIELDS)) {
    throw new Error('Only a sanitized compatibility profile can be submitted.');
  }
  if (typeof model !== 'string' || !model.trim() || model.length > 160 || /[\x00-\x1f\x7f]/.test(model)) {
    throw new Error('Enter the inverter model using at most 160 characters.');
  }
  if (consent !== true) { throw new Error('Confirm that you agree to submit this profile.'); }
  return {model: model.trim(), consent: true, profile: bundle};
}

if (typeof document !== 'undefined') {
  const backup = document.getElementById('backup'), choose = document.getElementById('choose');
  const model = document.getElementById('model'), consent = document.getElementById('consent');
  const submit = document.getElementById('submit'), fileStatus = document.getElementById('file-status');
  const submitStatus = document.getElementById('submit-status'), summary = document.getElementById('profile-summary');
  let sanitized = null, busy = false, submitted = false;
  const update = () => {
    let valid = false;
    try { makeSubmission(sanitized, model.value, consent.checked); valid = true; } catch { /* Keep submission disabled. */ }
    submit.disabled = busy || submitted || !valid;
  };
  choose.addEventListener('click', () => backup.click());
  model.addEventListener('input', update);
  consent.addEventListener('change', update);
  backup.addEventListener('change', async () => {
    sanitized = null; submitted = false; summary.hidden = true; submitStatus.textContent = ''; submitStatus.className = 'status';
    update();
    const file = backup.files?.[0];
    if (!file) { fileStatus.textContent = 'No backup selected.'; return; }
    choose.disabled = true; fileStatus.className = 'status'; fileStatus.textContent = 'Removing private fields locally…';
    try {
      if (file.size > 128 * 1024) { throw new Error('The backup is too large.'); }
      sanitized = await sanitizeRvsc(new Uint8Array(await file.arrayBuffer()));
      document.getElementById('voltage').textContent = sanitized.profile.nominal_voltage + ' V';
      document.getElementById('model-code').textContent = String(sanitized.profile.model);
      document.getElementById('firmware').textContent = String(sanitized.profile.schema_firmware);
      document.getElementById('settings').textContent = String(sanitized.profile.settings.length);
      summary.hidden = false;
      fileStatus.textContent = 'Ready to submit. Private fields have been removed; your original has not left this browser.';
    } catch {
      sanitized = null; fileStatus.className = 'status error';
      fileStatus.textContent = 'This backup could not be converted. Choose a complete, current .rvsc for one standalone 230 V MultiPlus-II (maximum 128 KiB). Nothing was submitted.';
    } finally {
      backup.value = ''; choose.disabled = false; update();
    }
  });
  submit.addEventListener('click', async () => {
    let envelope;
    try { envelope = makeSubmission(sanitized, model.value, consent.checked); } catch { update(); return; }
    busy = true; update(); choose.disabled = true; model.disabled = true; consent.disabled = true;
    submitStatus.className = 'status'; submitStatus.textContent = 'Submitting the sanitized profile…';
    try {
      const response = await fetch('/api/compatibility', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(envelope), credentials: 'omit', cache: 'no-store',
        mode: 'same-origin', redirect: 'error', signal: AbortSignal.timeout(30000),
      });
      if (response.status !== 202) { throw new Error('The submission was not accepted.'); }
      const result = await response.json();
      if (result.ok !== true || result.status !== 'queued' || !/^[a-f0-9]{32}$/.test(result.receipt)) {
        throw new Error('The submission could not be confirmed.');
      }
      submitted = true;
      submitStatus.className = 'status success';
      submitStatus.textContent = 'Profile submitted for compatibility testing. Your reference: ' + result.receipt;
    } catch {
      submitStatus.className = 'status error';
      submitStatus.textContent = 'We could not confirm the submission. Check your connection and try again shortly. Your original backup was not sent.';
    } finally {
      busy = false; choose.disabled = false; model.disabled = false; consent.disabled = false; update();
    }
  });
}
