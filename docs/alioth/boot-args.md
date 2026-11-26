# Optional boot-args (November 2026)

English · [中文](boot-args.zh.md)

These boot-args were added in November 2026. Each one is off when it is not given: without it the kernel and our
programs behave as before. Since 2026-11-25 the line under "Tried together" is in `devices/redmi-k40/boot.args`
(so every build from that file has those on); the others are not. To try one, append it to that file before
the payload is built; to leave one of the line out, delete it there or give it `=0`. The whole command line has to fit in 1022 characters, and that count includes the loader's own
part and, in a release, the Wi-Fi address and the app token. Each boot-arg prints a line of its own at boot, so a log
shows which ones were in effect.

The numbers come from this phone (`sysctl kern.hakios.kthreads` gives wakes per second for each kernel thread).
Idle numbers are from the home screen at rest, with the modem's GNSS session on.

## Fewer wakeups at rest

Each of these threads used to check for work every few milliseconds. With its boot-arg, a quiet thread waits for its
own event, or for N ms at most, and wakes at once when work arrives.

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `idle_set=1` | 1 | turns on the twelve below at their measured values: `glink_idle=10 glink_follow=2 wifi_idle=4 sound_idle=100 bt_idle=10 mdm_idle=10 wifi_wq_idle=1000 haptic_idle=100 sound_feed_wait=20 net_idle=1000 mic_idle=100 torch_idle=100`. A boot-arg given by its own name wins, `0` included | the kernel's threads (the idle ones aside) 7,900 → 2,000 wakes a second and 8.9 → 3.5 % of a core; the kernel's total 15,600 → 4,100 wakes a second (about 11 minutes after boot, with the rest of the line under Tried together). Wi-Fi throughput, sound, Bluetooth and the modem's traffic unchanged. Spelled out, the first eleven took 1,104 characters with the rest of the line, too many to fit |
| `glink_idle=N` | ms | the sensor DSP and audio DSP links (GLINK) check every N ms after 10 quiet checks; a message we send wakes them | sensor link 880 → 94 wakes a second; audio link 880 → 601 (feeding it silence keeps it busy); message counts unchanged |
| `glink_follow=N` | 1–10, with `glink_idle` | the audio DSP link: after a check that found only the DSP's own traffic, or a send of ours that nobody waits on (the sound feeder's writes, about 38 a second even in silence), N quick checks follow instead of 10. A command that waits for its answer still gets 10 | N=2: audio link 571 → 226 wakes a second (0.83 → 0.40 % of a core); 0 underflows in 22,565 periods; the first sound reaches the amplifiers as before |
| `glink_irq=1` | 1, with `glink_idle` | the sensor DSP and audio DSP links wake on IPCC's interrupt (INTID 261, the doorbells both DSPs ring; nothing took it before): the link whose DSP rang is woken, and while the interrupt is on a link waits up to 1 s between checks (a message we send no longer wakes it: the answer rings) instead of `glink_idle`'s checks. One line for both: masked in its handler, unmasked once every ring is cleared. Back to `glink_idle`'s checks for good on: rings found after whole waits without an interrupt (3 in a minute), more interrupts than rings (over 2,000 a second), a ring of a client neither link listens to, no interrupt in the first minute | at rest, with the idle set: audio link 225.6 -> 100.0 wakes a second (one per ring: the sound feeder's write-dones), sensor link 91.4 -> 1.0, the kernel 2,079 -> 1,585; interrupts = rings (74,328 / 74,339 in 12 minutes), 0 missed; 0 underflows, the boot's sound, light readings, ping and throughput as without it |
| `wifi_idle=N` | ms, 1–500 | Wi-Fi's IPC router and ring threads check every N ms after 10 quiet checks; anything we send wakes them | N=10: router 885 → 169, rings 862 → 153 wakes a second, ping median 2 → 6 ms. N=4: 359 / 299 wakes a second, ping median 4 ms |
| `sound_idle=N` | ms, 1–1000 | the speaker watcher naps while the amplifiers are off; the first sound wakes it | 183 → 10 wakes a second; the amplifiers still come up about 172 ms after a sound starts, as before |
| `sound_feed_wait=N` | ms, 1–20 | the sound feeder waits for the DSP to report a period written, instead of checking every 2 ms | 443 → 86 wakes a second; 0 underflows in 22,571 periods |
| `bt_idle=N` | ms, 1–100 | the Bluetooth UART worker naps while the controller has announced sleep (IBS) and nothing is queued; anything we send wakes it | 882 → 94 wakes a second; the controller sleeps at rest, scans go through. Pairing not tried yet |
| `bt_irq=1` | 1, with `bt_idle` | the Bluetooth UART's own interrupt (INTID 639; nothing took it before) ends the worker's nap when the controller sends, so while the controller sleeps the worker naps up to 1 s instead of `bt_idle`'s N ms (the controller cannot send more than its wake-up byte before we answer it). The line is unmasked only for a nap, once the UART's receive queue is empty. Back to `bt_idle`'s naps for good on: bytes found after whole naps without an interrupt (3 in a minute), interrupts that bring nothing (over 200 a second), no interrupt in the first minute though the controller woke | at rest, with the idle set and `glink_irq=1`: `umi_bluetooth:bt_control_worker` 91.2 -> 1.0 wakes a second, the kernel 1,585 -> 1,417; one interrupt per controller wake-up, 0 missed, 0 transport faults; with Settings > Bluetooth open (run 140444) iOS's 23 HCI commands -- inquiries, scan enables, 3 LE scan rounds -- went through, 0 rejected |
| `mdm_idle=N` | ms, 1–100 | starting 60 s after boot, the modem's QRTR listener checks every N ms once 20 ms pass without an MHI event, and takes at most 4 events per ring per check | 881 → 118 wakes a second; packet and location-report counts identical, 0 dropped |
| `mdm_irq=1` | 1 | the modem's MHI interrupts wake its QRTR listener, which then waits for one after every poll (at most 1 s, or until the next HELLO / status line) instead of looking every `mdm_idle` ms; needs `mdm_msi=1` and `mdm_idle` (idle_set's). The MSI's data becomes 864, the device tree's PCIe2 block (INTIDs 864-895): `mdm_msi=1` alone writes 960, one past this GIC's last interrupt (959), so those writes go nowhere. No interrupt within its first idle minute: the old looks | at rest: `alioth_mdm:mdm_thread` 116.3 -> 12.7 wakes/s, one interrupt per event (752 a minute), 0 dropped; packets, GNSS reports and positions as without it |
| `wifi_wq_idle=N` | ms, 10–5000 | an empty Wi-Fi (LinuxKPI) work queue's thread checks again after N ms instead of 5 ms; queued work wakes it at once | each of four threads 181 → 1 wakes a second; throughput and ping unchanged |
| `umi_wifi_irq=N` | 1 or 2 | Wi-Fi by the radio's interrupts instead of looking: 1, its rings -- the MSI goes through its SMMU context to the GIC (SPIs from 800), and the copy engines' and data-path groups' functions hang at the vectors the radio is told on this chip (3-12 and 14-31); 2, also MHI's own interrupts (vectors 0-2) wake the IPC router's thread (`umi-ipcr`), which then waits up to a second when quiet | at rest (+11 min, with the default line): the Wi-Fi threads 929 -> 300 wakes/s (`umi-ipcr` 357 -> 2, `wifi-rings` 300 -> 10; `wifi-ticks` ~267 either way), their CPU 1.39 -> 0.27 % of a core, the kernel 4,107 -> 2,766; ping median 2 ms and 8-9 MB/s, as without it |
| `wifi_mon_ms=N` | ms, 20–1000 | ath11k's monitor-status reap timer -- re-armed every 10 ms for good on this chip, 96 % of the Wi-Fi ticker's fires at rest -- is armed N ms ahead while fewer than 20 frames came in the last second; traffic brings the driver's 10 ms back at its next arming. Pays off with the radio's interrupts on and the ticker allowed to sleep (`umi_wifi_irq=2 wifi_idle=50`; under polling wifi_idle is the rings' look as well) | with `umi_wifi_irq=2 wifi_idle=50`, at rest: `wifi-ticks` 93.4 -> 21.4 wakes/s (N=100; the reap 48,660 -> 9,259 fires in a run, half its armings stretched); ping median 2 ms, 8.5 MB/s, the received-rate statistics as without it |
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
| `gpu_ring_switch=1` | 1 | the GPU's command ring switches between apps' page tables itself (CP_SMMU_TABLE_UPDATE), so the processor no longer waits for the GPU to finish one app's work before it sends the next app's | not run on the phone yet; do not use it (its first boot is planned with someone at the phone) |

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
| `gnss_seed_auth=S` | s | gnss_feed waits up to S s for Location Services to come on before it authorizes the Wi-Fi seed's bundle. When locationd takes the authorization back, gnss_feed grants it again, up to 5 times | before: the seed was refused in 7 of 8 boots. With S=30, the seed (±35 m) reached the modem in every run checked. Use it with `gnss_seed_for` (below), or locationd keeps scanning Wi-Fi while the modem has no fix |
| `gnss_still=N` | s | gnss_feed sends a fix from a phone that is not moving at most every N s; a move or a better fix goes at once | not tested yet (no GNSS fix indoors) |
| `gnss_seed_for=S` | s | gnss_feed asks for the Wi-Fi seed at most S s, and stops 20 s after the first seed is given | without it, the seed keeps asking until the modem has its own fix. Indoors that may be never, and locationd then scans Wi-Fi about every 6 s for the whole session (182 scans in 20 minutes). This happens without `gnss_seed_auth` too, whenever the seed gets through by itself (4 of 15 boots in a week of test runs). With S=120 the seed was still given, and the scans stopped 20 s later (6 scans, then none for 8 minutes) |

## System

| boot-arg | values (default: off) | what it does | measured |
|---|---|---|---|
| `at_home=1` | 1 | AssistiveTouch's Home acts as the home button: `libhakios.at.dylib` in assistivetouchd sends the button through `/dev/hakios-touch`, because iOS's virtual HID device cannot be made with this kernel | Settings → AssistiveTouch → Home goes to the home screen (before: nothing) |
| `pb_store=1` | 1 | PosterBoard's wallpaper store gets a directory of its own: in a RAM boot PosterBoard has no container, so its store's base is the shared `Application Support`, and its store reset wiped what other services keep there -- ScreenTimeAgent's Remote Management store among them (`libhakios.pb.dylib` in PosterBoard, from upstream's `umi_posterboard.m`) | ScreenTimeAgent's "Can't create coordinator" 14 -> 0 on the same root; its store and PosterBoard's own directory stay; Screen Time opens; the wallpaper as before |
| `ipa_sign=keep` | `keep`, or `0` for never (default: sign every IPA when installing it) | an IPA that was already signed on the computer (marked `.hakios-adhoc`) is installed as is, without being signed again | 4.1 s less signing in the busiest first minute (5 IPAs) |
| `ramfs_swap=1` | 1 | the RAM file system supports renamex `RENAME_SWAP` / `RENAME_EXCL`, so usermanagerd can save its keybags | works, but on its own SpringBoard comes up about 10 s later: lsd's persona queries then succeed and cost more than the quick failure did |

Test roots only (their `loc_enable --all`): `loc_poll=N` (seconds between its app scans, default 10) and
`loc_photos_v2=0` (leaves the Photos rows at version 1 and does not restart tccd; no measured gain).

## Tried together

In `devices/redmi-k40/boot.args` since 2026-11-25. This line ran on the phone in one boot, with a release's apps installed, for 8 to 20 minutes:

    gl_wrap_once=1 state_shaders=1 cam_pieces=32 cam_gather=1 cam_stats=8 wifi_off_wait=300 amp_lead_ms=40 wifi_he=1 wifi_mimo=1 wifi_ht_ampdu=3 cpufreq_hwtop=1 thermal_hold=5 wifi_answers=1 gnss_seed_auth=30 at_home=1 idle_set=1 gnss_seed_for=120

All apps installed, each boot-arg printed its own line, and nothing faulted. Over 20 minutes the modem dropped 0
packets, sound had 0 underflows, Bluetooth slept normally, the temperature stayed at 36–38 °C, and no process was
killed for memory. `gnss_seed_for=120` was added after the 20-minute run and ran with the rest for 9 minutes, again with no fault. In a release boot the line comes to 974 of the 1022 characters.
