#!/bin/sh
# Stage the bridge code and a guarded YAML candidate. No compile, OTA or restart.
# Usage: PACE_BLE_ARCHIVE_URL=http://... PACE_BLE_ARCHIVE_SHA256=... sh script CONFIG [--apply]
set -eu
umask 077
config=${1:-/config/esphome/pacecan.yaml}
mode=${2:---stage}
case "$mode" in --stage|--apply) ;; *) printf '%s\n' 'Expected --stage or --apply' >&2; exit 2;; esac
test -f "$config" || { printf '%s\n' 'Configuration file not found' >&2; exit 2; }
source_sha=$(sha256sum "$config" | cut -d ' ' -f 1)
case "$config" in /config/esphome/*.yaml) ;; *) printf '%s\n' 'Only an explicit /config/esphome/*.yaml target is allowed' >&2; exit 2;; esac
: "${PACE_BLE_ARCHIVE_URL:?Public-code archive URL required}"
: "${PACE_BLE_ARCHIVE_SHA256:?Expected archive SHA-256 required}"
case "$PACE_BLE_ARCHIVE_SHA256" in *[!a-fA-F0-9]*|'') printf '%s\n' 'Invalid SHA-256' >&2; exit 2;; esac
test "${#PACE_BLE_ARCHIVE_SHA256}" -eq 64 || exit 2
stage=$(mktemp -d /tmp/brain-zero-pace-ble.XXXXXX)
archive=$stage/components.tar.gz
curl -fsS --max-time 30 "$PACE_BLE_ARCHIVE_URL" -o "$archive"
printf '%s  %s\n' "$PACE_BLE_ARCHIVE_SHA256" "$archive" | sha256sum -c - >/dev/null
tar -tzf "$archive" > "$stage/members"
while IFS= read -r item; do
  case "$item" in
    esphome/pace-can-ble.yaml|esphome/components/pace_can_ble/__init__.py|esphome/components/pace_can_ble/protocol.h|esphome/components/pace_can_ble/pace_can_ble.h|esphome/components/pace_can_ble/pace_can_ble.cpp) ;;
    *) printf '%s\n' 'Unexpected archive member; nothing applied' >&2; exit 2;;
  esac
done < "$stage/members"
test "$(wc -l < "$stage/members" | tr -d ' ')" -eq 5 || exit 2
tar -xzf "$archive" -C "$stage"
candidate=$config.bms-ble-candidate.yaml
test ! -e "$candidate" || { printf '%s\n' 'Candidate already exists; inspect it before retrying' >&2; exit 2; }
awk '
function fail(s) { print "Refused: " s > "/dev/stderr"; exit 2 }
function spaces(n, s) { s=""; while(n-->0) s=s" "; return s }
function rootkey(s, k) {
  if (s !~ /^[A-Za-z_][A-Za-z_0-9]*:[ \t]*(#.*)?$/) return ""
  k=s; sub(/:.*/, "", k); return k
}
{ text[++count]=$0 }
END {
  section=""; can_platform=0; platform_line=0; field_indent=0; have_id=0; have_rx=0; have_max=0
  have_packages=0; rx=5; max_frames=50
  for (i=1;i<=count;i++) {
    key=rootkey(text[i]); if(key!="") section=key
    if ((section=="api" || section=="mqtt" || section=="wifi") &&
        text[i] !~ /^[ ]*($|#)/ && match(text[i], /^[ ]+/)) {
      depth=RLENGTH
      if (!(section in child_indent) || depth<child_indent[section]) child_indent[section]=depth
    }
    if (key=="esp32_ble" || key=="esp32_ble_server" || key=="pace_can_ble") fail("BLE already configured; merge manually")
    if (text[i] ~ /brain_zero_pace_ble:/ || text[i] ~ /^[ \t]+pace_can_id:/) fail("bridge binding already present")
    if (key=="packages") have_packages++
    if (section=="canbus" && text[i] ~ /^[ ]*-[ ]*platform:[ ]*esp32_can[ ]*(#.*)?$/) {
      can_platform++; platform_line=i; field_indent=index(text[i],"-")+1
    }
    if (section=="canbus" && platform_line && text[i] ~ ("^" spaces(field_indent) "id:")) {
      value=text[i]; sub(/^[ ]*id:[ ]*/,"",value); sub(/[ ]*(#.*)?$/,"",value)
      if(value!="pace_can_bus") fail("CAN id is already bound to another name")
      have_id++
    }
    if (section=="canbus" && platform_line && text[i] ~ ("^" spaces(field_indent) "rx_queue_len:")) {
      value=text[i]; sub(/^[ ]*rx_queue_len:[ ]*/,"",value); sub(/[ ]*(#.*)?$/,"",value)
      if(value !~ /^[0-9]+$/) fail("RX queue must be an explicit integer")
      rx=value+0; have_rx++
    }
    if (section=="canbus" && platform_line && text[i] ~ ("^" spaces(field_indent) "max_frames_per_loop:")) {
      value=text[i]; sub(/^[ ]*max_frames_per_loop:[ ]*/,"",value); sub(/[ ]*(#.*)?$/,"",value)
      if(value !~ /^[0-9]+$/) fail("CAN drain limit must be an explicit integer")
      max_frames=value+0; have_max++
    }
  }
  if(can_platform!=1 || have_id>1 || have_rx>1 || have_max>1 || have_packages>1) fail("expected one CAN receiver and unique fields")
  if(rx>max_frames || rx<1 || max_frames<1) fail("CAN RX queue must fit one drain iteration")
  section=""
  for(i=1;i<=count;i++) {
    key=rootkey(text[i]); if(key!="") section=key
    if ((section=="api" || section=="mqtt" || section=="wifi") &&
        text[i] ~ ("^" spaces(child_indent[section]) "reboot_timeout:")) continue
    if (section=="wifi" && text[i] ~ ("^" spaces(child_indent[section]) "power_save_mode:")) {
      value=text[i]; sub(/^[ ]*power_save_mode:[ \t]*/,"",value); sub(/[ \t]*(#.*)?$/,"",value)
      value=tolower(value)
      if(value=="none" || value=="\"none\"" || value=="\047none\047") {
        comment=""; if(match(text[i],/[ \t]+#/)) comment=substr(text[i],RSTART)
        line=text[i]; sub(/:.*/,": light",line); print line comment
        continue
      }
    }
    print text[i]
    if(key=="api" || key=="mqtt" || key=="wifi") {
      depth=(key in child_indent) ? child_indent[key] : 2
      print spaces(depth) "reboot_timeout: 0s"
    }
    if(key=="packages") print "  brain_zero_pace_ble: !include pace-can-ble.yaml"
    if(i==platform_line) {
      if(!have_id) print spaces(field_indent) "id: pace_can_bus"
      if(!have_rx) print spaces(field_indent) "rx_queue_len: 5"
      if(!have_max) print spaces(field_indent) "max_frames_per_loop: 50"
    }
  }
  if(!have_packages) print "\npackages:\n  brain_zero_pace_ble: !include pace-can-ble.yaml"
}' "$config" > "$stage/candidate.yaml"

# Copy only task-owned public code. Existing battery_link code is untouched.
component_dir=/config/esphome/components/pace_can_ble
test ! -e "$component_dir" || { printf '%s\n' 'Component directory already exists; inspect before replacing it' >&2; exit 2; }
test ! -e /config/esphome/pace-can-ble.yaml || { printf '%s\n' 'Package already exists; inspect before replacing it' >&2; exit 2; }
mkdir -p "$component_dir"
for name in __init__.py protocol.h pace_can_ble.h pace_can_ble.cpp; do
  cp "$stage/esphome/components/pace_can_ble/$name" "$component_dir/$name"
done
cp "$stage/esphome/pace-can-ble.yaml" /config/esphome/pace-can-ble.yaml
cp "$stage/candidate.yaml" "$candidate"
if [ "$mode" = --apply ]; then
  test "$(sha256sum "$config" | cut -d ' ' -f 1)" = "$source_sha" || {
    printf '%s\n' 'Source configuration changed during staging; original was not replaced' >&2; exit 2;
  }
  backup_dir=/config/cerbo-light/backups/bms-ble-20261116
  mkdir -p "$backup_dir"
  backup=$backup_dir/pacecan-before-ble-$(date +%Y%m%d-%H%M%S)-$$.yaml
  test ! -e "$backup" || exit 2
  cp -p "$config" "$backup"
  mv "$candidate" "$config"
  printf '%s\n' 'Applied guarded YAML and public component files. Validate and compile before any OTA.'
else
  printf '%s\n' 'Staged public component files and YAML candidate. Original device YAML is unchanged.'
fi
printf '%s\n' 'No firmware upload, restart, CAN command or inverter setting operation was performed.'
