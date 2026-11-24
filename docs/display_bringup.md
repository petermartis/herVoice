# ST77916 display implementation

This document describes the **current deployed display path** for the Waveshare ESP32-S3-Touch-LCD-1.85C V2. Earlier bring-up experiments used different clock rates and register values; those are historical and must not override the current source.

Authoritative implementation: `firmware/components/ui/ui.c`.

## Hardware

| Item | Value |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-1.85C V2 |
| Panel | ST77916, round 360×360 IPS |
| Interface | QSPI |
| UI framework | LVGL 8.3-era managed dependency |
| Pixel format | RGB565 with configured byte ordering |

### Pin map

| Signal | GPIO | Notes |
|---|---:|---|
| QSPI D0 | 46 | data lane 0 |
| QSPI D1 | 45 | data lane 1 |
| QSPI D2 | 42 | data lane 2 |
| QSPI D3 | 41 | data lane 3 |
| QSPI clock | 40 | 80 MHz current configuration |
| Chip select | 21 | direct GPIO |
| Tearing effect | 18 | panel TE input path |
| Backlight | 5 | direct PWM/LEDC control |
| Panel reset | TCA9554 EXIO2 | expander address `0x20` on GPIO10/11 I2C bus |

Backlight and chip select are not routed through TCA9554 in the current hardware path. TCA9554 EXIO1 is reserved for CST816S touch reset; all expander writes must preserve both reset bits.

## Current stable transfer configuration

The stable deployed combination is:

- QSPI pixel clock: **80 MHz**
- transfer queue depth: **1**
- `lv_disp_flush_ready()` called synchronously after the panel draw operation completes
- two LVGL draw buffers, each 6480 RGB565 pixels
- draw buffers allocated with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`
- no PSRAM-backed LCD DMA buffers
- visualizer size updates paced to 20 FPS and skipped when unchanged

The older statement that 40 MHz was the stable clock is obsolete. It avoided crashes during one bring-up stage but produced a rough/grainy image on the current panel. Eighty megahertz gives the clean display used by the production firmware.

A queue depth greater than one combined with premature flush-ready signaling can race DMA and produce stripes. Calling LVGL flush-ready from an unsafe callback/ISR path has also caused memory faults. Preserve the current synchronous design unless a replacement is tested with audio playback under load.

## Driver and command protocol

The project requests the managed `espressif/esp_lcd_st77916` component in `firmware/main/idf_component.yml`. There is no tracked vendored v2 driver directory in the current repository.

QSPI command framing uses the driver's ST77916 command path. The panel-specific behavior comes from the full `s_vendor_init` table in `ui.c`, not the managed component's shorter default table.

## Panel initialization sequence

The current source uses the complete approximately 188-command Waveshare/teason2021-derived table. It includes:

- page/unlock setup
- power and timing configuration
- full GOA mapping blocks
- positive and negative gamma tables
- panel inversion command `INVON` (`0x21`)
- `SLPOUT` (`0x11`) followed by a 120 ms delay
- `DISPON` (`0x29`) followed by a 20 ms delay

Current source-specific values include:

- `DD=0x4F`, `DE=0x4F`
- charge-pump values `C8=0x01`, `CB=0x01`
- page-0x10 `E0=0x07`
- `INVON 0x21` present

Do not replace these with values from the previous experimental document (`no DD/DE`, `C8/CB=0x51`, `E0=0x0A`, or “never INVON”). Those claims described intermediate experiments and contradict the deployed table.

For maintenance, compare the complete byte table in source. A summarized hand-copied register list is not sufficiently authoritative for the GOA groups.

## Initialization flow

At a high level, `ui_init()`:

1. initializes the shared I2C/expander state needed for reset control
2. resets and initializes the ST77916 panel
3. applies the complete vendor command table
4. enables and configures the backlight
5. performs current diagnostic full-screen fills
6. creates DMA-capable LVGL draw buffers
7. registers the display flush path
8. constructs state labels and the centered level halo
9. starts the LVGL/UI task

The UI is initialized before WakeNet and audio in `main.c`. This gives immediate visual boot feedback and ensures panel reset activity completes before touch starts using the shared expander.

## Diagnostic fills still present

The audited `ui.c` still performs temporary full-screen `0x0000` and `0xFFFF` diagnostic fills with approximately two-second holds and emits initial display debug output before LVGL starts.

These diagnostics are no longer needed to prove basic panel operation and delay startup. Remove them in a dedicated change after verifying that no remaining timing dependency relies on the delay. Test cold boots, warm resets, audio startup, and touch after removal.

## LVGL state machine

| State | Label | Background |
|---|---|---|
| `UI_STATE_IDLE` | Ready | dark green `(0,80,0)` |
| `UI_STATE_WAKE` | Ready | dark green `(0,80,0)` |
| `UI_STATE_RECORDING` | Listening... | dark red `(160,0,0)` |
| `UI_STATE_SENDING` | Processing... | blue `(0,0,140)` |
| `UI_STATE_PLAYING` | Speaking... | teal `(0,120,120)` |
| `UI_STATE_ERROR` | Error | bright red `(200,0,0)` |

There is intentionally no yellow Wake screen. The wake/connect transition reuses Ready to avoid a distracting flash.

During a wake-free follow-up, the display can show Ready while the device waits for speech onset; it changes to Listening only after VAD detects speech. Ready therefore means either normal idle or an armed pre-speech follow-up window.

## Audio level halo

Recording and playback states show a centered translucent halo behind the label:

- recording: pink/red, driven by microphone raw RMS with recording-specific scale compensation
- playback: ice blue, driven by speaker sample peaks
- fast attack and slower decay smooth the visible response
- size updates are limited to one every 50 ms
- the object size is changed only when the computed size differs

The 20 FPS cap is a functional audio invariant. Unpaced display updates contend with QSPI, PSRAM, and audio traffic and have produced choppy streamed TTS.

## Full-screen GIF warning

A 360×360 LVGL GIF decoder needs roughly 518 KB for canvas/frame state at RGB565. In the standard allocator path this can exceed internal DRAM and fail silently. Moving decoder storage to PSRAM solves allocation but does not solve bus/CPU pressure: even a hidden GIF can leave its decode timer active and disrupt audio.

The production UI does not depend on an animated GIF. Do not re-enable one without:

1. explicit PSRAM allocation and matching free path
2. explicit timer pause/delete when hidden
3. audio-underrun testing during streamed TTS
4. cold-boot memory-headroom testing with Wi-Fi and WakeNet active

## Touch interaction with the display

The CST816S uses direct interrupt GPIO4 and reset through TCA9554 EXIO1. Display reset uses EXIO2. A whole-register expander write that sets only EXIO2 can hold touch in reset indefinitely; preserve the other bit on every write.

The touch task can temporarily show volume feedback. The normal voice state remains owned by the network/session flow.

After an esptool hard reset, touch may remain inactive until a true USB power cycle. Do not diagnose display/touch integration solely from the first post-flash reset.

## Debugging matrix

| Symptom | First checks |
|---|---|
| Black screen with no LVGL | panel reset, complete vendor table, draw-buffer allocation, backlight GPIO5 |
| Grainy/rough background | confirm QSPI is 80 MHz, not 40 MHz |
| Vertical stripes | queue depth, flush-ready timing, concurrent transfers |
| Wrong colors | byte-swap setting and exact current `INVON`/vendor table; do not apply old experimental advice |
| Panel works but TTS becomes choppy | halo pacing, GIF/decode timers, excessive redraws, PSRAM contention |
| Display works but touch is dead | true power cycle, EXIO1 preservation, register `0xFE=10`, GPIO4 interrupt/polling |
| Reset during display init | internal DMA memory headroom and callback context |

## Verification after display changes

A display change is accepted only after:

1. four consecutive cold or reset boots without panic
2. uniform Ready background with no grain, stripes, or corruption
3. correct state colors and centered labels
4. responsive tap and swipe gestures after a real power cycle
5. smooth recording/playback halo
6. uninterrupted TTS streaming during continuous UI animation
7. Wi-Fi and WakeNet still initialize with sufficient internal memory

## Source map

| File | Responsibility |
|---|---|
| `firmware/components/ui/ui.c` | panel bus/init table, LVGL, states, halo, backlight |
| `firmware/components/ui/include/ui.h` | UI API and state enumeration |
| `firmware/components/touch/touch.c` | CST816S, gestures, shared expander reset behavior |
| `firmware/main/main.c` | subsystem initialization order |
| `firmware/main/idf_component.yml` | managed display/LVGL dependencies |
| `firmware/sdkconfig.defaults` | durable memory/PSRAM/system settings |

When changing the panel table, update this document in the same commit and quote values from the current source, not from an earlier bring-up log.
