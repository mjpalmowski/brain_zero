# MultiPlus-II settings download investigation — 11 November 2026

Stock Venus completed all 35 simulated downloads. The production brain_zero
reader reproduced every native file byte-for-byte on both Windows and the
isolated ARM Venus Pi, with 22,628 read requests and zero write requests per
architecture. The tested download format is driven by live descriptors and
variable-length memory reads, rather than a separate protocol for each model.

## Purpose and scope

Observe the installed Venus OS download client against isolated virtual
MultiPlus-II units, then compare its backups with the production brain_zero
reader and serializer. Downloading a settings backup must remain available
independently of qualification for settings writes or ESS control.

The matrix covers all 35 MultiPlus-II product prefixes identified in the pinned
[public firmware archive](https://updates.victronenergy.com/feeds/VEBus/firmware-archive/).
A fresh checksum-validated metadata scan of all 116 available `26xx`/`27xx`
prefixes found the same 35-prefix set. This is a product-prefix protocol experiment,
not a test of every firmware revision or every physical model. Twelve cases use
descriptors from genuine exports for that prefix. The other 23 deliberately
borrow descriptors from the same DC voltage class and retarget the identity.
Those cases reveal how the native download client treats that identity; they
cannot establish the target model's real limits or capabilities. Every case
uses synthetic unit identity, standalone topology, descriptor-default values
and blank Assistant memory. The 120 V and 2×120 V cases do not add control
support to brain_zero.

## Isolation and reproducibility

The test runs the Pi's installed Venus OS v3.64 `mk2-dbus` and `mk2vsc`, rather
than a replacement download implementation. Each case creates fresh mount and
network namespaces, a private `/dev` with a simulated PTY, a private D-Bus and
scratch localsettings. The runner then drops to UID/GID 65534. Physical serial
devices and the system D-Bus socket are hidden. The simulator refuses settings,
RAM, Assistant, reset and inverter-configuration mutations; the native client
is invoked only with `-r`.

The physical MK3, live inverter, Rev C and Home Assistant are not used. No OTA
update or inverter write is part of this investigation.

Pinned binaries:

| Binary | SHA-256 |
| --- | --- |
| `mk2-dbus` | `bb4f66da146af7bf63f145f475c06970de50b4114c2dbb6a526dc1ad48bb6a55` |
| `mk2vsc` | `7c40593543ac864bac1dcb8cfdeb7d81cdd73c5b7f96a2221b93d56fd91d1280` |

Local tooling is in `tools/vebus_sim`: `prepare_backup_matrix.py` prepares
sanitized fixtures, `venus_backup_matrix.py` captures native requests and
replies, and `rvsc_rebuild.cpp` compares production parsing and serialization
against the native file. `backup_requester_replay.cpp` separately exercises
the actual reader with framed replies; `backup_replay_worker.py` runs it inside
the Pi sandbox. These development-workspace tools are separate from the
shipping ESP32 firmware. The full request captures and synthetic backups are
retained under `out/venus-backup-matrix-20261111`; original owner exports are
not copied into these evidence files.

## What the native client reads

The core backup sequence uses the same commands across the tested model
identities. Background driver queries can interleave with the download, so
phase counts are not an attribution of every request to `mk2vsc` alone:

| Read | Purpose and observed behavior |
| --- | --- |
| Extended `21 <unit ID> 05` | Unit/model and topology information. |
| Winmon `05` | Full firmware/version reply, including extra version bytes. |
| Winmon `3B` | Unit serial. The test supplies a synthetic value. |
| Winmon `36` | RAM-variable metadata probing, IDs 0–253 in the native download phase. These probes are not EEPROM writes. |
| Winmon `35` | Setting descriptors, IDs 0–253 in the native download phase. A supported long reply carries ten bytes of numeric metadata plus its access byte; an unsupported slot returns an empty descriptor. |
| Winmon `3C` | Two replies per selector for the opaque flag/access blocks, selectors `00`, `01`, `3C`, `3D`, `52`. |
| Winmon `31` | Current raw setting words for supported settings. The resulting file contains 192 setting slots, including zeros for unsupported slots. |
| Winmon `57` | Grid capability data in 64-byte records transferred in chunks of up to 11 bytes. Partial final records and the explicit end response matter. |
| Winmon `5F` | Assistant memory. Empty in-range records and out-of-range records have different replies. For blank memory the native client probes the boundary rather than downloading a fixed 2,816 bytes. |

The native file uses `Mk2vscInfo`, schema version 4 `BareSettingInfo`, and unit
version 3 `BareSettingData`, with section lengths and checksums. The schema
contains setting scale, offset, default, minimum, maximum, access bytes,
flag/access blocks and grid capability data. The unit section holds identity,
firmware, saved raw words and Assistant data encoded as literal/fill runs.

The extra shore-limit packet `3F 00 11 00 00` is a read. Its interpretation was
cross-checked against VictronConnect's `vebus.dll` GetShoreLimit implementation
(SHA-256 `ecd55b74f05503ca61728fa5007fe1cbbe49d660d86eeaf59d95fdb3e84679cb`).
The test permits only that exact packet; neighboring variants remain refused.
This extended packet is not described by the published MK2 protocol document.

## Consequences for brain_zero

The full backup reader should use the connected unit's replies and preserve
opaque fields. A firmware/model allowlist is unnecessary for a read-only
backup when all required replies are understood and complete. This does not
grant permission to restore that file, edit settings, alter Assistants or run
ESS. Those operations retain their separate live identity, descriptor and
control checks.

Descriptor completeness is now checked identically for qualified and
unqualified firmware. The supported long reply must include all five numeric
fields and the access byte: at least 14 bytes including the direct-bus `00` and
request ID. Only the explicit three-byte `00 <ID> 89` reply marks a setting
unsupported. A truncated or wrong-command reply fails that setting and blocks
the backup instead of inventing access flags or silently omitting a setting.
This follows the long-format layout in
[MK2 protocol section 7.3.8](https://www.victronenergy.com/upload/documents/Technical-Information-Interfacing-with-VE-Bus-products-MK2-Protocol-3-14.pdf)
and the observed native replies. Extra trailing bytes remain allowed, as the
protocol's extension guidance requires. This does not add support for the
separate short Winmon descriptor sequence.

The native client scans beyond the 192 EEPROM slots saved in this file format.
brain_zero reads the 192 saved slots rather than duplicating native RAM
metadata probing. A byte-identical serializer comparison checks the saved
format; it does not by itself test reader timing or prove that future firmware
cannot add settings beyond this format.

Observed grid lengths include 1,463, 1,536, 1,761, 1,772 and 1,792 bytes.
Assistant capacities include 2,816 and 4,096 bytes. These fit brain_zero's
current 3,072-byte grid and 4,096-byte Assistant limits. Larger future areas
must fail explicitly rather than truncate into a plausible-looking backup.
An incomplete descriptor, missing value, missing block or unknown memory end
must also prevent the download.

## Results

All 35 native downloads passed. All cases reported UID 65534, fresh private
mount/network namespaces, no physical serial devices or system D-Bus, no
attempted inverter mutations and no changed settings, RAM, Assistant data or
unit configuration. Adapter addressing and receive-mode negotiation remain
ordinary discovery/session operations.

Each native file passed its section checksums and matched its fixture's exact
catalogue, default setting words, grid bytes and blank Assistant capacity.
The files were 4,242–4,571 bytes; native download time was 47.02–48.74 seconds.
All used schema 4/unit 3. Thirty-four cases had 145 supported saved settings;
one had 144. Every supported long descriptor retained its eleventh access byte.

Three independent production comparisons passed for all 35 files:

1. Parse the native file, reconstruct a completed snapshot, and serialize it
   with production `rvsc.cpp`: all 35 files match byte-for-byte.
2. Feed framed replies through production `Requester::read_settings`, then
   serialize the result on Windows: all 35 files match byte-for-byte.
3. Compile the same production reader on ARM and repeat inside the isolated
   Venus Pi sandbox at UID 65534: all 35 files match byte-for-byte.

The reader replay uses the native file's saved timestamp for comparison and
matches the simulator's exact record padding and end responses. It exercises
partial final grid records and all 64 records of a 4,096-byte Assistant area.
Each architecture emitted 22,628 read requests and zero writes.

The descriptor completeness regression rejects missing access bytes, truncated
numeric fields, short-mode fragments and wrong reply types. Complete protected
descriptors retain their access flags; explicit absent descriptors and future
trailing bytes still work. The requester and RVSC suites, UI download test and
16 Python tests (23 additional subtests) pass. The ESP32-S3 firmware build
passes with 110,408 bytes static RAM, 33.7% of 320 KiB. This is build-time RAM
accounting, not a new runtime measurement. No firmware has been installed on Rev C.

Evidence: [per-prefix results](evidence/2026-11-11/venus-backup-matrix.json) and
[official archive coverage](evidence/2026-11-11/mp2-archive-coverage.json).

## Per-prefix matrix

Every row passed the native download, serializer and production reader comparisons.
**G** means genuine descriptors for this prefix and the stated firmware.
**B** means borrowed descriptors; the donor firmware is shown explicitly.
Archive labels identify firmware families and may differ from the product label on an older unit.

| Prefix | Archive family label | Simulated firmware | DC / AC V | Descriptor donor | Grid bytes | Assistant bytes |
| --- | --- | --- | --- | --- | --- | --- |
| `2601` | MultiPlus-II 48/4500/55-32 HIPLess | `2601560` | 48 / 230 | G: `2601560` | 1792 | 2816 |
| `2602` | MultiPlus-II 48/6500/100-50 HIPLess | `2602560` | 48 / 230 | G: `2602560` | 1792 | 2816 |
| `2604` | MultiPlus-II 12/5000/220-50 (HIP-less) | `2604556` | 12 / 230 | B: `2609510` | 1761 | 4096 |
| `2605` | MultiPlus-II 12/3000/120-32 (ACOut2 switchable etc) | `2605556` | 12 / 230 | B: `2609510` | 1761 | 4096 |
| `2611` | MultiPlus-II 24/3000/70-32 (ACOut2 switchable etc) (+ EasySolar) | `2611552` | 24 / 230 | G: `2611552` | 1761 | 4096 |
| `2615` | MultiPlus-II 24/5000/120-50 (ACOut2 switchable etc) (+ EasySolar) | `2615498` | 24 / 230 | G: `2615498` | 1463 | 4096 |
| `2619` | MultiPlus-II 48/15000/200-100 | `2619556` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2621` | MultiPlus-II 48/8000/110-100 | `2621556` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2623` | MultiPlus-II 48/5000/70-50 (ACOut2 switchable etc) | `2623563` | 48 / 230 | G: `2623563` | 1792 | 2816 |
| `2625` | MultiPlus-II 48/3000/35-32 (external sensor max 32A) | `2625556` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2626` | MultiPlus-II 48/5000/70-50 | `2626556` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2627` | MultiPlus-II 48/10000/140-100 | `2627508` | 48 / 230 | G: `2627508` | 1536 | 4096 |
| `2628` | MultiPlus-II 48/3000/35-32 | `2628556` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2629` | MultiPlus-II 48/3000/35-32 (ACOut2 switchable etc) (+EasySolar) | `2629558` | 48 / 230 | G: `2629558` | 1792 | 2816 |
| `2630` | MultiPlus-II 12/4000/180-32 (ACOut2 switchable etc) | `2630559` | 12 / 230 | B: `2609510` | 1761 | 4096 |
| `2636` | MultiPlus-II 48/4500/60-32 HIPLess LowTrafo Ratio | `2636570` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2637` | MultiPlus-II 48/6500/100-50 HIPLess LowTrafo Ratio | `2637570` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2682` | MultiPlus-II 48/8000/110-100 HIPLess | `2682560` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2683` | MultiPlus-II 48/10000/140-100 HIPLess | `2683558` | 48 / 230 | G: `2683558` | 1792 | 2816 |
| `2684` | MultiPlus-II 48/15000/200-100 HIPLess | `2684560` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2696` | MultiPlus-II 24/5000/120-50 HIPLess (ACOut2 switchable etc) (+ EasySolar) | `2696559` | 24 / 230 | B: `2611552` | 1761 | 4096 |
| `2697` | MultiPlus-II 24/3000/70-32 HIPLess (ACOut2 switchable etc) (+ EasySolar) | `2697557` | 24 / 230 | B: `2611552` | 1761 | 4096 |
| `2698` | MultiPlus-II 48/5000/70-50 HIPLess (ACOut2 switchable etc) | `2698559` | 48 / 230 | B: `2623563` | 1792 | 2816 |
| `2699` | MultiPlus-II 48/3000/35-32 HIPLess (ACOut2 switchable etc) (+EasySolar) | `2699562` | 48 / 230 | G: `2699562` | 1792 | 2816 |
| `2704` | MultiPlus-II 12/4k/160-50 120V (ACOut2 switchable etc) | `2704568` | 12 / 120 | B: `2780556` | 1772 | 2816 |
| `2705` | MultiPlus-II 12/3000/120-50 120V (ACOut2 switchable etc) | `2705552` | 12 / 120 | G: `2705552` | 1761 | 4096 |
| `2711` | MultiPlus-II 24/3000/70-50 120V (ACOut2 switchable etc) | `2711556` | 24 / 120 | B: `2611552` | 1761 | 4096 |
| `2713` | MultiPlus-II 24/3000/70-50 120V HIPLess (ACOut2 switchable etc) | `2713559` | 24 / 120 | B: `2611552` | 1761 | 4096 |
| `2715` | MultiPlus-II 24/5000/120-95 120V (ACOut2 switchable etc) | `2715556` | 24 / 120 | B: `2611552` | 1761 | 4096 |
| `2723` | MultiPlus-II 48/5000/70-95 120V (ACOut2 switchable etc) | `2723556` | 48 / 120 | B: `2729560` | 1792 | 2816 |
| `2729` | MultiPlus-II 48/3000/35-50 120V (ACOut2 switchable etc) | `2729560` | 48 / 120 | G: `2729560` | 1792 | 2816 |
| `2775` | MultiPlus-II 24/3000/70-50 2x120V | `2775556` | 24 / 120 | B: `2611552` | 1761 | 4096 |
| `2777` | MultiPlus-II 24/3000/70-50 2x120V HIPLess | `2777557` | 24 / 120 | B: `2611552` | 1761 | 4096 |
| `2779` | MultiPlus-II 12/4k/160-50 2x120V | `2779568` | 12 / 120 | B: `2780556` | 1772 | 2816 |
| `2780` | MultiPlus-II 12/3000/120-50 2x120V | `2780556` | 12 / 120 | G: `2780556` | 1772 | 2816 |

## Remaining hardware evidence

Genuine descriptors describe their original prefix and firmware only.
Retargeting another model's descriptors does not discover a missing model's
real ranges. We still need current exports and read captures for the borrowed
cases, other firmware revisions, populated Assistant programs, actual 2×120 V
topology and failure/timing behavior on physical units. Parallel and three-phase
systems are outside this single-unit experiment.

Do not promote settings-write or ESS compatibility solely because a native
simulator download passed. Model prefixes not yet mapped from an official
product listing remain inventory gaps. In particular, the current manual lists
[48/10k/150-100](https://www.victronenergy.com/media/pg/MultiPlus-II_4k_4k5_6k5_10k_230V/en/technical-specifications-mp-ii-k-230v.html),
whose prefix has not been confirmed. It must not be equated with the older
48/10000/140-100 family on the strength of the similar power rating.

## Genuine export search: 12 November 2026

The follow-up search covered all 23 prefixes marked **B** above. It found one
genuine legacy catalogue, for **2628459**, but no complete replacement fixture.
The other 22 prefixes still lack a matching genuine export from the sources
searched. A search miss does not establish that no file exists.

The published evidence is a [per-prefix search ledger](evidence/2026-11-12/genuine-export-search.json)
and a [sanitized observed 2628459 catalogue](evidence/2026-11-12/observed-2628459-legacy-catalogue.json).
Original exports, private file paths, post bodies, device serials, unique unit
IDs, saved setting values, credentials and Assistant programs are not published.
Only sanitized metadata reached the Pi. No live inverter, Rev C, physical MK3
or Home Assistant connection was used for this search or its tests.

### Search coverage

The search used external indexes and public forum search/topic APIs, including
Victron's current forum and archived community, DIY Solar Forum, Solarforum,
MyPower, Photovoltaikforum, Akkudoktor, RV/installer resources and public GitHub
repositories and issues. Full post streams were inspected, including posts
outside the first response page. Attachments renamed as JPG, PDF, TXT or DOC,
and ZIP members, were checked by content and section checksums. A name or
retail-model mention alone was never counted as a match.

| Search group | Prefixes | Logged web queries | Forum query attempts | Complete topic streams | Posts inspected | Candidate URLs checked |
| --- | --- | --- | --- | --- | --- | --- |
| Older/high-power 230 V | 2619, 2621, 2625, 2626, 2628, 2682, 2684 | 48 | 11 | 185 | 2,247 | 45 |
| Newer/low-voltage 230 V | 2604, 2605, 2630, 2636, 2637, 2696, 2697, 2698 | 48 | 18 | 25 | 389 | 12 |
| 120 V and 2×120 V | 2704, 2711, 2713, 2715, 2723, 2775, 2777, 2779 | 80 | 16 | 10 | 143 | 6 |

These are per-group work counts, with overlap, not a count of unique global
queries or posts. Forum query attempts include recorded rate-limit responses.
Requests were then coordinated through a shared six-second interval and
server-requested backoff. Access controls were not bypassed. Two potential
Solarforum attachments require membership; their actual firmware is unknown,
and the user has no account there. They remain inaccessible candidates.

The existing local corpus was also checked: 1,624 candidate files, 505 unique
hashes, plus an independent legacy section/checksum audit of parse failures.
It contained no additional genuine match for the 23 gaps. The rejected TXT
and ZIP candidates in the newer-model search were audited again without
assuming modern section order, so an old-format export was not discarded just
because the current parser rejected its layout.

### Genuine 2628459 finding

The [archived current-transformer discussion](https://communityarchive.victronenergy.com/questions/15221/current-transformer-multiplus-ii.html)
contains a file disguised as a JPG. Its actual contents are a checksum-valid
VEConfig settings container for the **2628** product family, firmware **459**:

- `BareSettingInfo` version 2 and `BareSettingData` version 2; no producer section.
- 192 numeric descriptor slots, of which 142 are supported, plus 192 actual
  access bytes and 80 flag/access bytes.
- A 35 A maximum charger-current descriptor and 4,096-byte Assistant capacity.
- Eight stored firmware/version bytes, rather than the ten retained by the
  modern format, and no grid-information blob.

The source SHA-256 is
`e39476aaa041d2987e7f227083448c68883767e11597c50851ecfc7a8cc13e26`.
The catalogue differs from the previous borrowed 2623563 table at 79 descriptor
IDs. That comparison changes both model and firmware revision; it does not
attribute every difference to hardware. In particular, the borrowed table's
70 A charger limit cannot be substituted for this genuine 35 A limit.

The old writer may have discarded version bytes that were present on the wire.
Absence of a grid blob likewise does not prove that the inverter lacks grid
information. Those fields stay **unknown**, not zero-filled.

### Isolated Venus test and completeness check

The dedicated probe replayed the eight stored version bytes, all genuine
numeric/access descriptors and flag blocks. Unit identity, topology, saved
words, blank Assistant contents and telemetry were explicitly synthetic. It
supplied no invented grid information or version padding. The Pi again ran
the pinned stock Venus v3.64 binaries in private mount/network namespaces as
UID 65534, with physical serial devices and system D-Bus hidden.

The driver connected as product `0x2628`, firmware `0x459`. During the native
download it read all 142 supported descriptors and values, the explicit absent
descriptors and five flag pairs correctly. It then requested grid record 0,
chunk 1 (`57 01 00`) five times. Because that data was not in the source file,
the harness deliberately withheld it. Native `mk2vsc` ended with exit 30 and
did not produce a complete valid backup. There were **zero write attempts or
configuration changes**.

This is an **inconclusive fixture-evidence gap**, not a failure demonstrated on
a physical 2628 inverter. Filling in another model's grid block or two zero
version bytes would erase the distinction this investigation is meant to test.

A separate offline production-reader check supplied exactly those eight bytes
in an 11-byte direct-bus reply. It retained the base firmware number but refused
to export an RVSC because the full firmware record was incomplete: zero output
bytes and zero write commands. Other replies in that negative test were
explicitly synthetic; it is a completeness check, not a faithful reconstruction
of the physical legacy inverter.

### Qualification outcome

No borrowed fixture was promoted to complete genuine download qualification,
and no settings-write or ESS support was added. Prefix 2628 now has genuine
legacy descriptor evidence, while the other 22 remain source gaps. A newer
genuine export or a read-only capture supplying the missing 2628 metadata is
needed before rerunning a complete case. For the other prefixes, obtain an
owner export containing the actual matching firmware/schema rather than a
renamed, retargeted or same-voltage donor.
