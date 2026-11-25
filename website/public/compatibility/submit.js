import {sanitizeBackup} from './sanitizer.js';
import {detectModel, modelNames} from './models.js';

const PROFILE_FIELDS = ['format', 'model', 'unit_firmware', 'schema_firmware', 'descriptor_firmware',
  'nominal_voltage', 'descriptor_sha256', 'version_reply', 'unit_info', 'schema_version', 'unit_version',
  'grid_bytes', 'assistant_bytes', 'flag_access', 'setting_fields', 'settings'];

function exactKeys(value, fields) {
  return value && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).length === fields.length && fields.every(field => Object.hasOwn(value, field));
}

const layouts = ['single', 'parallel', 'three_phase', 'three_phase_parallel'];
function capturedLayout([l1, l2]) {
  return l2 ? (l1 > 1 ? 'three_phase_parallel' : 'three_phase') : (l1 > 1 ? 'parallel' : 'single');
}

export function makeSubmission(bundle, consent, chosenModel, layout, counts) {
  if (!exactKeys(bundle, ['profile', 'grid']) || !exactKeys(bundle.profile, PROFILE_FIELDS)) {
    throw new Error('Only a sanitized compatibility profile can be submitted.');
  }
  if (!modelNames.includes(chosenModel) || !layouts.includes(layout) || !Array.isArray(counts) ||
      counts.length !== 3 || !counts.every(n => Number.isInteger(n) && n >= 0 && n <= 6) ||
      !counts[0] || (counts[1] || counts[2]) && !(counts[0] === counts[1] && counts[1] === counts[2]) ||
      (capturedLayout(counts) !== 'single' && capturedLayout(counts) !== layout)) {
    throw new Error('Check the inverter model and system layout.');
  }
  if (consent !== true) { throw new Error('Confirm that you agree to submit this profile.'); }
  return {model: chosenModel, system_layout: layout, units_per_phase: counts, consent: true, profile: bundle};
}

if (typeof document !== 'undefined') {
  const backup = document.getElementById('backup'), choose = document.getElementById('choose');
  const model = document.getElementById('model'), consent = document.getElementById('consent');
  const submit = document.getElementById('submit'), fileStatus = document.getElementById('file-status');
  const submitStatus = document.getElementById('submit-status'), summary = document.getElementById('profile-summary');
  const layout = document.getElementById('system-layout');
  for (const name of modelNames) model.add(new Option(name, name));
  let counts = null;
  let sanitized = null, busy = false, submitted = false;
  const update = () => {
    let valid = false;
    try { makeSubmission(sanitized, consent.checked, model.value, layout.value, counts); valid = true; } catch { /* Keep submission disabled. */ }
    submit.disabled = busy || submitted || !valid;
  };
  choose.addEventListener('click', () => backup.click());
  for (const field of [model, layout, consent]) field.addEventListener('change', update);
  backup.addEventListener('change', async () => {
    sanitized = null; submitted = false; summary.hidden = true; submitStatus.textContent = ''; submitStatus.className = 'status';
    counts = null; model.value = ''; consent.checked = false; model.disabled = layout.disabled = false;
    update();
    const file = backup.files?.[0];
    if (!file) { fileStatus.textContent = 'No backup selected.'; return; }
    choose.disabled = true; fileStatus.className = 'status'; fileStatus.textContent = 'Removing private fields locally…';
    try {
      if (file.size > 128 * 1024) { throw new Error('The backup is too large.'); }
      const result = await sanitizeBackup(new Uint8Array(await file.arrayBuffer()));
      sanitized = result.bundle; counts = result.unitsPerPhase;
      model.value = detectModel(sanitized.profile);
      layout.value = capturedLayout(counts);
      for (const option of layout.options) option.disabled = counts.reduce((a,b) => a+b) > 1 && option.value !== layout.value;
      document.getElementById('unit-count').textContent = counts[1] ? counts.join(' / ') + ' (L1 / L2 / L3)' : String(counts[0]);
      document.getElementById('voltage').textContent = sanitized.profile.nominal_voltage + ' V';
      document.getElementById('firmware').textContent = String(sanitized.profile.schema_firmware);
      summary.hidden = false;
      fileStatus.textContent = 'Ready to submit. Private fields have been removed; your original has not left this browser.';
    } catch {
      sanitized = null; fileStatus.className = 'status error';
      fileStatus.textContent = 'This backup could not be converted. Choose a complete .rvsc or .rvms for a recognized 230 V MultiPlus-II with matching units (maximum 128 KiB). Nothing was submitted.';
    } finally {
      backup.value = ''; choose.disabled = false; update();
    }
  });
  submit.addEventListener('click', async () => {
    let envelope;
    try { envelope = makeSubmission(sanitized, consent.checked, model.value, layout.value, counts); } catch { update(); return; }
    busy = true; model.disabled = layout.disabled = true; update(); choose.disabled = true; consent.disabled = true;
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
      busy = false; model.disabled = layout.disabled = submitted; choose.disabled = false; consent.disabled = false; update();
    }
  });
}
