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

### VEConfigure profile selection and Pi retest: 12 November 2026

A fresh, exact private copy of the genuine `2628459` export was opened in the
installed VEConfigure application, with its communication port confirmed
closed. The existing offline audit driver selected the offered **UK G83/2
August 2012 / G59/3-1 August 2014** profile and entered the previously audited
grid access password. The older-Assistant advisory was acknowledged with
**Ignore**, preserving the Assistant program. No settings were sent to a device.

The native save succeeded: the file grew from 3,666 to 3,706 bytes, gained a
producer section and one authorization record, and changed saved settings
`81`, `128`–`157` and `191`. The selected firmware profile code is **2**; the
experiment filename's suffix is not a grid code. This is a disposable test
selection, separate from any installed inverter configuration.

The decisive comparison is the settings schema: its complete body remained
**byte-identical**, including all numeric descriptors and access bytes. It
remained schema version 2 with **no grid-information blob**, and the unit still
stored exactly eight version bytes. The original export and Assistant program
were preserved. Thus protected legacy profile editing works in VEConfigure,
but selecting and saving a profile **did not recover the missing capability
metadata**. Changed grid setting values and authorization are separate from
the firmware-supplied grid table requested by Venus.

The Pi retest used the genuine descriptor metadata plus only the newly
selected `81 = 2` value. All other saved words were synthetic descriptor
defaults; identity/topology, blank Assistant contents and telemetry were also
synthetic. Neither the original saved user values nor the new authorization
record were copied to the Pi. No grid block, grid end marker or additional
version bytes were invented.

Stock Venus v3.64 connected as `0x2628` / `0x459`, read all 142 supported
descriptors and settings correctly, and read selector 81 back as 2. It then
requested `57 01 00` five times and ended with exit 30 because that grid data
remains unknown. There was no complete valid backup. The outcome remains
**inconclusive due to missing evidence**, not physical-inverter incompatibility.
The private mount/network namespaces ran as UID 65534 with physical serial
devices and system D-Bus hidden; write attempts and configuration changes were
both zero. The real inverter remained isolated.

The [sanitized experiment evidence](evidence/2026-11-12/veconfigure-2628459-grid-recovery.json)
records hashes, comparisons, readback and isolation checks. Its limits include
the harness-supplied absent replies beyond the 192-slot source table, existing
transport error counters without phase attribution, and the fact that the
native download never reached a complete Assistant-memory read. No additional
model/firmware, settings-write or ESS qualification follows from this test.

### brain_zero legacy export implementation: 12 November 2026

Build **20261112.1** adds a verified legacy `.rvsc` download path for
**2628459**, model byte **47**, with observed version prefix
`6b1b2800080d0000`. It exports the older schema 2 / unit 2 container that
VEConfigure already accepts, without a grid-information field. This is an
explicit file-format choice made before the optional grid read, not a claim
that the hardware lacks Winmon `0x57`. A timeout never selects this path.

VEConfigure's descriptor readers leave the missing grid table absent. Separate
compiled legacy logic provides profile handling. Static inspection and 100
bounded, memory-only native cases confirmed that dispatch without constructing
a grid table. The helper profile arguments and all runtime model globals are
not fully reconstructed, so those cases do not establish every GUI default or
a live-bus fallback contract. Actual offline UI tests are the acceptance proof
for the exported file format: both eight-byte and ten-byte version records
opened successfully, and a save preserved an explicit ten-byte test marker.
The extra two marker bytes are synthetic format evidence, not hardware data.

The production reader now retains the received version length and distinguishes
grid records from **omission by the legacy export format**. The writer preserves
all received eight or ten bytes, live numeric/access descriptors, flag blocks,
saved words and the complete Assistant area. It invents neither a grid record
nor an end-of-grid reply. The parser accepts bounded 2/2 containers with or
without the optional producer section; modern 4/3 exports retain their layout.
The settings API reports the received version length and grid-metadata state.

The initial synthetic fixture changed selector 81 without updating its grid
validity words. VEConfigure correctly offered to repair that combination. A
consistent fixture was then generated by VEConfigure itself from the wholly
synthetic seed, with owner settings and Assistant contents excluded. Embedded
authorization was stripped before transfer to the Pi. This repair belongs in
the configuration workflow: the backup reader preserves actual inverter words
even if a later editor considers their combination invalid.

The production Requester and codec were compiled for the Venus Pi's ARM CPU
and exercised with raw VE.Bus frames held in memory. Each consistent legacy
case completed **469 read commands, zero writes, zero grid requests and all
64 Assistant records**, then rebuilt its independent fixture exactly. Both
resulting files opened in native VEConfigure with the UK profile selected,
13 offered profiles and no repair prompt. The existing 35 modern simulator
fixtures also rebuilt exactly; their original genuine/borrowed provenance is
unchanged. This is a brain_zero reader test on the Pi, not a claim that stock
Venus `mk2vsc` can download the still-unobserved grid table.

Seventeen synthetic integration scenarios, codec corruption/bounds tests and
the existing requester, unsupported-backup, recorded-modern-backup and upload
regressions passed. Wrong firmware/model/signature, unsupported version
lengths, lost settings/flag blocks/Assistant data and modern grid timeouts
remain failures. The Pi workers ran as UID 65534 in private mount/network
namespaces, with physical serial devices and system D-Bus hidden.

This adds **legacy backup/download support**, not settings-write or ESS
qualification. Legacy upload plans are explicitly rejected, and
`compatibility.json` still requires actual device grid metadata rather than
invented descriptors. A physical 2628459 read capture is still needed to verify
its real version reply length and hardware timing. The real inverter was not
contacted or changed.

The firmware builds at **110,416 bytes static RAM (33.7%)**; runtime RAM was not
measured on hardware. Build 20261112.1 is prepared but has not been published
or installed. The [sanitized validation evidence](evidence/2026-11-12/legacy-grid-export-validation.json)
records source/build hashes, native acceptance, Pi isolation and test results.

### Settings writes and ESS for 2628459: 12 November 2026

Build **20261112.2** enables stored-settings uploads and ESS for the exact
**eight-byte** `2628459` / model 47 identity `6b1b2800080d0000`, following the
isolated tests below. This does not qualify an arbitrary ten-byte reply sharing
that prefix. Ten-byte legacy files remain readable/exportable but are not
write-qualified. Other runtime controls, DVCC/BOL and Assistant replacement
remain outside this legacy profile.

Stock Venus v3.64 `mk2vsc -w` accepted the legacy 2/2 file with its actual eight
version bytes and completed three settings cases: absorption to **57.50 V**,
restart-flagged setting 11 to 3721, and protected grid selector/generation words
with a synthetic authorization token. Each value read back correctly, and an
incorrect token was rejected before any EEPROM request. No case requested the
unknown grid table. The stock writer replays all 142 supported settings,
including unchanged words, and retransmits the unchanged unit configuration;
an independent audit checked every setting value against the entire synthetic
target file. Those native settings-only cases sent no explicit reset packet.
brain_zero continues to write only the changed settings and separately tests
its explicit restart/start-up sequence where access flags require it. This is
not a claim of identical native and brain_zero upload sequencing.

These are protocol simulations: the fixture acknowledges unchanged words and
recognizes a known synthetic token. It does not establish physical authorization
acceptance on an untested inverter. The catalogued 35 A charge-current limit,
48 V voltage ranges and access bytes came from the genuine legacy descriptors;
all identities, saved values, telemetry, credentials and ESS RAM chains were
synthetic. No owner's saved settings or Assistant program reached the Pi.

Stock Venus discovered ESS in reference and relocated RAM layouts, applied
positive/negative/zero setpoints with the expected wire sign, and changed the
three ESS flag bits while preserving unrelated bits. A missing ESS Assistant
exposed no writable control and sent no writes. Every actual runtime write
targeted only the discovered setpoint or flag word. Boundary probes recorded
native behavior separately; brain_zero retains its existing **±5000 W** cap.
The production requester additionally passed 17 scenarios covering unknown
signature lengths, wrong firmware/model/role, missing/duplicate/short/wrapping
or stale layouts, periodic version rechecks and rediscovery after a session
change. No genuine firmware-459 live ESS RAM capture is claimed.

The production Requester, RVSC checker and Uploader were compiled for ARM and
tested inside the Pi's private namespaces. All six suites and ten complete
upload scenarios passed. The successful ordinary-voltage and charger-current
cases each sent one EEPROM request. The restart-required case sent one setting
and completed reset/start-up/readback; the protected case sent three settings,
authorization and reset/start-up/readback. Bad, missing or expired authorization,
36 A against the 35 A limit, and Assistant program/mark changes sent **zero
EEPROM requests**. All ten scenarios sent zero grid requests and zero Assistant
or unit-configuration writes.

Assistant replacement did **not** qualify. The native test failed while the
isolated D-Bus tunnel was being reopened, before a complete programming result;
the process subsequently aborted. This is an inconclusive simulator lifecycle
failure, not a demonstrated physical-inverter defect. Both the file checker and
the low-level writer independently refuse legacy Assistant replacement.

Upload authorization still requires a fresh full read of the current unit,
matching serial/ID, exact received firmware length/signature, standalone layout,
all 192 descriptors/access bytes, flag-access blocks, Assistant capacity and
valid ranges/masks. Protected changes require fresh embedded authorization.
The full file is read back after application. No grid descriptors or extra
version bytes are fabricated to satisfy these checks.

The workers ran as UID 65534 with private mount/network namespaces, only a
virtual serial device and no system D-Bus or physical adapter visibility.
The real inverter was not contacted. The firmware builds at **110,416 bytes
static RAM (33.7%)**; runtime RAM was not measured. Build 20261112.2 is prepared
but not published or installed. A physical 459 wire capture and live-unit
acceptance remain follow-up evidence. See the [sanitized qualification report](evidence/2026-11-12/legacy-459-control-qualification.json).

### Assistant replacement for 2628459: 12 November 2026

Build **20261112.3** enables nonempty Assistant installation and replacement for
the exact eight-byte `2628459` identity above, with a complete **64-record /
4096-byte** Assistant area. Legacy removal/disable remains blocked. This
supersedes the initial Assistant restriction recorded for build 20261112.2.

The original reconnect failure came from the simulator's process lifecycle:
stock `mk2-dbus` deliberately exits after MK2 takeover and asks Daemontools to
restart it. The harness had started it only once. An opt-in private supervisor
now restarts that unmodified driver after its documented successful exit,
retaining the simulated inverter memory, virtual serial device, private D-Bus
and cache. It checks the current process generation's log, limits restarts and
fails on unexpected exits or supervisor errors. A separate negative test
confirmed that an older log marker cannot authorize another restart.

With the repaired lifecycle, stock Venus v3.64 `mk2vsc -w` completed:

- Installation into an empty area, using 128 synthetic program bytes.
- Replacement of 192 old bytes with 128 different bytes.
- Replacement of 192 old bytes with a shorter 64-byte program.

Each upload exited zero. After stopping the private driver, a fresh raw protocol
read compared **all 4096 logical bytes**, including erased trailing records, the
unit/version/serial and final presence flag, and all **142 supported settings**.
The complete program image matched, identity matched and saved settings were
unchanged. Native installation/replacement used 79 Assistant programming
requests and shrinking used 74. No case requested the missing grid table.
Disabling the supervisor reproduced the original reopen failure and abort,
with zero Assistant programming requests and unchanged program/settings memory.

Native removal is a different operation: Venus reports that VLL code is unused,
sets the F0 presence flag and skips program writes. The old bytes remain stored.
That observation does **not** qualify an empty-image erase/readback operation.
Both brain_zero's file checker and low-level writer therefore refuse legacy
removal/disable before any write or reset.

Assistant records hold 64 logical bytes but their six transfer chunks carry up
to 66 wire bytes. The extra two bytes are excluded from the logical image and
are recorded separately in the experiment. Their physical interpretation is
not established; the implementation follows the observed legacy transfer
behavior while preserving the existing modern-profile behavior. There is no
claim of byte-identical native upload sequencing: native Venus also retransmits
unchanged scalar settings and may perform a preparatory reset for installation.
An independent replay matched all 232 Assistant programming requests across
the three native positive cases, including record order and extra wire bytes.

The production Uploader regression covers full-area replacement and addition,
short-bank and empty-bank refusal, removal refusal, record rejection/no reply,
ignored final presence flag, corruption of logical byte 4095, last-record read
timeout, unqualified ten-byte identity, legacy transfer bytes and modern-profile
regression. Truncated, oversized and bad-checksum files are also rejected. The
writer requires a fresh complete standalone-unit read and retains all identity,
descriptor, access, authorization, capacity and full readback checks.
All seven isolated Pi suites and nine Windows suites passed, including the
13 complete Assistant upload scenarios. Static RAM remains **110,416 bytes
(33.7%)**; runtime RAM was not measured.

This is a program-transfer qualification using synthetic program bytes, not an
Assistant execution simulator. The descriptors and eight stored version bytes
come from the genuine legacy export; identities, saved values, program memory
and telemetry are synthetic. Actual firmware reply length, Assistant execution
and recovery on a physical 459 unit still require hardware evidence.

All Pi execution stayed in private mount/network namespaces as UID 65534, with
physical serial devices and system D-Bus hidden. The real inverter was not
contacted. Build 20261112.3 is prepared locally, not published or installed.
The [sanitized Assistant qualification report](evidence/2026-11-12/legacy-459-assistant-qualification.json)
records native outcomes, production tests, source hashes and the firmware image.
