# E-ink waveform and refresh pipeline

How inx drives the panel's waveform: which code picks a refresh mode, how that mode becomes SSD1677 (X4) or X3
controller commands, where each LUT lives and gets loaded, and how temperature enters the picture.

There is no waveform file on SD or flash. Two waveform sources exist:

- **Controller OTP waveform** (X4 / SSD1677 only): selected by the controller itself when `DISPLAY_UPDATE_CTRL2`
  (`0x22`) has the `LUT_LOAD` bit set. Used for FULL, HALF and plain FAST refreshes.
- **Compiled-in LUTs** (`PROGMEM` arrays in `EInkDisplay.cpp`): pushed over SPI before a refresh. On X4 these are
  only the grayscale LUTs; on X3 every refresh uploads a full 5-register LUT bank.

Node tags like `[D3]` refer to the legend below each diagram.

## 1. Black/white update path

```mermaid
flowchart TD
    subgraph APP["App layer (src/)"]
        R1["[A1] Readers: page-turn countdown<br/>pagesUntilFullRefresh <= 1 ? HALF : FAST"]
        R2["[A2] Epub smart refresh<br/>HALF after a page with a large image"]
        R3["[A3] Menus, drawers, pickers<br/>FAST (default arg)"]
        R4["[A4] Sleep screen pre-clear<br/>HALF"]
        R5["[A5] Refresh-on-load pages, shelf exit<br/>HALF"]
        R6["[A6] OTA finished screen<br/>FULL"]
        R7["[A7] Power button / PAGE_REFRESH action<br/>MANUAL"]
        SET["[A8] ReaderSetting::getRefreshFrequency<br/>1 / 5 / 10 / 15 / 30 pages"]
    end

    SET --> R1
    R1 & R2 & R3 & R4 & R5 & R6 & R7 --> GFX

    GFX["[G1] GfxRenderer::displayBuffer(mode)"] --> HAL

    subgraph HALL["HAL (lib/hal)"]
        HAL["[H1] HalDisplay::displayBuffer"]
        CONV{"[H2] convertRefreshMode"}
        RESYNC["[H3] X3 + MANUAL:<br/>EInkDisplay::requestResync()"]
        HAL -->|"MANUAL on X3"| RESYNC
        HAL --> CONV
    end

    CONV -->|"FULL→FULL, HALF→HALF,<br/>FAST→FAST, STRONG_FAST→STRONG_FAST"| DB
    CONV -->|"MANUAL: X4→HALF, X3→FAST"| DB
    RESYNC --> DB

    subgraph DRV["Driver (open-x4-sdk/libs/display/EInkDisplay)"]
        DB["[D1] EInkDisplay::displayBuffer(mode)"]
        FORCEHALF["[D2] X4 and screen off:<br/>mode forced to HALF"]
        REVERT["[D3] inGrayscaleMode?<br/>grayscaleRevert():<br/>setCustomLUT(lut_grayscale_revert) + FAST"]
        ISX3{"_x3Mode?"}
        DB --> FORCEHALF --> REVERT --> ISX3

        %% X4 branch
        RAMW["[D4] setRamArea + writeRamBuffer<br/>FULL/HALF: frame→0x24 BW and 0x26 RED<br/>FAST: frame→0x24 BW only"]
        STRONG["[D5] STRONG_FAST:<br/>4 × refreshDisplay(FAST)"]
        RD["[D6] EInkDisplay::refreshDisplay(mode)"]
        SYNCRED["[D7] single-buffer mode:<br/>frame→0x26 RED after refresh"]
        ISX3 -->|"X4"| RAMW --> STRONG --> RD
        RAMW --> RD
        RD --> SYNCRED

        %% X3 branch
        X3DEC{"[D8] doFullSync =<br/>FULL || !redRamSynced ||<br/>initial sync || forced resync"}
        X3FULL["[D9] Upload lut_x3_*_img bank (0x20–0x24)<br/>inverted frame → 0x13 and 0x10<br/>0x50 = A9 07"]
        X3FAST["[D10] Upload lut_x3_*_full bank (0x20–0x24)<br/>frame → 0x13 (diff vs 0x10)<br/>0x50 = 29 07"]
        X3TRIG["[D11] 0x04 power on (if needed)<br/>0x12 refresh, 0x02 off (optional)<br/>frame → 0x10 for next diff"]
        ISX3 -->|"X3 (HALF treated as fast)"| X3DEC
        X3DEC -->|"yes"| X3FULL --> X3TRIG
        X3DEC -->|"no"| X3FAST --> X3TRIG
    end

    subgraph SSD["SSD1677 controller (X4)"]
        CTRL1["[C1] 0x21 DISPLAY_UPDATE_CTRL1<br/>FAST: 0x00 compare RED vs BW<br/>FULL/HALF: 0x40 bypass RED"]
        CTRL2{"[C2] 0x22 DISPLAY_UPDATE_CTRL2<br/>(+0xC0 if screen was off,<br/>+0x03 if turnOffScreen)"}
        FULLW["FULL: 0x34<br/>TEMP_LOAD + LUT_LOAD + START<br/>OTP waveform for sensed temp"]
        HALFW["[C3] HALF: 0x1A ← 0x5A (fake hot temp)<br/>0xD4 LUT_LOAD + START, no TEMP_LOAD<br/>shorter OTP waveform"]
        FASTW["FAST, no custom LUT: 0x1C<br/>LUT_LOAD + MODE2 + START"]
        FASTC["FAST, custom LUT active: 0x0C<br/>MODE2 + START (uses uploaded LUT)"]
        ACT["[C4] 0x20 MASTER_ACTIVATION"]
        CTRL1 --> CTRL2
        CTRL2 --> FULLW & HALFW & FASTW & FASTC --> ACT
    end

    RD --> CTRL1
    ACT --> BUSY
    X3TRIG --> BUSY

    BUSY["[D12] waitWhileBusy / waitForRefresh<br/>polls BUSY pin, calls busyHook ~1 ms"]
    BUSY -.->|"busyHook"| HOOK["[A9] onDisplayBusy (main.cpp)<br/>gpio.pollInput + renderer.runRefreshJob"]
    BUSY --> PANEL[("E-ink panel")]
```

### Legend — diagram 1

| Tag | What | Where |
|---|---|---|
| A1 | Reader cadence: HALF when the countdown hits 1, else FAST via the default arg | `src/activity/reader/Epub/EpubActivity.cpp:2018-2026` (`displayPageBuffer` lambda), `src/activity/reader/TxtReaderActivity.cpp:440-446`, `src/activity/reader/XtcReaderActivity.cpp:736-757, 789-795, 856-862` (uses `xtcRefreshFrequency`, `src/state/ReaderSetting.h:57`) |
| A2 | `smartRefreshAfterLargeImage` forces HALF on the page after a large image | `src/activity/reader/Epub/EpubActivity.cpp:1988, 2017-2019` |
| A3 | Most UI redraws call `displayBuffer()` / `displayBuffer(HalDisplay::FAST_REFRESH)` | e.g. `src/activity/reader/Epub/MenuDrawer.cpp:382`, `src/activity/page/components/recent/HomeMenuDrawer.ipp:216`, `src/system/ScreenComponents.cpp:164` |
| A4 | White HALF pre-clear before the sleep image so the quality LUT starts from clean white | `src/activity/system/SleepActivity.cpp:294-305` |
| A5 | Optional HALF on entering Recent/Library/Settings/Sync/Stats; HALF after leaving shelf mode | `SystemSetting::runHalfRefreshOnLoadIfEnabled` `src/state/SystemSetting.cpp:574-599`; `src/activity/page/LibraryActivity.cpp:1512-1517` |
| A6 | FULL refresh once OTA finishes, to clear the long-lived progress screen | `src/activity/settings/OtaUpdateActivity.cpp:423-426` |
| A7 | Manual refresh: global short-press power and the reader's `BTN_ACTION_PAGE_REFRESH` | `handleGlobalPowerRefresh` `src/main.cpp:309-322`; `src/activity/reader/Epub/ReaderButtonBindings.cpp:131-132` |
| A8 | Maps the stored `REFRESH_FREQUENCY` enum to page counts | `ReaderSetting::getRefreshFrequency` `src/state/ReaderSetting.cpp:462-476` |
| A9 | Busy hook installed at boot; keeps input alive and runs the queued prefetch job during a refresh | `onDisplayBusy` `src/main.cpp:287-297`, `display.setBusyHook` `src/main.cpp:304`, `GfxRenderer::runRefreshJob` `lib/GfxRenderer/GfxRenderer.cpp:254` |
| G1 | Thin pass-through to the HAL | `GfxRenderer::displayBuffer` `lib/GfxRenderer/GfxRenderer.cpp:252` |
| H1–H3 | HAL adds `MANUAL_REFRESH` (not in the driver enum) and translates it per device | `lib/hal/HalDisplay.h:17`, `convertRefreshMode` / `HalDisplay::displayBuffer` `lib/hal/HalDisplay.cpp:29-57` |
| D1 | Driver entry for B/W updates | `EInkDisplay::displayBuffer` `open-x4-sdk/libs/display/EInkDisplay/src/EInkDisplay.cpp:778` |
| D2 | X4 only: first update after power-off is promoted to HALF | `EInkDisplay.cpp:779-782` |
| D3 | Leaving medium grayscale runs the revert LUT as a FAST refresh first | `grayscaleRevert` `EInkDisplay.cpp:653-664`, LUT `lut_grayscale_revert` `EInkDisplay.cpp:81-112` |
| D4 | RAM writes; firmware builds with `EINK_DISPLAY_SINGLE_BUFFER_MODE=1` so the RED (previous-frame) RAM is never rewritten before FAST | `EInkDisplay.cpp:895-910`, `platformio.ini:27, 89, 145` |
| D5 | `STRONG_FAST_REFRESH` = four FAST passes (no caller in `src/` uses it today) | `EInkDisplay.cpp:916-920` |
| D6 | Builds the `0x21`/`0x22` command bytes; on X3 it just recurses to `displayBuffer` | `EInkDisplay::refreshDisplay` `EInkDisplay.cpp:1176-1237` |
| D7 | Re-syncs RED RAM to the shown frame so the next FAST diff is correct | `EInkDisplay.cpp:925-930` |
| D8–D11 | X3 policy: full sync with image LUTs vs. differential fast with full LUTs; HALF is deliberately treated as fast | `EInkDisplay.cpp:789-893`; LUT banks `lut_x3_*_full` `:115-134`, `lut_x3_*_img` `:171-197`; `requestResync` `:212`; one initial full sync armed in `begin()` `:297` |
| D12 | BUSY-pin wait loops with 30 s timeout and hook callback | `waitForRefresh` `EInkDisplay.cpp:354-379`, `waitWhileBusy` `:408-435` |
| C1–C4 | SSD1677 update-control encoding (bit names from the driver's comment table) | `EInkDisplay.cpp:1182-1232`; command defines `:28-39` |

## 2. Grayscale / custom-LUT path

```mermaid
flowchart TD
    subgraph CALLERS["Callers"]
        E1["[E1] Epub: text anti-aliasing<br/>quality=false, preserveText=true"]
        E2["[E2] Epub: high-quality images<br/>quality=true, fastQuality=kReaderHighQualityFastLut"]
        E3["[E3] Sleep cover / custom image<br/>quality=true, preserveText=false"]
        E4["[E4] XTC 2-bit pages<br/>quality=true"]
        E5["[E5] ImageDisplayCache::displayTwoBitIfAvailable<br/>cached 2-bit planes from SD"]
    end

    E1 & E2 & E3 & E4 --> RGP
    E2 -.->|"via ImageRender::displayGrayscale"| RGP

    RGP["[G2] GfxRenderer::renderGrayscalePasses"]
    RGP --> PREP{"quality && !X3?"}
    PREP -->|"yes"| PQ["[D13] prepareQualityGrayscale<br/>0x21=0x40, 0x22=0x03 power down"]
    PREP -->|"no"| LSB
    PQ --> LSB
    E5 --> LSB

    LSB["[G3] RenderMode GRAY2_LSB / GRAYSCALE_LSB<br/>drawPlane() → copyGrayscaleLsbBuffers"]
    MSB["[G4] RenderMode GRAY2_MSB / GRAYSCALE_MSB<br/>drawPlane() → copyGrayscaleMsbBuffers"]
    LSB --> MSB

    LSB -.->|"X4: 0x24 BW RAM<br/>X3: 0x10 (mirrored rows)"| CRAM[("Controller RAM")]
    MSB -.->|"X4: 0x26 RED RAM<br/>X3: 0x13 (only if LSB valid)"| CRAM

    MSB --> PICK{"fastQuality && quality && !X3?"}
    PICK -->|"yes"| DGFQ["[D14] displayGrayBufferFastQuality<br/>→ displayGrayBuffer(lut_x4_quality_fast)"]
    PICK -->|"no"| DGB["[D15] displayGrayBuffer(quality, trackForRevert)"]
    DGFQ --> DGB

    DGB --> X3G{"_x3Mode?"}
    X3G -->|"X3"| X3GRAY["[D16] quality ? lut_x3_*_img : lut_x3_*_gray<br/>0x20–0x24 bank, 0x50 = A9/29 07<br/>0x04 on, 0x12 refresh"]
    X3G -->|"X4"| SEL["[D17] LUT select:<br/>explicit lutData, else<br/>quality ? lut_x4_quality : lut_grayscale"]

    SEL --> SCL["[D18] setCustomLUT(true, lut)<br/>0x32 ← bytes 0–104 (VS, TP/RP, FR)<br/>0x03 ← VGH, 0x04 ← VSH1/VSH2/VSL<br/>0x2C ← VCOM"]
    SCL --> QQ{"quality?"}
    QQ -->|"yes"| QREF["[D19] settle 120 ms (skipped for fast LUT)<br/>0x21=0x00, 0x22=0xC7, 0x20<br/>inGrayscaleMode=false"]
    QQ -->|"no (medium)"| MREF["[D20] refreshDisplay(FAST) → 0x22=0x0C<br/>inGrayscaleMode = trackForRevert"]
    QREF & MREF --> OFF["setCustomLUT(false)"]

    OFF & X3GRAY --> CLEAN
    CLEAN["[G5] Rebase B/W baseline<br/>preserveText: restoreBwBuffer<br/>else: clearScreen(0xFF) + cleanupGrayscaleWithFrameBuffer"]
    CLEAN --> CGB["[D21] cleanupGrayscaleBuffers(bw)<br/>X4: bw → 0x24 and 0x26<br/>X3: bw → 0x13 and 0x10, redRamSynced=true"]
    CGB --> NEXT["Next B/W displayBuffer:<br/>if inGrayscaleMode → grayscaleRevert [D3]"]
```

### Legend — diagram 2

| Tag | What | Where |
|---|---|---|
| E1 | Text AA second pass over a stored B/W frame | `src/activity/reader/Epub/EpubActivity.cpp:2054-2069` |
| E2 | Quality image pass with the fast quality LUT (`kReaderHighQualityFastLut = true`) | `EpubActivity.cpp:73, 2043-2053`; `ImageRender::displayGrayscale` `lib/GfxRenderer/ImageRender.cpp:237-240` |
| E3 | Sleep image quality grayscale | `src/activity/system/SleepActivity.cpp:504-512, 591-594` |
| E4 | XTC 2-bit page render | `src/activity/reader/XtcReaderActivity.cpp:764` |
| E5 | Pre-rendered 2-bit planes replayed from the SD cache; runs the same LSB/MSB/display/cleanup steps inline | `ImageDisplayCache::displayTwoBitIfAvailable` `lib/GfxRenderer/ImageDisplayCache.cpp:262-313`; entry from `ImageRender::displayCachedTwoBit` `lib/GfxRenderer/ImageRender.cpp:158` |
| G2 | Shared LSB→MSB→refresh→rebase sequence | `GfxRenderer::renderGrayscalePasses` `lib/GfxRenderer/GfxRenderer.cpp:451-486` |
| G3–G4 | Render modes `GRAYSCALE_*` (medium, `lut_grayscale`) vs `GRAY2_*` (quality); quality mid-tones are swapped by `mapQualityGray2Level` so both devices route levels to the same LUT entries | `lib/GfxRenderer/GfxRenderer.h:28`; `GfxRenderer.cpp:302-304`; `lib/GfxRenderer/BitmapUtil.cpp:169-174`; driver RAM writes `EInkDisplay.cpp:666-734` |
| G5 | Restore or blank the B/W frame after a gray refresh | `restoreBwBuffer` `GfxRenderer.cpp:402-437`, `cleanupGrayscaleWithFrameBuffer` `:444-449` |
| D13 | Powers analog down so the quality waveform starts from a known state (X4 only) | `EInkDisplay::prepareQualityGrayscale` `EInkDisplay.cpp:1121-1133` |
| D14 | Fast quality on X4; HAL routes X3 to `displayGrayBuffer(…, quality=true)` instead | `EInkDisplay.cpp:1117-1119`; `HalDisplay::displayGrayBufferFastQuality` `lib/hal/HalDisplay.cpp:77-83` |
| D15 | Gray refresh entry; HAL always passes `lutData = nullptr` | `EInkDisplay::displayGrayBuffer` `EInkDisplay.cpp:1009`; `HalDisplay::displayGrayBuffer` `lib/hal/HalDisplay.cpp:73-75` |
| D16 | X3 gray: tuned 4-level bank or the stock image bank | `EInkDisplay.cpp:1011-1085`; `lut_x3_*_gray` `:144-168`, `lut_x3_*_img` `:171-197` |
| D17 | X4 LUT choice | `EInkDisplay.cpp:1092-1093`; `lut_grayscale` `:45-79`, `lut_x4_quality` `:222-228`, `lut_x4_quality_fast` `:214-220` |
| D18 | LUT upload layout (110 bytes: 105 waveform + 5 voltages) | `EInkDisplay::setCustomLUT` `EInkDisplay.cpp:1239-1267` |
| D19–D20 | Quality fires `0xC7` directly; medium uses FAST with custom LUT (`0x0C`, no OTP load) and arms the revert | `EInkDisplay.cpp:1087-1115`; FAST custom-LUT bit choice `:1222-1223` |
| D21 | Rewrites both RAM planes from the B/W frame so the next differential update has a coherent base | `EInkDisplay::cleanupGrayscaleBuffers` `EInkDisplay.cpp:742-775` |

## 3. Temperature handling

```mermaid
flowchart LR
    INIT["[T1] initDisplayController (X4)<br/>0x18 ← 0x80 internal temp sensor"] --> SENSOR[("SSD1677 internal sensor")]
    FULLT["[T2] FULL: 0x22 has TEMP_LOAD (0x20)<br/>controller reads sensor, picks OTP waveform"] --> SENSOR
    HALFT["[T3] HALF: 0x1A ← 0x5A written as temperature<br/>0x22 = 0xD4 without TEMP_LOAD<br/>→ OTP picks the hot (short) waveform"]
    CUST["[T4] Custom LUTs (gray, revert, quality)<br/>fixed timings and voltages, no temp compensation"]
    X3T["[T5] X3: no temperature commands sent;<br/>LUT banks are fixed"]
```

| Tag | What | Where |
|---|---|---|
| T1 | Internal sensor selected once at controller init | `EInkDisplay.cpp:489-497` (`CMD_TEMP_SENSOR_CONTROL` `:14`) |
| T2 | FULL refresh loads temperature and the matching OTP waveform | `EInkDisplay.cpp:1215-1216` |
| T3 | HALF writes a fake high temperature to get the faster OTP waveform | `EInkDisplay.cpp:1217-1221` (`CMD_WRITE_TEMP 0x1A` `:39`) |
| T4 | All four X4 custom LUTs carry their own frame rate and VGH/VSH1/VSH2/VSL/VCOM bytes | `EInkDisplay.cpp:45-112, 214-228`, applied in `setCustomLUT` `:1249-1259` |
| T5 | X3 init and refresh paths contain no `0x18`/`0x1A`-style temperature commands | `EInkDisplay.cpp:437-484, 789-893, 1011-1085` |

## Notes

- `EInkDisplay::displayWindow`, `EInkDisplay::displayGrayBufferWindow` and `STRONG_FAST_REFRESH` exist in the
  driver but have no callers in `src/` or `lib/`.
- Power-down for sleep: `src/main.cpp:256` → `HalDisplay::deepSleep` → `EInkDisplay::deepSleep`
  (`EInkDisplay.cpp:1269-1293`: `0x22 = 0x03` if powered, then `0x10 ← 0x01`).
- `turnOffScreen` is never passed by the HAL's `displayBuffer`, so the X4 analog rails stay on between updates
  until a quality gray refresh (`0xC7` includes power-off bits), `prepareQualityGrayscale`, or `deepSleep` turns
  them off — which is what makes [D2] promote the next update to HALF.
