# herVoice Display Bringup — ST77916 QSPI 360×360

## Board & Display

- **MCU board**: Waveshare ESP32-S3-Touch-LCD-1.85C **V2**
- **Display**: ST77916 controller, round IPS 360×360, QSPI interface
- **ESP-IDF**: 5.3.2, ESP32-S3

---

## Wiring (confirmed from Waveshare schematic)

| Signal  | GPIO | Notes |
|---------|------|-------|
| DATA0   | 46   | QSPI IO0 (MOSI) |
| DATA1   | 45   | QSPI IO1 |
| DATA2   | 42   | QSPI IO2 |
| DATA3   | 41   | QSPI IO3 |
| CLK     | 40   | QSPI clock |
| CS      | 21   | Chip select |
| TE      | 18   | Tearing effect (input) |
| BL      | 5    | Backlight PWM (LEDC) |
| RST     | —    | Via TCA9554PWR I2C expander: I2C bus GPIO10/11, addr 0x20, EXIO2 = P1 (bit 0x02) |

---

## Driver Stack

- **Managed component**: `espressif/esp_lcd_st77916` v1.0.1
- **LVGL**: `lvgl/lvgl` (version in `idf_component.yml`)
- **sdkconfig**: `CONFIG_LV_COLOR_16_SWAP=y` — LVGL pre-swaps RGB565 bytes before writing to draw buffer

### QSPI opcodes (used by the driver)

| Operation | Opcode |
|-----------|--------|
| Write command | 0x02 |
| Write color data | 0x32 |
| Command frame width | 32 bits |

---

## Key Decisions Made (and Why)

### 1. V2 chip variant unlock key: `F0 = 0x28`

The managed component's built-in default sequence uses `F0 = 0x08` (a different chip variant). That sequence only drives the middle third of the V2 display. The Waveshare V2 reference uses `F0 = 0x28` plus registers `7C=D1`, `83=E0`, `84=61` on the 0x28 page. These are the first 6 commands in `s_vendor_init`.

### 2. Full GOA block (0x60–0xD9) is required

Without the GOA (Gate Output Array) signal mapping block in Page 0x10, the gate driver cannot address all rows. Adding the full Waveshare GOA block makes the whole circle responsive.

### 3. VCOM registers DD/DE must NOT be set

Adding `DD=0x35, DE=0x35` (present in the component's default sequence) overrides the factory OTP VCOM with the wrong value for V2, causing pixel charge decay (visible as fading within 3 seconds). The Waveshare V2 reference deliberately leaves these out. Factory OTP VCOM is correct for this panel.

### 4. The 0x4C GRAM init command is destructive for V2

The component default includes a `4C=01` command (GRAM initialization at a special row address). On V2 this causes immediate display corruption (dotted line artifacts). Do not add it back.

### 5. INVON (0x21) must NOT be sent

This was the root cause of all the "wrong colours" / "pink/purple instead of dark green" symptoms.

**Panel polarity**: The display is **normally-black** (NB). Without any field applied (0x0000 in GRAM), the LC blocks light → BLACK. With full field (0xFFFF), light passes through → WHITE. This is standard NB-IPS behavior.

**Effect of INVON on NB panel**: INVON inverts all GRAM bits before the LC driver sees them. So:
- Writing 0xFFFF → INVON → 0x0000 → no field → BLACK  
- Writing 0x0280 (dark green) → INVON → 0xFD7F → near-full field → **bright pink/white** ✗

Without INVON:
- 0xFFFF → full field → WHITE ✓  
- 0x0280 → correct partial field → **dark green** ✓
- All LVGL colors display correctly

The Waveshare demo code includes INVON; it may have been tested with a different LVGL colour mapping or a different panel batch. For V2 with standard LVGL color conventions, INVON breaks colour rendering. **Do not add it back.**

### 6. No `swap_xy`, no `mirror`, no `set_gap`

The Waveshare reference and testing confirm: no axis swap, no mirror, no GRAM offset needed. CASET 0–359, RASET 0–359 maps to the full 360×360 visible area.

### 7. QSPI clock: 40 MHz

80 MHz caused issues in a prior session (exact failure mode undocumented). 40 MHz is stable.

### 8. DMA buffers must be internal DRAM

`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` is required. PSRAM (SPIRAM) buffers are not SPI-DMA safe with IDF 5.3. LVGL buffer size: 6480 pixels (360×360 / 20), two buffers for double-buffering.

### 9. C8 and CB charge pump: 0x51

Changed from Waveshare reference value (0x01) to match the component default (0x51) after the middle-section banding investigation. No visible regression.

### 10. GOA timing E0: 0x0A

Changed from 0x08 (Waveshare reference) to 0x0A (component default). No visible regression.

---

## Current State (as of last commit)

### What works
- Full 360×360 circle responds to GRAM writes (confirmed by diagnostic fills)
- LVGL renders and flushes correctly — confirmed by serial log showing flush callbacks with correct pixel values:
  - White background: `px[0..3]=ffff ffff ffff ffff`
  - IDLE dark green: `px[0..3]=8002 8002 8002 8002` (0x0280 byte-swapped = correct dark green)
- Audio subsystem working (ES7210 mic, ES8311 speaker, 440 Hz test tone plays at boot)
- WiFi not yet configured (placeholder credentials)

### Current diagnostic code in `ui.c` (to be removed once display confirmed good)

After `disp_on_off`, before LVGL starts, there is a 2-pass fill:
1. `0x0000` fill (2 s hold) — expected: **BLACK** on NB panel without INVON
2. `0xFFFF` fill (2 s hold) — expected: **WHITE** on NB panel without INVON

**This fill block needs to be removed** once the display visually confirms correct solid colours. Look for the comment `/* DBG: fill entire GRAM with 0x0000 */` in `ui_init()`.

---

## Immediate Next Steps (pick up from here)

### Step 1 — Confirm panel polarity (in progress)

The diagnostic fill was just flashed. After a reset:
- First 2 s: display should go solid **BLACK**
- Next 2 s: display should go solid **WHITE**
- Then LVGL renders: **dark green circle, "Listening..." in white**

Take a photo/note of what each phase shows. Expected on NB panel without INVON:  
`0x0000 → BLACK`, `0xFFFF → WHITE`

If 0x0000 → WHITE and 0xFFFF → BLACK: the panel is NW (normally-white). In that case, add `INVON` back to the vendor init AND invert all LVGL state colours.

### Step 2 — Remove diagnostic code

Once polarity confirmed and LVGL shows dark green correctly:
1. Remove the `/* DBG: fill entire GRAM ... */` block from `ui_init()` in `firmware/components/ui/ui.c`
2. Remove all `ESP_LOGI(TAG, "DBG ...")` flush logging lines from `lcd_flush_cb()`
3. Remove the `static int s_flush_n` counter

### Step 3 — WiFi credentials

In `sdkconfig` (or via `idf.py menuconfig`):
```
CONFIG_HERVOICE_WIFI_SSID="your_ssid"
CONFIG_HERVOICE_WIFI_PASSWORD="your_password"
```

### Step 4 — Flash wake word model

The `model` partition at `0x310000` (3 MB) needs the wake-word binary:
```
idf.py -p PORT flash  # already done as part of normal flash
```
The `srmodels.bin` is included in the build output and flashed automatically. However the serial log shows:
```
Please check if there are target files in /srmodel/wn9_sophia_tts/wn9_index
```
This may require placing model files in the correct SPIFFS path or verifying the partition layout.

### Step 5 — State machine integration

The `ui_set_state(ui_state_t state)` API is ready. Hook it up to the voice pipeline state machine:
- `UI_STATE_IDLE` — dark green, "Listening..."
- `UI_STATE_WAKE` — amber, "Wake!"
- `UI_STATE_RECORDING` — dark red, "Recording..."
- `UI_STATE_SENDING` — blue, "Processing..."
- `UI_STATE_PLAYING` — teal, "Speaking..."
- `UI_STATE_ERROR` — bright red, "Error"

---

## Init Sequence Reference (`s_vendor_init` in `ui.c`)

```
Page 0x28 unlock:
  F0=28, F2=28, 7C=D1, 83=E0, 84=61, F2=82

Page 0x01 (power/timing):
  F0=00, F0=01, F1=01
  B0=49, B1=4A, B2=1F, B4=46, B5=34, B6=D5, B7=30
  B8=04, BA=00, BB=08, BC=08, BD=00
  C0=80, C1=10, C2=37, C3=80, C4=10, C5=37
  C6=A9, C7=41, C8=51, C9=A9, CA=41, CB=51
  D0=91, D1=68, D2=68, F5={00,A5}
  [NO DD/DE — factory VCOM OTP]
  F1=10

Page 0x02 (gamma):
  F0=00, F0=02
  E0={70,09,12,0C,0B,27,38,54,4E,19,15,15,2C,2F}
  E1={70,08,11,0C,0B,27,38,43,4C,18,14,14,2B,2D}

Page 0x10 (GOA timing + signal mapping):
  F0=10, F3=10
  E0=0A, E1=00, E2=0B, E3=00, E4=E0, E5=06, E6=21, E7=00
  E8=05, E9=82, EA=DF, EB=89, EC=20, ED=14, EE=FF, EF=00
  F8=FF, F9=00, FA=00, FB=30, FC=00, FD=00, FE=00, FF=00
  GOA signal groups 0x60-0xD9 (full block, see source)
  F3=01, F0=00

[NO INVON — NB panel, INVON would invert all colours]
11=00 (SLPOUT, 120 ms delay)
```

---

## File Map

| File | Purpose |
|------|---------|
| `firmware/components/ui/ui.c` | All display + LVGL init and state rendering |
| `firmware/components/ui/ui.h` | Public API: `ui_init()`, `ui_set_state()`, `ui_get_state()` |
| `firmware/managed_components/espressif__esp_lcd_st77916/` | ST77916 QSPI panel driver (managed component, do not edit) |
| `firmware/sdkconfig` | Build config — `LV_COLOR_16_SWAP=y` at line ~2208 |
| `docs/display_bringup.md` | This file |
