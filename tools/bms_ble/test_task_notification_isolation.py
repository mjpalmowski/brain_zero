#!/usr/bin/env python3
"""Compile actual runtime callback bodies against the pinned NimBLE wait code."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


def main() -> None:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--nimble-utils", type=Path, default=root / "firmware/vebus_listener/.pio/libdeps/listener/NimBLE-Arduino/src/NimBLEUtils.cpp")
    args = parser.parse_args()
    runtime = (root / "firmware/vebus_listener/src/battery_ble.cpp").read_text(encoding="utf-8")
    calls = re.findall(r"\b(?:xTaskNotify\w*|ulTaskNotify\w*)\s*\(", runtime)
    assert not calls, f"Application runtime must not touch NimBLE's default notification slot: {calls}"
    assert "vTaskDelay(pdMS_TO_TICKS(20))" in runtime
    assert "vTaskDelay(pdMS_TO_TICKS(100))" in runtime
    assert "setConnectTimeout(CONNECT_TIMEOUT_MS)" in runtime
    assert "operation_deadline(CONNECT_GUARD_MS)" in function(runtime, "bool connect_link(")
    assert "link.index ? link.client->connect(peer, true, false, true) : connect_jk(link, peer)" in runtime
    assert "link.owner->links[0].state.load() == State::CLOSING" in runtime
    assert "link.owner->links[1].state.load() == State::CLOSING" in runtime
    callbacks = "\n".join(function(runtime, name) for name in (
        "void Callbacks::onConnect(", "void Callbacks::onConnectFail(", "void Callbacks::onDisconnect("))
    enqueue = function(runtime, "void enqueue(").replace("void enqueue(", "void Link::enqueue(", 1)
    upstream = args.nimble_utils.read_text(encoding="utf-8")
    waits = "\n".join(function(upstream, name) for name in (
        "bool NimBLEUtils::taskWait(", "void NimBLEUtils::taskRelease("))
    helpers = "\n".join(re.search(pattern, runtime).group(0) for pattern in (
        r"constexpr uint32_t OPERATION_MS\s*=.*?;", r"constexpr uint32_t CONNECT_TIMEOUT_MS\s*=.*?;",
        r"constexpr uint32_t CLOSE_GUARD_MS\s*=.*?;", r"constexpr int GAP_TIMEOUT_ERROR\s*=.*?;"))
    helpers += "\n" + "\n".join(function(runtime, name) for name in (
        "uint32_t operation_deadline(", "bool current_attempt(", "bool close_jk_attempt(",
        "bool fail_jk_attempt(", "bool connect_jk(", "bool quiescent("))
    cxx = str(Path(args.cxx).resolve()) if Path(args.cxx).exists() else args.cxx
    env = os.environ.copy()
    if Path(cxx).is_file():
        env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="brain-zero-notification-test-") as folder:
        work = Path(folder)
        (work / "actual_nimble_wait.inc").write_text(waits, encoding="utf-8")
        (work / "actual_ble_callbacks.inc").write_text(callbacks + "\n" + enqueue, encoding="utf-8")
        (work / "actual_runtime_helpers.inc").write_text(helpers, encoding="utf-8")
        executable = work / ("isolation.exe" if os.name == "nt" else "isolation")
        subprocess.run([cxx, "-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-I" + str(work), "-I" + str(root / "firmware/vebus_listener/src"),
                        str(root / "firmware/vebus_listener/test/battery_ble_notification_test.cpp"),
                        "-o", str(executable)], check=True, env=env)
        subprocess.run([str(executable)], check=True, env=env, timeout=10)


if __name__ == "__main__":
    main()
