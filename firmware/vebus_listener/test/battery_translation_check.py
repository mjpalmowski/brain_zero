"""Check translated battery UI and the errors displayed by that UI, offline."""
import importlib.util
import json
import re
from html.parser import HTMLParser
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "firmware/vebus_listener/src"
TECHNICAL = {"JK", "PACE", "AA:BB:CC:DD:EE:FF"}
# These French words are correctly identical to English; they are not fallbacks.
IDENTICAL = {"fr": {"Batteries", "Batteries · brain_zero", "Diagnostics"}}


class VisibleText(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ignored = 0
        self.messages = set()

    def handle_starttag(self, tag, attrs):
        if tag in {"script", "style"}:
            self.ignored += 1
        if not self.ignored:
            for name, value in attrs:
                if name in {"title", "placeholder", "aria-label"} and value:
                    self.messages.add(value.strip())

    def handle_endtag(self, tag):
        if tag in {"script", "style"}:
            self.ignored -= 1

    def handle_data(self, text):
        if not self.ignored and text.strip():
            self.messages.add(" ".join(text.split()))


def strings(source):
    for match in re.finditer(r'"(?:[^"\\]|\\.)*"', source):
        yield json.loads(match.group())


def battery_messages():
    page = (SRC / "battery_page.h").read_text(encoding="utf-8")
    parser = VisibleText()
    parser.feed("".join(re.findall(r'R"HTML\((.*?)\)HTML"', page, re.S)))
    messages = parser.messages
    rows = re.search(r"const rows=\[(.*?)\];", page, re.S).group(1)
    messages.update(re.findall(r"\['([^']+)'", rows))
    messages.update(re.findall(r"tr\('([^']+)'\)", page))
    messages.update(re.findall(r"j\.bank\.complete\?'([^']+)':'([^']+)'", page)[0])
    for error in re.findall(r"throw Error\((.*?)\)", page):
        messages.update(re.findall(r"'([^']+)'", error))
    sources = [(SRC / name).read_text(encoding="utf-8")
               for name in ("battery_ble.cpp", "battery_control.cpp")]
    main = (SRC / "main.cpp").read_text(encoding="utf-8")
    sources.append(main.split("bool listener_bms_set(", 1)[1]
                   .split("bool listener_battery_input_enabled(", 1)[0])
    web = (SRC / "web.cpp").read_text(encoding="utf-8")
    sources.append(web.split("void handle_bms_get(", 1)[1]
                   .split("void battery_input_json(", 1)[0])
    for source in sources:
        for text in strings(source):
            if re.match(r"[A-Z][A-Za-z ]+", text) and " " in text:
                messages.add(text)
            if text.startswith('{"error":"'):
                messages.add(json.loads(text)["error"])
    messages.add("Bluetooth batteries and charge limits")
    return messages


def main():
    catalogue = json.loads((ROOT / "assets/brand/ui-translations.json").read_text(encoding="utf-8"))
    required = battery_messages() - TECHNICAL
    assert required <= set(catalogue["english"]), sorted(required - set(catalogue["english"]))
    for code, translated in catalogue["translations"].items():
        for key in required:
            assert translated.get(key, "").strip(), (code, key)
            assert translated[key] != key or key in IDENTICAL.get(code, set()), (code, "English fallback", key)
    # Exercise the production generator: complete language sets, sorted unique
    # English messages, matching placeholders, and byte-for-byte generated output.
    spec = importlib.util.spec_from_file_location("embed_languages", ROOT / "tools/brand/embed_languages.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    generated, languages, _ = module.build()
    assert generated == (SRC / "language_assets.h").read_text(encoding="utf-8")
    print(f"Verified {len(required)} battery UI/error messages in all {languages} languages")


if __name__ == "__main__":
    main()
