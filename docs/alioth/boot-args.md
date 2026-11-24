# Optional boot-args (November 2026)

English · [中文](boot-args.zh.md)

These boot-args were added in November 2026. Each one is off by default: without it the kernel and our programs
behave as before, and none of them is in `devices/redmi-k40/boot.args`. To try one, append it to that file before
the payload is built. The whole command line has to fit in 1022 characters, and that count includes the loader's own
part and, in a release, the Wi-Fi address and the app token. Each boot-arg prints a line of its own at boot, so a log
shows which ones were in effect.

The numbers come from this phone (`sysctl kern.hakios.kthreads` gives wakes per second for each kernel thread).
Idle numbers are from the home screen at rest, with the modem's GNSS session on.

## Fewer wakeups at rest

Each of these threads used to check for work every few milliseconds. With its boot-arg, a quiet thread waits for its
own event, or for N ms at most, and wakes at once when work arrives.

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `idle_set=1` | 1 | turns on the eleven below at their measured values: `glink_idle=10 wifi_idle=4 sound_idle=100 bt_idle=10 mdm_idle=10 wifi_wq_idle=1000 haptic_idle=100 sound_feed_wait=20 net_idle=1000 mic_idle=100 torch_idle=100`. A boot-arg given by its own name wins, `0` included | our drivers' threads 7,900 → 2,350 wakes a second and 8.9 → 3.9 % of a core; the kernel's total 15,600 → 4,800 wakes a second. Wi-Fi throughput, sound, Bluetooth and the modem's traffic unchanged. Spelled out, the eleven take 1,104 characters with the rest of the line, too many to fit |
| `glink_idle=N` | ms | the sensor DSP and audio DSP links (GLINK) check every N ms after 10 quiet checks; a message we send wakes them | sensor link 880 → 94 wakes a second; audio link 880 → 601 (feeding it silence keeps it busy); message counts unchanged |
| `wifi_idle=N` | ms, 1–500 | Wi-Fi's IPC router and ring threads check every N ms after 10 quiet checks; anything we send wakes them | N=10: router 885 → 169, rings 862 → 153 wakes a second, ping median 2 → 6 ms. N=4: 359 / 299 wakes a second, ping median 4 ms |
| `sound_idle=N` | ms, 1–1000 | the speaker watcher naps while the amplifiers are off; the first sound wakes it | 183 → 10 wakes a second; the amplifiers still come up about 172 ms after a sound starts, as before |
| `sound_feed_wait=N` | ms, 1–20 | the sound feeder waits for the DSP to report a period written, instead of checking every 2 ms | 443 → 86 wakes a second; 0 underflows in 22,571 periods |
| `bt_idle=N` | ms, 1–100 | the Bluetooth UART worker naps while the controller has announced sleep (IBS) and nothing is queued; anything we send wakes it | 882 → 94 wakes a second; the controller sleeps at rest, scans go through. Pairing not tried yet |
| `mdm_idle=N` | ms, 1–100 | starting 60 s after boot, the modem's QRTR listener checks every N ms once 20 ms pass without an MHI event, and takes at most 4 events per ring per check | 881 → 118 wakes a second; packet and location-report counts identical, 0 dropped |
| `wifi_wq_idle=N` | ms, 10–5000 | an empty Wi-Fi (LinuxKPI) work queue's thread checks again after N ms instead of 5 ms; queued work wakes it at once | each of four threads 181 → 1 wakes a second; throughput and ping unchanged |
| `haptic_idle=N` | ms, 1–1000 | while nothing plays, the haptics service waits for iOS's next waveform instead of checking every 4 ms | 224 → 10 wakes a second; a home-screen long press still gives its haptic |
| `net_idle=N` | ms, 20–60000 | the thread of a network link that is never polled (the radio's) checks every N ms; its minute counter line stays on time | 93 → 1 wakes a second |
| `mic_idle=N` | ms, 1–1000 | while the microphone device is closed, its service waits for iOS to open it | 48 → 10 wakes a second. A recording not tried yet |
| `torch_idle=N` | ms, 1–1000 | while the flashlight is off, its service waits for iOS to ask for it | 48 → 10 wakes a second. Turning it on not tried yet |

## Wi-Fi

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `wifi_mimo=1` | 1 | our association request sets SM power save to "off", as Linux's mac80211 writes it for a station with both receive chains on. Before, it passed on the driver's raw "static" (one chain) | the access point went from 1 to 2 spatial streams (433 → 867 Mbit/s on air); TCP receive 122–149 → 161–181 Mbit/s |
| `wifi_he=1` | 1 | Wi-Fi 6: where the access point announces HE, our association request includes our HE capabilities, and the driver gets the access point's HE capabilities and operation | received frames went from VHT MCS 9 to HE MCS 9, 2 streams, 80 MHz. TCP unchanged: the rest of the receive limit is the network's (Android on the same phone and access point shows the same ratio). Use it with `wifi_mimo=1` |
| `wifi_ht_ampdu=N` | 1–3 | the HT A-MPDU factor in our association request (16 / 32 / 64 KB). FreeBSD's LinuxKPI defines ath11k's "64K" as 0, which means 8 KB | no measured gain (for VHT frames the access point uses VHT's own limit) |
| `wifi_off_wait=N` | ms, 1–10000 | when Wi-Fi turns off, the firmware never answers the "off" request; wait N ms for that answer instead of 10 s, then treat the service going away as the answer, as Linux's ath11k does | with N=300, turning the driver off at the end of a run takes 0.39 s instead of 11.36 s |
| `wifi_answers=1` | 1 | Apple80211: wifid's SET_PROPERTY (383) is accepted with no effect, and its COUNTRY_CODE (51) is stored and returned by the matching get. The radio's regulatory setting is untouched | wifid's "Failed to set locale" 13 → 0 in a 4-minute run |

## Sound

| boot-arg | values | what it does | measured |
|---|---|---|---|
| `amp_lead_ms=N` | ms, at most 200 (default 0) | the amplifiers take 170–182 ms to come up, but only 160 ms of sound is queued at the DSP. So when they are off and a sound starts, the feeder holds the sound back N ms. The delay is made up by dropping silence later | start margin −10 to −22 ms → +25 ms with N=40; nothing cut off, 0 underflows |
| `amp_hold_ms=N` | ms, 1000–600000 (default 3000) | how long the amplifiers stay on through silence before they stop | not measured: the trade-off is in battery current |
| `amp_look_ms=N` | ms, 20–5000 (default 200) | how often the amplifiers' status is read over the bit-banged I2C bus while they play | no gain measured |

## Graphics

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `gl_wrap_once=1` | 1 | the render server adds a CGImage's surface once instead of every frame | late frames on the home screen 2.6 / 3.9 % → 1.7 / 2.0 %; the display thread about 11 % less busy |
| `state_shaders=1` | 1 | a saved app state also keeps the app's shader cache and Metal translations (`Library/Caches/hakios-mesa`) | Flutter Check after a reboot with its state restored: the longest draw 33 → 5 ms; the state zip +183 KB |
| `appgl_wait_ms=N` | ms (with `appgl_fps`) | a GL app's present waits until its next surface is no longer on screen, so the app runs at the rate the screen takes rather than at a fixed cap | a map app at a 60 cap → about 85 frames a second, at uneven intervals |
| `metal_upload=1` | 1 | Metal `replaceRegion` copies its bytes outside the library lock | correct, no measurable gain (the cost is the staging allocation) |

## Camera

| boot-arg | values | what it does | measured |
|---|---|---|---|
| `cam_pieces=N` | 1–64 (default 8) | the camera picture is made in N pieces of rows | N=32: 10.6 → 9.6 ms a frame |
| `cam_gather=N` | 1 or 2 (default 0) | preview cells are made only where the picture uses them. With 1 the output is byte-identical; 2 also covers narrower pictures | 1: about 17 % less time in Camera.app. 2: 1280×960 takes 15 ms instead of 28; 3 bytes in 1.8 million differ |
| `cam_stats=N` | cells (default 4) | the camera statistics (exposure, white balance) use every Nth cell in each direction | N=8: 3.9 → 1.3 ms a frame; exposure and white balance settle the same |

## CPU and temperature

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `cpufreq_hwtop=1` | 1 | the biggest core's frequency table stops at the clock the hardware reads back (2841 MHz) instead of 3187 MHz | same clock as before; the governor's averages now show the true value (2703 MHz instead of 3013 at the same share of time at the top) |
| `thermal_hold=N` | s, at most 60 | a thermal level above nominal goes back to nominal only after N s in a row with every zone 3 °C below the moderate threshold; going up is immediate, as before | 4-minute runs: 5–13 level changes → 4, and changes less than 1 s apart 1–9 → 0 |
| `cpu_suspend=MASK` | cores (with `cpu_suspend_min`, µs) | idle cores power down through PSCI CPU_SUSPEND and are woken by a QTimer frame | not run on the phone yet; do not use it. A core that does not come back hangs the phone |
| `psci_probe=1` | 1 | read-only: the PSCI version and idle features, in `sysctl kern.hakios.psci` | PSCI 1.1; CPU_SUSPEND with OS-initiated mode and the extended state ID; no SYSTEM_SUSPEND |

## Location

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `gnss_seed_auth=S` | s | gnss_feed waits up to S s for Location Services to come on before it authorizes the Wi-Fi seed's bundle. When locationd takes the authorization back, gnss_feed grants it again, up to 5 times | before: the seed was refused in 7 of 8 boots. With S=30, the seed (±35 m) reached the modem in every run checked |
| `gnss_still=N` | s | gnss_feed sends a fix from a phone that is not moving at most every N s; a move or a better fix goes at once | not tested yet (no GNSS fix indoors) |
| `gnss_seed_for=S` | s | gnss_feed stops requesting the Wi-Fi seed after S s | no measured gain |

## System

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `at_home=1` | 1 | AssistiveTouch's Home acts as the home button: `libhakios.at.dylib` in assistivetouchd sends the button through `/dev/hakios-touch`, because iOS's virtual HID device cannot be made with this kernel | Settings → AssistiveTouch → Home goes to the home screen (before: nothing) |
| `ipa_sign=keep` | `keep`, or `0` for never (default: sign every IPA when installing it) | an IPA that was already signed on the computer (marked `.hakios-adhoc`) is installed as is, without being signed again | 4.1 s less signing in the busiest first minute (5 IPAs) |
| `ramfs_swap=1` | 1 | the RAM file system supports renamex `RENAME_SWAP` / `RENAME_EXCL`, so usermanagerd can save its keybags | works, but on its own SpringBoard comes up about 10 s later: lsd's persona queries then succeed and cost more than the quick failure did |

Test roots only (their `loc_enable --all`): `loc_poll=N` (seconds between its app scans, default 10) and
`loc_photos_v2=0` (leaves the Photos rows at version 1 and does not restart tccd; no measured gain).

## Tried together

This line ran on the phone in one boot, with a release's apps installed, for 8 to 20 minutes:

    gl_wrap_once=1 state_shaders=1 cam_pieces=32 cam_gather=1 cam_stats=8 wifi_off_wait=300 amp_lead_ms=40 wifi_he=1 wifi_mimo=1 wifi_ht_ampdu=3 cpufreq_hwtop=1 thermal_hold=5 wifi_answers=1 gnss_seed_auth=30 at_home=1 idle_set=1

All apps installed, each boot-arg printed its own line, and nothing faulted. Over 20 minutes the modem dropped 0
packets, sound had 0 underflows, Bluetooth slept normally, the temperature stayed at 36–38 °C, and no process was
killed for memory. In a release boot the line comes to 956 of the 1022 characters.
