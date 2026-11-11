"""Verify the hosted controller transmits sanitized data, never backup files."""

import base64
import json
from pathlib import Path
import re
import subprocess
import unittest

from build_hosted_sanitizer import OUTPUT, generated_parser
from export_compatibility import export_compatibility
from test_intake_compatibility import ASSISTANT, CREDENTIAL, SERIAL, private_backup


ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / "website" / "public" / "compatibility"


class HostedSanitizerTest(unittest.TestCase):
    def test_generated_parser_cannot_drift_from_tested_offline_parser(self):
        self.assertEqual(OUTPUT.read_text(encoding="utf-8"), generated_parser())

    def test_hosted_page_uses_only_same_origin_assets_and_api(self):
        html = (PAGE / "index.html").read_text(encoding="utf-8")
        self.assertIn("connect-src 'self'", html)
        self.assertIn("form-action 'none'", html)
        self.assertNotRegex(html, r"(?:src|href)=\"https?://")
        self.assertNotRegex(html, r"<script[^>]*>\s*[^<\s]")
        self.assertRegex(html, r'<input id="backup"[^>]* hidden>')
        self.assertNotRegex(html, r"<form\b")
        controller = (PAGE / "submit.js").read_text(encoding="utf-8")
        self.assertNotIn("file.name", controller)
        self.assertNotIn("FormData", controller)
        self.assertNotIn("localStorage", controller)
        self.assertNotIn("sessionStorage", controller)
        self.assertEqual(re.findall(r"fetch\(([^,]+)", controller), ["'/api/compatibility'"])

    def test_controller_sends_only_sanitized_envelope_after_explicit_consent(self):
        # Execute the exact shipped controller, using DOM elements and a network
        # stub. The source backup contains artificial private-field markers.
        driver = r"""
import {readFileSync} from 'node:fs';
const source = JSON.parse(readFileSync(0, 'utf8'));
class Element {
  value = ''; checked = false; disabled = false; hidden = true; textContent = ''; className = '';
  events = {}; files = [];
  addEventListener(type, handler) { this.events[type] = handler; }
  click() { return this.events.click?.(); }
}
const elements = Object.fromEntries(['backup','choose','model','consent','submit','file-status',
  'submit-status','profile-summary','voltage','model-code','firmware','settings'].map(id=>[id,new Element()]));
globalThis.document = {getElementById: id => elements[id]};
const calls = [];
let responseStatus = 202;
globalThis.fetch = async (url, request) => {
  if (!elements.submit.disabled || !elements.choose.disabled || !elements.model.disabled || !elements.consent.disabled) {
    throw new Error('Submission controls must be disabled while sending.');
  }
  calls.push({url, ...request, signal: undefined});
  return {status:responseStatus,json:async()=>({ok:true,status:'queued',receipt:'0123456789abcdef0123456789abcdef'})};
};
const parser = 'data:text/javascript;base64,' + source.parser;
const controller = source.controller.replace("'./sanitizer.js'", JSON.stringify(parser));
const module = await import('data:text/javascript;base64,' + Buffer.from(controller).toString('base64'));
elements.model.value = '  MultiPlus-II 48/5000/70-50  ';
const raw = Buffer.from(source.backup, 'base64');
const file = {size:raw.length, arrayBuffer:async()=>raw.buffer.slice(raw.byteOffset,raw.byteOffset+raw.byteLength)};
Object.defineProperty(file,'name',{get(){throw new Error('Original filename must never be read.');}});
elements.backup.files = [file];
await elements.backup.events.change();
if(calls.length || !elements.submit.disabled || elements.backup.value !== '') throw new Error('Parsing must not upload.');
if(elements['profile-summary'].hidden || elements.voltage.textContent !== '48 V') throw new Error('Profile summary missing.');
await elements.submit.events.click();
if(calls.length) throw new Error('Explicit consent is required.');
elements.consent.checked = true; elements.consent.events.change();
if(elements.submit.disabled) throw new Error('Valid consent must enable submission.');
await elements.submit.events.click();
if(calls.length !== 1 || !elements.submit.disabled || !elements['submit-status'].textContent.includes('0123456789abcdef')) {
  throw new Error('Successful receipt must prevent duplicate submission.');
}
elements.backup.files = [{size:4,arrayBuffer:async()=>new Uint8Array([0,1,2,3]).buffer}];
await elements.backup.events.change();
await elements.submit.events.click();
if(calls.length!==1 || !elements.submit.disabled || !elements['profile-summary'].hidden) throw new Error('Invalid replacement must discard prior profile.');
elements.backup.files = [file]; await elements.backup.events.change();
responseStatus = 503;
await elements.submit.events.click();
if(calls.length!==2 || elements.submit.disabled || elements['submit-status'].className !== 'status error') throw new Error('Failures must allow explicit retry.');
let rejected=0;
for(const invalid of [{...JSON.parse(calls[0].body).profile,serial:'private'},
  {...JSON.parse(calls[0].body).profile,profile:{...JSON.parse(calls[0].body).profile.profile,serial:'private'}}]) {
  try{module.makeSubmission(invalid,'model',true);}catch{rejected++;}
}
if(rejected!==2)throw new Error('Unexpected private fields must reject.');
process.stdout.write(JSON.stringify(calls[0]));
"""
        data = {"parser": base64.b64encode(OUTPUT.read_bytes()).decode(),
                "controller": (PAGE / "submit.js").read_text(encoding="utf-8"),
                "backup": base64.b64encode(private_backup()).decode()}
        result = subprocess.run(["node", "--input-type=module", "-e", driver], input=json.dumps(data),
                                text=True, capture_output=True, check=True, timeout=30)
        self.assertEqual(result.stderr, "")
        call = json.loads(result.stdout)
        self.assertEqual(call["url"], "/api/compatibility")
        self.assertEqual(call["method"], "POST")
        self.assertEqual(call["credentials"], "omit")
        self.assertEqual(call["mode"], "same-origin")
        self.assertEqual(call["redirect"], "error")
        envelope = json.loads(call["body"])
        self.assertEqual(set(envelope), {"model", "consent", "profile"})
        self.assertTrue(envelope["consent"])
        self.assertEqual(envelope["model"], "MultiPlus-II 48/5000/70-50")
        self.assertEqual(envelope["profile"], json.loads(json.dumps(export_compatibility(private_backup()))))
        for secret in (SERIAL, CREDENTIAL, ASSISTANT, b"PRIVATE-TIMEDATA"):
            self.assertNotIn(secret.decode(), result.stdout)
            self.assertNotIn(secret.hex(), result.stdout)


if __name__ == "__main__":
    unittest.main()
