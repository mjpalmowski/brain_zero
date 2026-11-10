"""Read factual control-to-wire bindings from the installed VEConfigure resources."""

import argparse
import ctypes as ct
import hashlib
import json
from pathlib import Path
import re
import struct

from veconfigure_dfm import DFM, walk


FLAG_WORDS = (0, 1, 60, 61, 82)


def reference_form(executable):
    # Map the PE as a data resource. This does not execute its code.
    kernel = ct.WinDLL("kernel32", use_last_error=True)
    api = (
        ("LoadLibraryExW", (ct.c_wchar_p, ct.c_void_p, ct.c_uint), ct.c_void_p),
        ("FindResourceW", (ct.c_void_p, ct.c_wchar_p, ct.c_void_p), ct.c_void_p),
        ("SizeofResource", (ct.c_void_p, ct.c_void_p), ct.c_uint),
        ("LoadResource", (ct.c_void_p, ct.c_void_p), ct.c_void_p),
        ("LockResource", (ct.c_void_p,), ct.c_void_p),
        ("FreeLibrary", (ct.c_void_p,), ct.c_int),
    )
    for name, args, result in api:
        function = getattr(kernel, name)
        function.argtypes = args
        function.restype = result
    module = kernel.LoadLibraryExW(str(executable.resolve()), None, 0x22)
    if not module:
        raise ValueError("Could not open VEConfigure as a data resource.")
    try:
        resource = kernel.FindResourceW(module, "TVECONFIGURE2FORM", 10)
        if not resource:
            raise ValueError("The reference executable has no main Delphi form resource.")
        size = kernel.SizeofResource(module, resource)
        address = kernel.LockResource(kernel.LoadResource(module, resource))
        if not address or not 0 < size < 4 * 1024 * 1024:
            raise ValueError("The main Delphi form resource has an invalid size.")
        return DFM(ct.string_at(address, size)).parse()
    finally:
        kernel.FreeLibrary(module)


def variable_ids(data):
    marker = b"\x03\x0bTVariableID"
    start = data.index(marker) + len(marker)
    low, high, _ = struct.unpack_from("<iiI", data, start + 1)
    if not 0 <= low <= high < 4096:
        raise ValueError("Unsupported VEConfigure variable enumeration.")
    start += 13
    entries = {}
    for value in range(low, high + 1):
        size = data[start]
        start += 1
        entries[data[start:start + size].decode("ascii")] = value
        start += size
    return entries


def wire_bindings(entries):
    first = entries["EPROM_FlagsWord0"]
    first_bit = entries["EBIT_MultiPhaseSystem"]
    permanent = entries["pEBIT_ShortCircuitIsFatal"]
    bindings = {}
    for name, index in entries.items():
        if name.startswith("EPROM_"):
            bindings[name] = {"setting_id": index - first, "mask": 65535, "kind": "word"}
        elif name.startswith("EBIT_") and 0 <= index - first_bit < 64:
            word, bit = divmod(index - first_bit, 16)
            bindings[name] = {"setting_id": FLAG_WORDS[word], "mask": 1 << bit, "kind": "bit"}
        elif name.startswith("pEBIT_") and 0 <= index - permanent < 16:
            bindings[name] = {"setting_id": FLAG_WORDS[-1], "mask": 1 << (index - permanent), "kind": "bit"}
    if sorted(binding["setting_id"] for binding in bindings.values() if binding["kind"] == "word") != list(range(192)):
        raise ValueError("VEConfigure does not expose the expected 192 EEPROM word identifiers.")
    return bindings


def build(executable):
    raw = executable.read_bytes()
    root = reference_form(executable)
    storage = wire_bindings(variable_ids(raw))
    nodes = list(walk(root))
    properties = {node["name"]: node["properties"] for _, node in nodes}
    controls = []
    for path, node in nodes:
        props = node["properties"]
        if not node["class"].startswith("TSetting"):
            continue
        symbol = props.get("VariableID")
        pairs = [dict(storage[symbol], symbol=symbol)] if symbol in storage else []
        peer = props.get("VariableInfo.PeerVarsOrVariableInfo") or ""
        unresolved = [symbol] if symbol and not pairs else []
        for term in filter(None, peer.split(",")):
            match = re.fullmatch(r"(\w+)=\$([0-9A-F]+)", term)
            other = properties.get(match[1], {}).get("VariableID") if match else None
            if other in storage and int(match[2], 16) <= 65535:
                pairs.append(dict(storage[other], symbol=other, mask=int(match[2], 16), peer=match[1]))
            else:
                unresolved.append(term)
        callbacks = sorted(key for key in props if key in ("OnPeerDataSet", "OnChange", "OnDataSet"))
        controls.append({
            "control": node["name"], "path": path, "type": node["class"],
            "label": props.get("LongSettingDescription") or props.get("Caption") or node["name"],
            "unit": (props.get("MeasurementUnit") or "").lstrip("|").strip(),
            "inverted": bool(props.get("Inverted")),
            "bindings": pairs, "unresolved": unresolved, "callbacks": callbacks,
            "resource_visible": props.get("Visible", True), "resource_enabled": props.get("Enabled", True),
        })
    return {
        "status": "mapped",
        "source": "VEConfigure form resources and TVariableID enumeration",
        "executable_sha256": hashlib.sha256(raw).hexdigest(),
        "scope": "Design-time bindings. Runtime availability, value transforms and write access require separate evidence.",
        "setting_symbols": [dict(binding, symbol=symbol) for symbol, binding in storage.items()],
        "controls": controls,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    args.report.write_text(json.dumps(build(args.exe), indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
