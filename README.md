# CYD Desk Controller (ESP32-2432S028R "Cheap Yellow Display")

A touchscreen desk panel for the house MQTT network. The firmware knows nothing
about lights or blinds: what the panel shows and what it publishes comes from a
JSON config, so the same binary can drive a different room by swapping a few
lines of JSON.

## How it works

```
main/default_config.json  ──embedded──┐
                                      ├─► cfg::parse() ─► ui::init() ─► buttons
cmnd/desk/config  ──► NVS "uicfg" ────┘
```

- Each button is a label, a topic to publish to, and a payload (or two, for a
  toggle).
- Optional `state` makes a button show the real device state, read from a JSON
  topic by dot-path — no per-device code.
- Optional `query` re-asks the device for its state on connect and every
  `poll_s` seconds, so a change missed while offline repairs itself.
- Pressing a button publishes. Nothing else. A toggle always sends an explicit
  `on` or `off` payload based on what it last saw, so a stale state cannot invert
  a press.

## Default config

`main/default_config.json` (embedded at build time, used until a config is stored
over MQTT):

| Row | Button | Publishes | State |
|-----|--------|-----------|-------|
| 1 | DESK (toggle) | `cmnd/zigbridge/ZbSend` `{"Device":"0x1234","send":{"Power":"On"\|"Off"}}` | `tele/zigbridge/SENSOR` `ZbReceived.0x1234.Power` |
| 2 | LIGHT (toggle) | `cmnd/light/POWER` `ON` \| `OFF` | `stat/light/RESULT` `POWER` |
| 3 | UP / STOP / DOWN | `cmnd/zigbridge/ZbSend` `{"Device":"0x5678","Write":{"EF00/0401":0\|1\|2}}` | — |

Blind DP1 values are the Tuya control enum: `0` = open (UP), `1` = stop (STOP),
`2` = close (DOWN).

**The bridge has more than one identical TS0601 curtain motor**, so the address
matters: the panel targets the one it drives (`0x5678`) directly rather than
publishing to the Node-RED `blind/command` flow, which is wired to a different
motor.

## Config schema

```json
{
  "title": "desk",          // status-bar caption (defaults to sensor_name)
  "poll_s": 60,             // re-send every button's query this often (0 = never)
  "rows": [
    { "height": 64,         // pixels; omit or 0 to share the leftover height
      "buttons": [
        { "label": "LIGHT",
          "publish": { "topic": "cmnd/light/POWER",
                       "on": "ON", "off": "OFF" } },
        { "label": "BOOST",
          "publish": { "topic": "cmnd/light/boost",
                       "press": "1" } }
      ] }
  ]
}
```

| Key | Meaning |
|-----|---------|
| `publish.topic` | topic to publish a tap to (required) |
| `publish.on` / `publish.off` | payloads for a **toggle** button; both required |
| `publish.press` | payload for a **momentary** button (no state, flashes on tap) |
| `state.topic` | JSON topic that carries the device's state |
| `state.path` | dot path inside that JSON, e.g. `ZbReceived.0x1234.Power` |
| `state.on` | value (or array of values) meaning "on"; default `"ON"` |
| `query.topic` / `query.payload` | state request sent on connect and every `poll_s` |

A button is a toggle if `publish.press` is absent. Buttons in a row share the
width equally; rows stack under the 38 px status bar and above a 26 px footer
strip that holds the clock. Limits: 8 rows, 16 buttons, and every state topic
gets exactly one handler (buttons sharing a topic are all updated by it).

## Replacing the config at runtime

```bash
# store a new config — validated, saved to NVS, acked, then the panel reboots
mosquitto_pub -h mqtt.example.com -t cmnd/desk/config -f new_config.json

# back to the firmware default
mosquitto_pub -h mqtt.example.com -t cmnd/desk/config -m '{"reset":true}'
```

The panel answers on `stat/desk/CONFIG`:

```json
{"ok":true,"restarting":true}
{"ok":false,"err":"'BOOST' needs publish.on + publish.off (toggle) or publish.press (momentary)"}
```

An invalid config is rejected and the previous one stays in force — including a
corrupt NVS copy, which is ignored at boot in favour of the firmware default. A
restart is used because `mqttwrapper` cannot unregister handlers; a clean boot is
the only way to drop the old state subscriptions.

## Light hold gate (Node-RED)

Pressing **LIGHT** shows the real light state (from `stat/light/RESULT`,
queried on connect and every 60 s), so a press always sends the opposite of what
the light is actually doing.

The "don't let the radar turn it off while I'm sitting still" hold used to live in
the firmware. It now lives in `nodered/light-hold-gate.json`, because the panel
reboots and loses time, while Node-RED does not:

1. Import the flow (**Menu → Import → select the file**). It creates a tab
   *light hold gate* and a broker config pointing at `mqtt.example.com`;
   if you already have a broker entry, re-select it on the three MQTT nodes and
   delete the imported one.
2. Point the radar's empty-room automation at `cmnd/light/auto` instead of
   `cmnd/light/POWER`. That is the only change needed in the existing
   flows — the gate decides whether the request is allowed through.

How it behaves:

| Input | When | Result |
|-------|------|--------|
| `cmnd/light/POWER` `ON` | always | opens a 10 min window (`flow.hold_min`) |
| `cmnd/light/POWER` `OFF` | always | closes the window, OFF passes through untouched |
| `cmnd/light/auto` `OFF` | window open | swallowed, remembered as *pending* |
| `cmnd/light/auto` `OFF` | window closed | forwarded to the light |
| `stat/light/RESULT` `POWER=OFF` | window open | light put straight back on (catches rules/timers) |
| 30 s tick | window expired + pending | one `OFF` is published, pending cleared |

So a room that is genuinely empty still ends up dark, at most 10 minutes later,
and a human press of LIGHT is always immediate.

The firmware no longer holds anything on its own: with the flow absent, LIGHT is
just a plain on/off switch and the radar behaves as it always did.

## Settings

Only what the panel cannot work without lives in NVS, editable at runtime via
`cmnd/desk/settings`:

```
mqtt_server    mqtt.example.com   mqtt_port 1883
sensor_name    desk               (also the MQTT base topic and panel title)
```

```json
{"sensor_name": "desk"}
```

## Structure

```
main/
  Board.h            board:: CYD pin + size constants
  Config.h/.cpp      cfg:: JSON config -> rows/buttons (parse + validate)
  default_config.json  the compiled-in panel layout
  Display.h/.cpp     display:: ILI9341 panel, XPT2046 touch, backlight, RGB LED
  Font.h             font:: 4bpp-bitmap face/glyph structures
  font_gen.h         generated glyph data — Inter at 22 / 18 / 15 px
  Ui.h/.cpp          ui:: config-driven control surface + touch/MQTT loop
  PanelWebServer.h/.cpp  HTTP: /healthz, /reset, /set_hostname + OTA (/firmware)
  Settings.h         mianesp settingsbase schema (broker, device name)
  main.cpp           app_main(): Wi-Fi + MQTT + settings + cmnd/<name>/config
  Kconfig.projbuild  CYD_BRIGHTNESS
nodered/
  light-hold-gate.json  the light hold (see above)
tools/
  gen_font.py        regenerates main/font_gen.h (needs Pillow)
  fonts/Inter.ttf    Inter variable font + its OFL licence
```

## Dependencies

- `espressif/esp_lcd_ili9341`, `espressif/mqtt`, `espressif/cjson` (registry)
- mianesp (git): `wifimanager`, `nvsstoragemanager`, `settingsbase`,
  `mqttwrapper`, `jsonwrapper`, `webserver`

## Build & flash

```bash
./build.sh            # or: ./build.sh flash   /   ./build.sh monitor
```

## OTA over HTTP

Flash layout is dual-OTA (two 1.875 MB app slots, `partitions.csv`), so once a
panel is on Wi-Fi updates go over the LAN — no cable:

```bash
./build.sh ota 192.168.1.50        # or: OTA_HOST=192.168.1.50 ./build.sh ota
```

That builds, then `POST`s `build/cyd_display_test.bin` to
`http://<ip>/firmware`. The device streams the body straight into the inactive
slot, replies `{"status":"ok","written":…,"partition":"ota_1"}`, and reboots.
Same routes as stillerate2 / ws-voice / mqttradar, so the same `curl` one-liners
work against this panel.

| Route | Method | Purpose |
| --- | --- | --- |
| `/firmware` | GET | running version, build date, partition label |
| `/firmware` | POST | raw `.bin` body → inactive OTA slot → reboot |
| `/healthz` | GET | uptime, heap, version, partition |
| `/reset` | POST | clear Wi-Fi credentials, restart into ESP-Touch |
| `/set_hostname` | POST | `{"host_name":"desk"}` |

`/healthz` also reports `clock_set`: false until NTP has landed. The panel syncs
`pool.ntp.org` at boot (it keeps retrying until Wi-Fi/DNS are up), and formats
timestamps with the POSIX zone in `kTimeZone` in `main/main.cpp` — currently
`AEST-10AEDT,M10.1.0,M4.1.0/3`, i.e. Sydney with NSW DST. Change that one string
for another site, or use `UTC0`.

With no helper script:

```bash
curl -H 'Expect:' --data-binary @build/cyd_display_test.bin http://192.168.1.50/firmware
curl http://192.168.1.50/firmware     # what is running?
```

A freshly-OTA'd image boots **pending verify**: it is only marked valid once it
has an IP (`otaVerifyTask` in `main.cpp`), so an image that panics or never
reaches the network rolls back to the previous slot on the next reset. Wi-Fi
credentials and the stored `uicfg` live in NVS at their usual offsets, so both
survive the swap.

> One wired flash is still needed after this change: the partition table moved
> from a single `factory` app to `otadata` + `ota_0`/`ota_1`. `./build.sh flash`
> rewrites the bootloader and table and leaves NVS (credentials, config) intact.

## First boot — provision Wi-Fi

On first boot (empty NVS) the board starts **ESP-Touch v2** provisioning. Use the
Espressif "EspTouch" app to send your Wi-Fi credentials; the board then connects
and joins the MQTT broker. The RGB LED is green when Wi-Fi + MQTT are both up,
amber for Wi-Fi-only, red for offline.

## Hold gate

See [Light hold gate (Node-RED)](#light-hold-gate-node-red). The
firmware itself no longer holds the light on — it only publishes what you tap.

## Text

The panel is 240x320 on 2.8", i.e. ~143 ppi, so 22 px type is only about 8 pt —
big enough to look normal, small enough that the old 5x7 bitmap font had to be
doubled to 10x14 px to be readable at all. Text is now **Inter** (SIL OFL 1.1),
rendered to 4 bits of coverage per pixel and alpha-blended into the framebuffer,
at three sizes: 22 px for labels, 18 px for the `ON`/`OFF` state, 15 px for the
status bar.

The status bar itself is deliberately quiet: the `title` is set in 18 px with 2 px
of tracking as a wordmark (it is not upper-cased — that stays your choice), behind
a small blue accent bar, while the connection state is one coloured dot plus a
neutral `WIFI`/`MQTT` label, so the bar is chrome rather than a second set of
buttons. A hairline highlight above and a dark rule below lift it off the page.
Long titles drop to 15 px and are then trimmed so they can never run under the
lights.

The bottom 26 px are a footer holding the clock: `HH:MM` in the 18 px face,
centred, no seconds and no date, so it reads as chrome like the status bar. It
stays blank until SNTP has set the time (a clock stuck at 1970 is worse than no
clock) and repaints once a minute. The default config's rows take 232 of the
266 px content area, so the footer costs no button height; with `height: 0`
rows the stack simply shares what is left over. The format is one `strftime`
call in `drawScene()` — `%I:%M` for a 12-hour clock, or add `%a %d` if you want
the date next to it.

Each row picks the largest face whose widest label fits its button, so a cramped
three-across row shrinks as a unit instead of mixing sizes. All three faces
together are ~17 KB of flash (0 RAM) and blending costs little: only glyph pixels
are touched, and the panel repaints on state changes rather than per frame.

Regenerate after changing sizes or typeface:

```bash
python3 -m pip install pillow
python3 tools/gen_font.py     # rewrites main/font_gen.h
```

`tools/gen_font.py` sets the variable font's optical-size and weight axes per
face (`FACES` at the top). The generated header is committed, so ordinary builds
need neither Python nor Pillow. To change typeface, drop another TTF in
`tools/fonts/` and point `TTF` at it.

## Notes

- Status bar shows a green/red dot beside each of `WIFI` and `MQTT`; the RGB LED is
  green when both are up, amber for Wi-Fi only, red for offline.
- Touch uses a calibrated linear map (`kRawXMin`.. in `main/Ui.cpp`); if buttons
  are offset, re-check those four constants.
- `mqttwrapper` only dispatches JSON payloads, so `state.topic` must carry JSON.
  Tasmota's `stat/<name>/RESULT` (`{"POWER":"ON"}`) and the Zigbee bridge's
  `tele/zigbridge/SENSOR` (`{"ZbReceived":{...}}`) both qualify; a plain
  `"ON"/"OFF"` topic does not.
- The desk light has no usable push state on this bridge beyond the `ZbSend`
  read reply on `tele/zigbridge/SENSOR`; the config queries it with
  `{"Device":"0x1234","read":{"Power":1}}`, which is what keeps the DESK button
  honest after a reboot.
- The CYD has no PSRAM, so the screen is drawn in 40-row bands rather than a
  full framebuffer (a full 240x320 RGB565 buffer no longer fits beside Wi-Fi).

