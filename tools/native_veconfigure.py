"""Observe VEConfigure's runtime controls on a synthetic offline file."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from export_compatibility import MAX_BACKUP_BYTES, export_compatibility
from validate_compatibility import validate_profile


EXE = Path(r"C:\Program Files (x86)\VE Configure tools\VEConfig.exe")
EXPECTED_EXE = "d979d4f7ad111f2fe9bb3c504ec74c55857a124373dd202af1454750b43fca2f"
FIREWALL_RULE = "brain_zero VEConfigure offline"
MAIN_FORM = "TVEConfigure2Form"
PAGE_CONTROL = "TPageControl"
CONTROLS = {
    "TSettingEdit", "TRelatedLabel", "TSettingCheckBox", "TSettingRadioGroup",
    "TSettingComboBox", "TComboBox", "TCheckBox", "TGroupButton",
}
COMBO_BOXES = {"TSettingComboBox", "TComboBox"}
CHECK_BOXES = {"TSettingCheckBox", "TCheckBox", "TGroupButton"}


class Blocked(Exception):
    """An isolation or runtime prerequisite is unavailable."""


class NativeFailure(Exception):
    """VEConfigure did not expose the expected offline UI."""


def digest(data):
    return hashlib.sha256(data).hexdigest()


def bind(library, name, result, *args):
    function = getattr(library, name)
    function.restype, function.argtypes = result, args
    return function


def powershell_path():
    return Path(os.environ["SystemRoot"]) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"


def remove_vm_serial_console():
    # This is called only on a disposable GitHub-hosted worker. The offline
    # capture independently checks that no DOS or registry serial ports remain.
    script = r"""
    $ErrorActionPreference = 'Stop'
    $ports = @(Get-PnpDevice -Class Ports -PresentOnly | Where-Object FriendlyName -Match '\(COM\d+\)')
    foreach ($port in $ports) {
        Write-Host "Removing hosted VM serial device $($port.FriendlyName) [$($port.InstanceId)]"
        & "$env:SystemRoot\System32\pnputil.exe" /remove-device $port.InstanceId /force
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    """
    result = subprocess.run([str(powershell_path()), "-NoProfile", "-NonInteractive", "-Command", script],
                            timeout=25)
    if result.returncode:
        raise Blocked("Could not remove the virtual serial console from the hosted worker.")


def check_firewall(exe):
    """Verify the worker has applied a program-wide outbound block."""
    script = r"""
    $ErrorActionPreference = 'Stop'
    $disabled = @(Get-NetFirewallProfile -PolicyStore ActiveStore | Where-Object Enabled -ne True)
    if ($disabled.Count -ne 0 -or (Get-Service MpsSvc).Status -ne 'Running') { exit 1 }
    $rules = @(Get-NetFirewallRule -PolicyStore ActiveStore -DisplayName $env:BZ_RULE |
        Where-Object { $_.Enabled -eq 'True' -and $_.Direction -eq 'Outbound' -and
                       $_.Action -eq 'Block' -and $_.Profile -eq 'Any' })
    foreach ($rule in $rules) {
        $app = $rule | Get-NetFirewallApplicationFilter
        $addresses = $rule | Get-NetFirewallAddressFilter
        $ports = $rule | Get-NetFirewallPortFilter
        $service = $rule | Get-NetFirewallServiceFilter
        if ($app.Program -ieq $env:BZ_EXE -and $addresses.LocalAddress -eq 'Any' -and
            $addresses.RemoteAddress -eq 'Any' -and $ports.Protocol -eq 'Any' -and
            $service.Service -eq 'Any') { exit 0 }
    }
    exit 1
    """
    result = subprocess.run([str(powershell_path()), "-NoProfile", "-NonInteractive", "-Command", script],
                            env={**os.environ, "BZ_EXE": str(exe), "BZ_RULE": FIREWALL_RULE},
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
    if result.returncode:
        raise Blocked("A verified outbound firewall block for VEConfig.exe is required.")


class Windows:
    """Small Win32 reader. Input is limited to page navigation and one known dialog."""

    def __init__(self):
        w = wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        ui = ctypes.WinDLL("user32", use_last_error=True)
        self.devices = bind(kernel, "QueryDosDeviceW", w.DWORD, w.LPCWSTR, w.LPWSTR, w.DWORD)
        self.callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        self.enum_windows = bind(ui, "EnumWindows", w.BOOL, self.callback, w.LPARAM)
        self.enum_children = bind(ui, "EnumChildWindows", w.BOOL, w.HWND, self.callback, w.LPARAM)
        self.window_pid = bind(ui, "GetWindowThreadProcessId", w.DWORD, w.HWND, ctypes.POINTER(w.DWORD))
        self.class_name = bind(ui, "GetClassNameW", ctypes.c_int, w.HWND, w.LPWSTR, ctypes.c_int)
        self.visible = bind(ui, "IsWindowVisible", w.BOOL, w.HWND)
        self.enabled = bind(ui, "IsWindowEnabled", w.BOOL, w.HWND)
        self.parent = bind(ui, "GetParent", w.HWND, w.HWND)
        self.rect = bind(ui, "GetWindowRect", w.BOOL, w.HWND, ctypes.POINTER(w.RECT))
        self.client_rect = bind(ui, "GetClientRect", w.BOOL, w.HWND, ctypes.POINTER(w.RECT))
        self.style = bind(ui, "GetWindowLongW", w.LONG, w.HWND, ctypes.c_int)
        self.post_message = bind(ui, "PostMessageW", w.BOOL, w.HWND, w.UINT, w.WPARAM, w.LPARAM)
        self.send_message = bind(ui, "SendMessageTimeoutW", ctypes.c_ssize_t, w.HWND, w.UINT, w.WPARAM,
                                 w.LPARAM, w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t))
        thread = bind(kernel, "GetCurrentThreadId", w.DWORD)
        desktop = bind(ui, "GetThreadDesktop", w.HANDLE, w.DWORD)
        info = bind(ui, "GetUserObjectInformationW", w.BOOL, w.HANDLE, ctypes.c_int, ctypes.c_void_p,
                    w.DWORD, ctypes.POINTER(w.DWORD))
        name = ctypes.create_unicode_buffer(256)
        if not desktop(thread()) or not info(desktop(thread()), 2, name, ctypes.sizeof(name), None):
            raise Blocked("The hosted worker has no accessible Windows desktop.")
        if not self.enum_windows(self.callback(lambda _handle, _value: True), 0):
            raise Blocked("The hosted worker cannot enumerate desktop windows.")

    def check_serial(self):
        import winreg

        names = ctypes.create_unicode_buffer(65536)
        used = self.devices(None, names, len(names))
        if not used:
            raise Blocked("The worker could not verify that COM devices are absent.")
        ports = {item.upper() for item in names[:used].split("\0") if re.fullmatch(r"COM\d+", item, re.IGNORECASE)}
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                for index in range(winreg.QueryInfoKey(key)[1]):
                    _, value, _ = winreg.EnumValue(key, index)
                    ports.add(value.upper() if isinstance(value, str) and re.fullmatch(r"COM\d+", value, re.IGNORECASE)
                              else "unidentified")
        except FileNotFoundError:
            pass
        if ports:
            raise Blocked("The hosted worker exposes serial ports: " + ", ".join(sorted(ports)) + ".")

    def owns(self, handle, pid):
        process_id = wintypes.DWORD()
        self.window_pid(handle, ctypes.byref(process_id))
        return process_id.value == pid

    def send(self, handle, pid, msg, first=0, second=0):
        result = ctypes.c_size_t()
        if not self.owns(handle, pid) or not self.send_message(handle, msg, first, second, 2, 400,
                                                                ctypes.byref(result)):
            raise NativeFailure("A native control stopped responding.")
        return ctypes.c_ssize_t(result.value).value

    def post(self, handle, pid, msg, first=0, second=0):
        if not self.owns(handle, pid) or not self.post_message(handle, msg, first, second):
            raise NativeFailure("Could not navigate the native page.")

    def dismiss_information(self, handle, pid):
        rect = wintypes.RECT()
        if not self.owns(handle, pid) or not self.client_rect(handle, ctypes.byref(rect)):
            raise NativeFailure("Could not acknowledge the known offline information dialog.")
        point = (rect.bottom // 2 << 16) | rect.right // 2
        self.post(handle, pid, 0x0201, 1, point)  # WM_LBUTTONDOWN
        self.post(handle, pid, 0x0202, 0, point)  # WM_LBUTTONUP

    def describe(self, handle, pid):
        label = ctypes.create_unicode_buffer(512)
        kind = ctypes.create_unicode_buffer(128)
        rect = wintypes.RECT()
        self.class_name(handle, kind, len(kind))
        self.rect(handle, ctypes.byref(rect))
        if "edit" in kind.value.casefold() and self.style(handle, -16) & 0x20:
            label.value = "[redacted]"
        elif kind.value not in {"TEdit", "Edit"}:
            self.send(handle, pid, 0x000D, len(label), ctypes.addressof(label))  # WM_GETTEXT
        return {"handle": handle, "parent": self.parent(handle), "class": kind.value,
                "text": label.value, "visible": bool(self.visible(handle)), "enabled": bool(self.enabled(handle)),
                "rect": [rect.left, rect.top, rect.right, rect.bottom]}

    def windows(self, pid, parent=None):
        handles = []

        def record(handle, _value):
            if self.owns(handle, pid) and (parent is not None or self.visible(handle)):
                handles.append(handle)
            return True

        if parent is None:
            self.enum_windows(self.callback(record), 0)
        else:
            self.enum_children(parent, self.callback(record), 0)
        return [self.describe(handle, pid) for handle in handles]

    def combo_items(self, handle, pid):
        count = self.send(handle, pid, 0x0146)  # CB_GETCOUNT
        if not 0 <= count <= 256:
            raise NativeFailure("A native selector has an unexpected option count.")
        items = []
        for index in range(count):
            size = self.send(handle, pid, 0x0149, index)  # CB_GETLBTEXTLEN
            if not 0 <= size <= 512:
                raise NativeFailure("A native selector has an unexpected option length.")
            label = ctypes.create_unicode_buffer(size + 1)
            self.send(handle, pid, 0x0148, index, ctypes.addressof(label))  # CB_GETLBTEXT
            items.append(label.value)
        return items


def area(control):
    left, top, right, bottom = control["rect"]
    return max(0, right - left) * max(0, bottom - top)


def offline_controls(win, main, pid):
    if not win.enabled(main):
        raise NativeFailure("An unexpected native dialog interrupted the offline capture.")
    controls = win.windows(pid, main)
    release = [item for item in controls if item["text"] == "Release port"]
    if len(release) != 1 or release[0]["enabled"]:
        raise NativeFailure("VEConfigure did not confirm offline file mode.")
    return controls


def wait_for_file(win, process, report):
    deadline = time.monotonic() + 25
    acknowledged = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise NativeFailure("VEConfigure exited before opening the synthetic file.")
        windows = win.windows(process.pid)
        main = next((item for item in windows if item["class"] == MAIN_FORM), None)
        dialogs = [item for item in windows if item["class"] == "TMessageForm"]
        report["startup"] = {"window_classes": [item["class"] for item in windows[:12]],
                             "fixture_title_seen": bool(main and "BZSIM000000" in main["text"]),
                             "dialogs": [item["text"][:80] for item in dialogs[:4]]}
        if main and dialogs:
            # The synthetic file has an erased Assistant area. On startup the
            # reference build shows one Information dialog with a single OK.
            dialog = dialogs[0]
            if len(dialogs) == 1 and dialog["handle"] == acknowledged:
                time.sleep(0.15)
                continue
            buttons = [item for item in win.windows(process.pid, dialog["handle"]) if item["visible"]]
            report["startup"]["dialog_controls"] = [item["text"][:80] for item in buttons[:8]]
            if (acknowledged is not None or len(dialogs) != 1 or dialog["text"] != "Information" or
                    len(buttons) != 1 or buttons[0]["text"] != "OK" or not buttons[0]["enabled"]):
                raise NativeFailure("An unexpected VEConfigure startup dialog needs review.")
            win.dismiss_information(buttons[0]["handle"], process.pid)
            acknowledged = dialog["handle"]
        elif main and main["enabled"] and "BZSIM000000" in main["text"]:
            offline_controls(win, main["handle"], process.pid)
            return main["handle"]
        time.sleep(0.15)
    raise NativeFailure("Timed out waiting for the synthetic VEConfigure file to open.")


def move_to(win, control, pid, target):
    handle = control["handle"]
    for _ in range(32):
        current = win.send(handle, pid, 0x130B)  # TCM_GETCURSEL
        if current == target:
            return
        key = 39 if current < target else 37  # Right / Left in the tab control
        win.post(handle, pid, 0x0100, key, 1)
        win.post(handle, pid, 0x0101, key, 0xC0000001)
        time.sleep(0.12)
    raise NativeFailure("A native tab could not be selected.")


def capture_controls(win, controls, pid):
    by_handle = {item["handle"]: item for item in controls}
    records = []
    for item in controls:
        if item["class"] not in CONTROLS:
            continue
        ancestors = []
        ancestor = by_handle.get(item["parent"])
        while ancestor:
            ancestors.append({key: ancestor[key] for key in ("class", "text")})
            if ancestor["class"] == "TTabSheet" and not ancestor["visible"]:
                break
            ancestor = by_handle.get(ancestor["parent"])
        else:
            record = {key: item[key] for key in ("class", "text", "visible", "enabled", "rect")}
            record["ancestors"] = ancestors
            if item["class"] in COMBO_BOXES and item["visible"]:
                record["options"] = win.combo_items(item["handle"], pid)
            if item["class"] in CHECK_BOXES:
                record["checked"] = win.send(item["handle"], pid, 0x00F0) == 1  # BM_GETCHECK
            records.append(record)
    return records


def page_count(win, control, pid):
    count = win.send(control["handle"], pid, 0x1304)  # TCM_GETITEMCOUNT
    if not 0 <= count <= 32:
        raise NativeFailure("VEConfigure returned an unexpected tab count.")
    return count


def selected_page(controls, tabs, index):
    sheets = [item for item in controls if item["class"] == "TTabSheet" and item["visible"] and
              item["parent"] == tabs["handle"]]
    if len(sheets) != 1:
        raise NativeFailure("Could not identify the selected VEConfigure tab.")
    return sheets[0]["text"] or str(index + 1)


def capture_pages(win, main, pid, report):
    controls = offline_controls(win, main, pid)
    tabs = [item for item in controls if item["class"] == PAGE_CONTROL and item["visible"]]
    if not tabs:
        raise NativeFailure("VEConfigure exposed no visible setting tabs.")
    primary = max(tabs, key=area)
    primary_count = page_count(win, primary, pid)
    if not primary_count:
        raise NativeFailure("VEConfigure exposed no setting pages.")
    pages = report["pages"] = {}
    for index in range(primary_count):
        report["stage"] = f"native_page_index/{index}"
        move_to(win, primary, pid, index)
        controls = offline_controls(win, main, pid)
        label = selected_page(controls, primary, index)
        report["stage"] = "native_page/" + label
        pages[label] = capture_controls(win, controls, pid)
        nested = [item for item in controls if item["class"] == PAGE_CONTROL and item["visible"] and
                  item["handle"] != primary["handle"]]
        if nested:
            sub = max(nested, key=area)
            for sub_index in range(page_count(win, sub, pid)):
                report["stage"] = f"native_page/{label}/{sub_index}"
                move_to(win, sub, pid, sub_index)
                controls = offline_controls(win, main, pid)
                sub_label = selected_page(controls, sub, sub_index)
                pages[f"{label}/{sub_label}"] = capture_controls(win, controls, pid)


def run(fixture, exe, report, isolated):
    report["stage"] = "fixture_validation"
    with fixture.open("rb") as handle:
        data = handle.read(MAX_BACKUP_BYTES + 1)
    validate_synthetic(data)
    report["fixture_sha256"] = digest(data)
    report["stage"] = "worker_isolation"
    if not isolated or os.name != "nt" or os.environ.get("RUNNER_ENVIRONMENT") != "github-hosted":
        raise Blocked("Native capture requires an isolated, GitHub-hosted Windows worker.")
    report["stage"] = "native_binary"
    if not exe.is_file():
        raise Blocked("VEConfig.exe is unavailable on the Windows worker.")
    report["veconfigure_sha256"] = digest(exe.read_bytes())
    if report["veconfigure_sha256"] != EXPECTED_EXE:
        report["expected_veconfigure_sha256"] = EXPECTED_EXE
        raise Blocked("This VEConfig.exe build has no verified offline capture driver.")
    report["stage"] = "desktop"
    win = Windows()
    report["stage"] = "serial_isolation"
    remove_vm_serial_console()
    win.check_serial()
    report["stage"] = "network_isolation"
    check_firewall(exe)
    with tempfile.TemporaryDirectory(prefix="brain-zero-native-") as work:
        copy = Path(work) / "synthetic.rvsc"
        copy.write_bytes(data)
        env = {key: os.environ[key] for key in ("SystemRoot", "WINDIR") if key in os.environ}
        env.update(TEMP=work, TMP=work, USERPROFILE=work, APPDATA=work, LOCALAPPDATA=work)
        report["stage"] = "native_launch"
        process = subprocess.Popen([str(exe), "/F" + str(copy)], cwd=work, env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            report["stage"] = "offline_file_open"
            main = wait_for_file(win, process, report)
            report["stage"] = "native_pages"
            capture_pages(win, main, process.pid, report)
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        if copy.read_bytes() != data:
            raise NativeFailure("VEConfigure unexpectedly changed the disposable file.")
    report.update(status="completed", stage="complete", offline_file_mode=True)


def validate_synthetic(data):
    # Import only here so the rest of the driver can be checked on any host.
    from prepare_veconfigure import build_synthetic_rvsc

    bundle = export_compatibility(data)
    normalized, _ = validate_profile(json.dumps(bundle).encode())
    expected, _ = build_synthetic_rvsc(normalized)
    if data != expected:
        raise ValueError("The fixture must be the deterministic, credential-free synthetic export.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--exe", type=Path, default=EXE)
    parser.add_argument("--isolated", action="store_true", help="Confirm this is a disposable Windows worker")
    args = parser.parse_args()
    report = {"evidence": "native_veconfigure_synthetic_defaults"}
    try:
        run(args.fixture.resolve(), args.exe.resolve(), report, args.isolated)
    except Blocked as error:
        report.update(status="blocked", reason=str(error))
    except (ValueError, NativeFailure) as error:
        report.update(status="failed", reason=str(error))
    except (OSError, KeyError, subprocess.SubprocessError) as error:
        report.update(status="failed", reason=f"The worker could not complete this stage ({type(error).__name__}).")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"Native VEConfigure capture {report['status']} at {json.dumps(report['stage'])}.")
    return int(report["status"] != "completed")


if __name__ == "__main__":
    sys.exit(main())
