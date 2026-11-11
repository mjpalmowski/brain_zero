# brain_zero Home Assistant integration

Local control of the brain_zero S3 through Home Assistant's MQTT integration.
It uses the controller's existing validation and VE.Bus command handling.
The integration is independent of Home Assistant's built-in Victron GX integration
and carries its own approved brain_zero artwork.

## Installation

Connect brain_zero and Home Assistant's MQTT integration to the same broker.
Copy this directory to `/config/custom_components/brain_zero` and restart Home
Assistant. For a new entry, add **brain_zero** under **Settings → Devices &
services** and enter the **Controller ID** shown on the board's MQTT page. The
optional web address and port are used for links to the board's console.
Requires MQTT control firmware and Home Assistant 2026.10 or later.

Existing entries connect over MQTT after restart and retain their entity IDs.
Home Assistant uses its existing broker credentials. Once the entry connects,
the integration removes its old web API token.

The integration registers three devices and 122 entities:

* Controller: ESS modes/limits, five charging schedules, overnight plan,
  free-power draft and explicit arm/cancel actions, confirmation and diagnostics.
* MultiPlus-II: AC/DC telemetry, operating mode and input current limit,
  stored charge current/absorption/float settings, PowerAssist, assist current
  boost factor and explicit inverter readback.
* PACE battery: fresh battery SOC and voltage.

The five equalization/restart/redetect buttons and controller restart button are
disabled by default. The inverter buttons map
only to existing S3 actions. Do not enable or exercise them casually.

## Traffic and command handling

The controller subscribes to `brain_zero/<controller-id>/request` and replies
on `brain_zero/<controller-id>/reply/<request-id>`. With broker topic permissions,
allow Home Assistant to publish requests and subscribe to replies, and give the
controller the reverse permissions. Both use non-retained messages. Writes
carry a short UTC expiry and require the controller's clock to be synchronized.

All entities share one coordinator. Every five seconds it reads cached telemetry
and at most one due settings/status endpoint. Nominal auxiliary intervals are
15 seconds for inverter/overnight status and 30 seconds for ESS/free-power;
they can be longer when several become due together. No entity polls on its own.
Cold setup performs a bounded serial refresh, one stored charger read and one
PowerAssist settings read. Neither parameter group is periodically read.

There is one MQTT request in flight, at least 500 ms between request starts,
a five-second timeout, and connection-failure backoff up to 60 seconds. Concurrent
user commands are rejected, and command starts are separated by two seconds.
Verification temporarily uses one-second readback polling with a finite timeout;
Writes are never automatically retried. Idle charger polling is deliberately
absent because that endpoint can initiate VE.Bus parameter reads.

An ESS control confirms the S3's saved value; this is distinct from proving an
electrical outcome. Operating mode/input limit require inverter command
acknowledgement and live value readback. Charger writes first fetch fresh
parameter metadata and compare the raw base values, then require verified raw
readback of all three parameters. A no-op does not rewrite stored settings.
Overnight/free-power plans additionally check the R4875 peer where applicable.
`Last command result` explains pending or failed confirmation.

Disabled power limits have separate enable switches; editing their numbers while
disabled saves a preset in HA without enabling the limit. Free-power date/power
edits similarly save a draft in HA: **Arm saved free-power draft** applies it.
Use **Reload free-power draft** after another client changes the saved session.
Overnight and five-slot schedule fields apply when edited. Use the atomic
services to change several related fields together.

Services: `set_ess`, `set_schedule`, `set_schedules`, `set_overnight`, `set_charger`,
`set_assist`, `restart_controller`, `set_free_power`, `cancel_free_power`, `refresh`,
`get_snapshot`. They accept an optional
`entry_id`, required if several controllers are configured. Free-power service
dates must include their timezone offset. Schedule clocks are controller-local
time; schedule duration in the service is seconds (entity display is minutes).

Legacy firmware retains its controller-side charging execution until the
Home Assistant charging adapter is enabled and unified firmware is detected.
Existing MQTT discovery, dashboard entity IDs and calendar scheduling continue.
To roll back this integration, disable its config entry; after removing that
entry the component folder can be removed and HA restarted.

## Home Assistant charging adapter (0.6.0)

Configure the existing integration entry under **Settings → Devices & services →
brain_zero → Configure**. Enable **Manage charging plans in Home Assistant** and
enter the battery-link and optional external-charger host/pairing-key pairs.
Pairing keys live only in Home Assistant options and are omitted from diagnostics
and dashboard snapshots. Leave a key blank to retain its current value. The
controller itself continues to use Home Assistant's MQTT connection; no controller
web API token is needed.

The options keys are `charging_adapter` (boolean), `battery_link_host`,
`battery_link_token`, `r4875_host`, and `r4875_token`. Hosts accept a DNS name or
IPv4 address; pairing keys are 32–128 printable characters. The battery source
must implement the existing authenticated battery-link UDP protocol on port
41903. The optional charger must implement the paired `/api/recharge` and
`/api/free-power` endpoints. External charger execution remains on that charger;
Home Assistant does not introduce a second charger on/off timer.

For migration, enable the adapter while legacy firmware is still installed.
It imports the current overnight and dated free-power plans into Home Assistant's
Store and keeps using the legacy controller endpoints. Confirm
`brain_zero.get_snapshot` reports `charging_adapter.plans_imported: true` before
installing unified firmware. Only the new `battery-input` capability activates
the adapter; unsupported firmware receives no battery-input or lease writes.
Stored plans, completion markers and lease ownership survive Home Assistant
restarts. Existing entity unique IDs, services, drafts and snapshot section names
are retained.

On unified firmware, authenticated battery SOC/voltage measurements are forwarded
as non-retained MQTT messages to the topic returned by `battery-input`. Random
challenges, HMAC verification, original CAN ages and conservative round-trip
timing prevent stale battery data becoming fresh. SOC expires after 10 seconds;
external charger current expires after 6 seconds. Current is derived from fresh
authenticated charger DC watts and fresh battery voltage, with its original HTTP
observation time. Each field's timestamp is forwarded once; an unchanged field
cannot make a new field fail replay validation. The SOC-aligned entity requires
the controller's fresh post-broadcast inverter readback, rather than a transmit
echo.

Explicit overnight-plan edits compare the current native scheduled charge level
1 against the last mapped plan, change only that slot and its grid-after-target
flag, and verify readback. Other schedules and the global charge limit stay under
their existing controls. The native overnight window remains the fallback when
Home Assistant is offline. A lower requested overnight power cap uses a temporary
ESS lease; the controller's global cap still applies. At the target, the lease
can hold loads on grid until the window ends. Plans use their saved local time
zone, including both daylight-saving transitions.

Dated free-power sessions use the same generic `ess/lease` interface. Leases
expire after 20 seconds and renew every 5 seconds while the session is active.
The session ends by clearing only its own lease; loss of Home Assistant or MQTT
lets it expire automatically and restores the controller's saved ESS behavior.
The ordinary native overnight window can continue if it is independently active.
Completed or cancelled dated sessions are not replayed after restart or a clock
correction. No temporary session changes the saved ESS baseline.

An external-charger plan is written once per explicit edit with revision checks;
confirmation retries only read it back. A fresh matching plan and valid charger
clock are required for the existing verified-plan entities. Pending confirmation
is reported without repeatedly sending plan writes. The charger's own saved
plans continue operating when Home Assistant is offline.

## Protocol boundaries

No arbitrary bus command, raw register write, firmware upload, remote console,
VEConfigure file upload, protection/grid-code configuration, or invented BMS/DVCC
control is exposed. Those require separate work and, where the existing captures
do not establish the transaction and acknowledgement, a Venus + MK3 bus capture.
Keep the physical inverter isolated from competing bus masters during captures.

## Ignore AC input (0.3.0)

Ignore AC input (0.3.0) adds nine native entities and a protected
`brain_zero.set_ac_input` service. The user-selected reconnect floor is 10%; a
higher ESS reserve applies. The dashboard exposes separate requested, actual
ignore and derived grid-acceptance states, plus SOC, backup voltage and duration
settings. Charging schedules, stale data and local battery protection cancel the
manual request on the S3. HA is not required for automatic reconnection. Status
rides the existing five-second state request.
The [integration regression tests](../../tests/test_brain_zero.py) cover command
validation, confirmation and stale-state handling.

## PV-disable (0.4.0)

`set_pv_disable` accepts `disable`, `seconds` and an optional `base` revision.
The saved local duration is 10–300 seconds (default 60). Saving duration does not
activate PV-disable or extend an active lease. Six native entities expose the
request switch, independent inverter readback, state/reason, remaining time and
duration. The S3 Inverter page and MultiPlus Controls offer the same control.

Activation requires confirmed local Ignore AC island operation and fresh safe
inverter/battery data. It raises AC-out frequency to about 53 Hz, affecting all
connected appliances. Automatic release runs on the S3, including on timeout,
charging priority, stale data, protection or reconnection; after another master
takes over, release is verified when S3 ownership returns. Requests are not saved
or replayed after restart. A clear flag allows solar to restart; it does not
guarantee immediate production. The supported firmware is the captured 2623563.

All state comes from the existing shared five-second coordinator. Writes remain serialized,
CAS guarded, and never retried automatically by HA. Completion requires a fresh
actual RAM flag in the readback. The
[integration regression tests](../../tests/test_brain_zero.py) cover activation,
confirmation and automatic-release status.

## PowerAssist and controller restart (0.2.0)

PowerAssist uses documented stored setting 0, bit 5; boost factor uses setting
48 with the inverter-reported scale and range. Each save reads fresh metadata,
checks the original full raw values, changes only the allowed bit/value, sends
changed settings once and verifies exact full raw readback. A no-op performs no
EEPROM writes. The dashboard offers a draft, explicit Read and reviewed Save.

`refresh` with section `assist` explicitly refreshes those stored parameters;
`set_assist` accepts `power_assist`, `boost_factor` and optional original raw CSV
`base`. Unknown metadata, access protection, stale original values and missing
readback reject the save. Controller restart verifies the same controller MAC
and a changed boot ID; it does not send an inverter restart command.

These controls use verified inverter profiles. See the
[supported models and firmware](../../../README.md#multiplus-ii-compatibility)
for current coverage; this does not claim complete Venus software-policy or
other-hardware compatibility.

AC-in watts are import-positive. AC-out watts retain their signed terminal value.
MultiPlus DC power excludes the mains-fed R4875 chargers. These terminal readings
are not the incoming-tail CT reading or whole-house consumption. External battery
readings become unavailable when stale. brain_zero retains native ESS control
and battery protection; external-charger plans execute on the paired charger,
coordinated through the Home Assistant charging adapter described above.

`brand/` contains PNG versions of the approved SVGs in `assets/brand`. Original
hardware manufacturer names remain accurate on the inverter and battery devices.

## Dashboard cache and batch saves (0.1.1)

`get_snapshot` returns a copied, credential-free view of the existing cache and
its ages/connection status. It performs no S3 request and does not reset the poll
timer or last-command result. Stored charger values include their read age.
`refresh` optionally accepts a `section`; `charger` explicitly reads parameters.
All saves return the confirmed cache for the dashboard to display.

`set_schedules` accepts `windows` and `base`, each exactly five six-field schedule
objects. It rejects changed original settings, sends only changed schedule fields
in one write, and verifies the whole schedule set. `set_overnight` optionally takes
`base_id`; `set_charger` optionally takes the original raw CSV `base`. These guards
reject stale dashboard forms after a fresh controller/inverter read.
