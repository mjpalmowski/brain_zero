"""Bounded diagnostics without credentials or personal schedule dates."""
from time import monotonic


async def async_get_config_entry_diagnostics(hass, entry):
    c = entry.runtime_data
    return {"version": "0.6.0", "controller": c.identity,
            "traffic": {"requests": c.api.requests, "connection_errors": c.api.errors,
                        "backoff_remaining_s": round(max(0, c.api.retry_at - monotonic()), 1),
                        "live_interval_s": 5, "max_parallel_requests": 1, "min_request_spacing_s": .5},
            "cache_age_s": {key: round(monotonic() - seen, 1) for key, seen in c.fetched.items()},
            "last_command": c.last_command, "command_busy": c.command_lock.locked(),
            "controller_status": c.snapshot.get("state", {}).get("ctl", {}),
            "transport": "MQTT request/reply with cached snapshots"}
