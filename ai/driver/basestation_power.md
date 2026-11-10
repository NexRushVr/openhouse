# 2.0 base station power and management over Bluetooth (2026-11-09)

## Why

SteamVR powers 2.0 base stations from vrmonitor, through a headset's own Bluetooth radio
(Index, Vive Pro, Bigscreen Beyond): it switches the headset's nRF radio between the
controller protocol and BLE through driver debug requests to the lighthouse driver. With
any other headset (Quest through Virtual Desktop / ALVR / Steam Link, ...) vrmonitor finds
no radio and does nothing, so the bases spin until they're unplugged, even though
SteamVR's power list holds every base. Under openhouse the headset path works too: the
driver answers vrmonitor's radio mode requests for the headset's built-in receiver
(`driver/openvr_driver.md`, `usb/radio_mode.hpp`; from upstream, checked there on a
Bigscreen Beyond).

So openhouse does it itself, through the PC's Bluetooth: wake the room's 2.0 bases when
the driver starts and, when the user asks for it, sleep (or standby) them when SteamVR
exits. Plus a tool for the rest: status without connecting, identify, channel, firmware
versions.

## Pieces

| Path | What |
|---|---|
| `src/basestation/protocol.*` | GATT layout, power values, the advert format, firmware string, serials, SteamVR's list. No radio code: `tests/unit/test_basestation.cpp` checks it all |
| `src/basestation/radio.hpp` | The radio interface: scan adverts, connect, read, write with response |
| `src/basestation/radio_winrt.cpp` | Windows: WinRT Bluetooth LE through its ABI, the few interfaces needed declared by hand (vtables from the 10.0.19041 SDK headers), combase.dll loaded at run time, async ops polled through `IAsyncInfo`. Written to need no WinRT headers or C++/WinRT, so MinGW can build it; **only the MSVC build has been compiled and run so far**. Other platforms: a stub that says so |
| `src/basestation/manager.*` | Operations: scan first (finds addresses, reads every base's state from its advert), skip bases already in the asked state, then one thread per base (4 at once). Remembers addresses in `<config>/basestations.json`. A scan that can't listen at all (Bluetooth off or absent) fails every base with that reason, nothing is tried |
| `src/basestation/targets.*` | What "the room" means: the 2.0 bases of openhouse's room map (the room updated last; a base is a 1.0 by its config's `hardwareVersion` or by SteamVR's list), else SteamVR's power list (`driver_lighthouse/PowerManagedBaseStations2`, 2.0 entries); never a base switched off in that list (`:0`). A room of 1.0 bases only gives no targets (and no scan) |
| `apps/lhbase` | The tool (Windows only for now): `status` (default), `on`/`standby`/`sleep`, `identify`, `channel`, `info`, `watch` (follows vrserver for SteamVR's own driver). Targets: serials and/or `--room`, `--all`, minus `--except` |
| `src/openvr/basestation_power.*` | The driver starts lhbase (out of process: it takes seconds and must outlive the host at shutdown) at `Init` (`on --room`) and as the last step of `Cleanup` (`sleep`/`standby` for `--room` plus the 2.0 bases seen this session), both with `--except` for the bases switched off in SteamVR's list. Live sessions under SteamVR (`vrserver`) on Windows only: never replays, and never Monado, whose older `steamvr_lh` aborts on the `GetString` the settings need |

Settings page (`driver_openhouse`): `baseStationPower` (bool, default on: the wake),
`baseStationPowerOff` (`none` default, `sleep`, `standby`). Nothing at exit by default, as
with SteamVR's own power management: a room's bases can be shared with another PC, and
the exit has no grace period for a SteamVR restart (the helper must not linger, Steam
may count it as SteamVR still running; `lhbase watch` waits 10 s). Environment:
`LH_NO_BASESTATION_POWER=1` turns it off, `LH_BASESTATION_TOOL=PATH` picks another lhbase.
The driver finds lhbase in the data directory (`%LOCALAPPDATA%\openhouse`), where the
installer copies it next to openhouse-check; its log is `basestations.log` there (one
1 MB backup kept). `platform::RunInBackground` starts it without a window and breaks
away from the host's job object when allowed; when it isn't, the exit command waits up
to 8 s so a job that kills its processes with the host can't cut it short.

## Protocol (read from 2.0 bases, radio firmware 2.9, through Windows 11's Bluetooth)

| Service / characteristic | Properties | Use |
|---|---|---|
| `00001523-1212-efde-1523-785feabcd124` | | control service (handles 14-22) |
| `00008421-...` | write | identify: 0x01 blinks the LED |
| `00001525-...` | read, write, notify | power |
| `00001524-...` | read, write, notify | channel, 1-16 (the number SteamVR shows; its `sobChannel` is this minus 1) |
| `0x180A` | | 0x2A29 manufacturer, 0x2A24 model, 0x2A25 the sticker serial, 0x2A27 hardware revision, 0x2A26 firmware (`R: radio\nM: mcu\nB: bootloader`, space padded) |

Also there: 0xFE59 (Nordic DFU) and one 128-bit service we don't use.

Power, write: 0x01 on, 0x02 standby, 0x00 sleep. Read: 0x0b on, 0x02 standby, 0x00
sleeping; 0x01/0x08/0x09 while spinning up (decoded as "waking"). What one base did:

| From | Command | Read right after | Then |
|---|---|---|---|
| on | standby | 0x02 | |
| standby | on | 0x0b | at once: standby keeps the motor spinning (lasers off) |
| on | sleep | 0x00 | |
| sleep | on | waking | on after ~12 s (the motor spinning up) |

So standby saves no motor wear. lighthouse_pm (an Android/iOS app for these bases) maps
the same read values (0x00 sleep, 0x02 standby, 0x0b on, 0x01/0x08/0x09 booting).

Adverts: the name `LHB-XXXXXXXX` and manufacturer data under Valve's company id 0x055D:
`00 02 CC 01 PP 06 00`. CC is the channel (matched the channel characteristic on every
base read), PP the power byte (matched the power characteristic: on, sleeping and waking seen
in adverts). Bytes 0, 3, 5 and 6 never changed. `lhbase status` needs no connection, and
power commands skip bases whose advert already says so (`--force` sends anyway).

Behaviour worth knowing:
* The first connection to a base takes 2-4 s through WinRT (service discovery), later
  ones about 2.5 s. Four bases in parallel take about as long as one.
* Windows keeps the link a few seconds after we close it, and a connected base doesn't
  advertise: a scan right after a command can miss it. The address cache covers that.
* Base addresses are random static (top bits 11), stable across power states.
* `--wait S` makes "got there" part of success: a base still waking (or anything else)
  when the time is up gets the command once more, then counts as failed (exit code 1).
  JSON output (`--json`) has each base's power state and advertised channel, for scripts
  that want every base on and no two on one channel.
* Identify writes are acknowledged; the blink itself wasn't watched.
* Channel: one base moved 12 -> 9 -> 12; each write took effect at once and the advert's
  channel byte followed. `channel` refuses a channel another base in range advertises
  (`--force` overrides).
* Connecting has one time budget (20 s per attempt, two attempts); a read or write on
  the open link gets 5 s. Commands hold a cross-process lock, so a slow one delays the
  next (it gives up waiting after 120 s).

## Tested on hardware (2026-11-09: four 2.0 bases, Windows 11, the PC's own Bluetooth)

`status`; `on` from sleep with `--wait` (12.8-14.8 s per base, 19 s for the four);
`standby` and back (2.4-3.9 s per base, about 4.3 s for the four); `sleep`; `identify`;
`info`; the channel round trip above; the SteamVR-list fallback (the bases in range
used, the list's other entries counted as away); two commands at once (the second waits). The
driver's hooks, live in the mock host (`lh_openvr_host_test ... live`, renamed to start
with `vrserver`, `LH_HOST_SETTING_baseStationPowerOff=sleep`): four sleeping bases were
waking 9 s after `Init`, and asleep 6 s after `Cleanup` (lhbase outlived the host).

Inside real SteamVR (one session, no headset connected, `baseStationPowerOff` = `sleep`):
vrserver started `lhbase on --room` at `Init` (bases waking 7 s later) and `lhbase sleep
--room` at the end of `Cleanup`, logged "Graceful exit" 0.35 s after that (it didn't wait:
the helper outlives the real vrserver too), and the bases were asleep 7 s later. Steam
kept SteamVR in its running list until the helper was done, about 9 s after vrserver
exited (the same second with the stock driver), and lists the wake helper as one of
SteamVR's processes while it runs. Starting SteamVR again inside those seconds wasn't
tried.

Not tested: a full session with a headset and trackers, with
Bluetooth off or absent (unit-tested with a fake radio only), older base firmware
(community notes on the protocol say older ones can't read the power state or do standby),
a MinGW build.

## Open

* Linux (BlueZ over D-Bus) and macOS (CoreBluetooth) backends: `OpenPlatformRadio`.
* 1.0 bases: their BLE power protocol is different (HTC's, and waking needs each base's
  id); not covered.
* Idle: standby or sleep the bases while no tracked device is on (SteamVR has an
  inactivity timeout for this).
* With a headset that has the radio (Index, Vive Pro, Beyond), SteamVR's own path and this
  one both act on the same bases. Two wakes are harmless; whether both connecting at once
  makes either side retry or fail wasn't tried (no such headset here). `baseStationPower`
  off, or `LH_NO_BASESTATION_POWER=1`, leaves it to SteamVR.
