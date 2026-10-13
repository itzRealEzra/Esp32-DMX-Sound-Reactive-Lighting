## 1. What the Numbers Mean

The dB value is **relative**, not real SPL.

```
dB shown = (loudness above the room's noise floor) x SENSITIVITY + QUIET_DB
```

- A silent room reads about **25** (`QUIET_DB`).
- Zone 1 starts at **30**, which is only about **5 dB above the room floor** at default sensitivity.
- The noise floor is learned at startup, so the same numbers work in different rooms.

Default levels:

| dB | Result |
|---|---|
| below 30 | Zone 0, white |
| 30 to 32 | Zone 1, red |
| 32 to 34 | Zone 2, green |
| 34 to 36 | Zone 3, blue |
| 36 and up | Zone 4, rainbow |
| 37 and up | Strobe (on top of zone 4) |

---

## 2. Step 1: Look at Your Real Numbers

1. Upload the sketch and open **Serial Monitor** at **115200 baud**.
2. Keep the room quiet for the first 3 seconds (calibration).
3. Play your usual music at your usual volume.
4. Watch this line (4 times a second):

```
dB: 33.2 floor: 41.0 zoneReq: 2 strobe: 0
```

Write down three numbers:

| What to note | Example |
|---|---|
| **Quiet part** (between songs) | 26 |
| **Normal part** (usual loudness) | 33 |
| **Peak** (drops, loud hits) | 40 |

---

## 3. Step 2: Set the Zone Levels

Edit these two arrays in `CONFIGURATION`:

```cpp
//                         zone:  0      1   2   3   4
const float ZONE_LO[5] = { -1000,  30,  32, 34, 36 };
const float ZONE_HI[5] = {    30,  32,  34, 36, 1000 };
```

**Rules**

- Each `ZONE_HI[z]` must equal `ZONE_LO[z+1]`. No gaps, no overlaps.
- Keep `-1000` as the first low and `1000` as the last high.
- Numbers must go up from left to right.

**How to pick them from your notes**

- Zone 1 starts a little above your **quiet** level.
- Zone 4 (rainbow) starts a little below your **peak**.
- Split the space in between for zones 2 and 3.

Example from the notes above (quiet 26, normal 33, peak 40):

```cpp
const float ZONE_LO[5] = { -1000, 29, 32, 35, 38 };
const float ZONE_HI[5] = {    29, 32, 35, 38, 1000 };
```

---

## 4. Step 3: Set the Strobe Level

```cpp
const float STROBE_DB = 37.0f;
```

- Strobe only works while zone 4 is showing, so keep `STROBE_DB` **at or above** `ZONE_LO[4]`.
- It turns **on** at `STROBE_DB` and **off** below `STROBE_DB - HYST`.

| Goal | Value |
|---|---|
| Strobe right when rainbow starts | Same as `ZONE_LO[4]` |
| Strobe only on big peaks | A few dB above `ZONE_LO[4]` (example: 40) |
| **No strobe at all** | `1000.0f` |

With the example zones above (zone 4 at 38), use `STROBE_DB = 40.0f`.

---

## 5. Overall Sensitivity (Shift Everything at Once)

Instead of editing every zone, scale the whole thing:

```cpp
const float SENSITIVITY = 1.0f;
```

| Value | Effect |
|---|---|
| 0.5 | Needs twice the sound to reach the same zone |
| 0.7 | Calmer |
| **1.0** | Default |
| 1.5 | Reacts to quieter sound |
| 2.0 | Very reactive |

Quick math for zone 1 (starts at 30, `QUIET_DB` 25, so 5 dB of "headroom"):
dB above floor needed = 5 / `SENSITIVITY`

- SENSITIVITY 1.0: 5 dB above the floor
- SENSITIVITY 2.0: 2.5 dB above the floor
- SENSITIVITY 0.5: 10 dB above the floor

**Use `SENSITIVITY` for quick changes and the zone arrays for fine control.**

---

## 6. Baseline Shift

```cpp
const float QUIET_DB = 25.0f;
```

This is what a silent room shows. Raising it shifts **every** reading up by the same amount, so it has the same effect as lowering all the zone thresholds together.

You usually do not need to touch it. If you do change it, keep zone 1 above it, or the lights will never sit in white at idle.

---

## 7. Stopping Flicker at Borders

```cpp
const float HYST = 1.0f;
```

The sketch stays in a zone until the level moves `HYST` dB past the border.

| Symptom | Change |
|---|---|
| Colors flicker between two zones | Raise `HYST` to 1.5 or 2.0 |
| Zones feel sticky and slow to change | Lower `HYST` to 0.5 |

Keep `HYST` smaller than half your narrowest zone width (default zones are 2 dB wide, so 1.0 is already the practical max).

---

## 8. How Fast the Level Falls

```cpp
const float RELEASE_DB_PER_SEC = 10.0f;
```

The level jumps up instantly but falls at this speed.

| Symptom | Change |
|---|---|
| Lights drop to white between every beat | Lower it (5.0) |
| Lights stay stuck in rainbow after the music drops | Raise it (20.0 or 30.0) |

---

## 9. How Fast Colors Change

```cpp
const float ZONE_MIN_HOLD_MS = 200.0f;
```

Minimum time between zone changes.

| Value | Feel |
|---|---|
| 100 | Twitchy, very reactive |
| 200 | Default (about 5 changes per second max) |
| 400 to 600 | Calm |

---

## 10. Noise Floor Behavior

```cpp
const float FLOOR_FALL_DB_PER_SEC = 1.0f;
```

How fast the floor follows a **quieter** room down. The floor rises very slowly on purpose, so loud music does not become the new baseline.

| Symptom | Change |
|---|---|
| Lights stop reacting after a long loud stretch | Normal drift upward. Reboot in a quieter moment, or lower the zones a bit |
| Floor stays too high after the room gets quiet | Raise it (2.0 to 3.0) |

If the venue is loud from the moment the board powers on, calibration measures that loudness as the "floor." Power up during the quietest normal moment.

---

## 11. Ready-Made Presets

Replace the matching lines in `CONFIGURATION`. Start from the default and change one preset at a time.

### Quiet room / low volume

```cpp
const float SENSITIVITY = 1.5f;
const float ZONE_LO[5] = { -1000, 29, 31, 33, 35 };
const float ZONE_HI[5] = {    29, 31, 33, 35, 1000 };
const float STROBE_DB  = 36.0f;
```

### Loud club / party

```cpp
const float SENSITIVITY = 0.8f;
const float ZONE_LO[5] = { -1000, 30, 33, 36, 39 };
const float ZONE_HI[5] = {    30, 33, 36, 39, 1000 };
const float STROBE_DB  = 41.0f;
const float HYST       = 1.5f;
```

### Wide spread (more dynamic range, slower build)

```cpp
const float ZONE_LO[5] = { -1000, 30, 34, 38, 42 };
const float ZONE_HI[5] = {    30, 34, 38, 42, 1000 };
const float STROBE_DB  = 45.0f;
```

### Rainbow most of the time, strobe only on huge peaks

```cpp
const float ZONE_LO[5] = { -1000, 28, 29, 30, 31 };
const float ZONE_HI[5] = {    28, 29, 30, 31, 1000 };
const float STROBE_DB  = 42.0f;
```

(Keep `HYST` at 0.5 or less here, since these zones are only 1 dB wide.)

---

## 12. Symptom Lookup

| What you see | Change |
|---|---|
| Always white | Lower zone thresholds or raise `SENSITIVITY` |
| Always rainbow and strobing | Raise thresholds or lower `SENSITIVITY` |
| Never reaches rainbow | Lower `ZONE_LO[4]` or raise `SENSITIVITY` |
| Rainbow too early | Raise `ZONE_LO[4]` |
| Strobe too often | Raise `STROBE_DB` (or `1000.0f` to disable) |
| Never strobes | Lower `STROBE_DB` toward `ZONE_LO[4]` |
| Flicker at a color border | Raise `HYST` |
| Colors change too fast | Raise `ZONE_MIN_HOLD_MS` |
| Colors change too slowly | Lower `ZONE_MIN_HOLD_MS` |
| Drops to white too quickly | Lower `RELEASE_DB_PER_SEC` |
| Sticks on loud color too long | Raise `RELEASE_DB_PER_SEC` |

---

## 13. Safe Tuning Loop

```
watch Serial Monitor  ->  note quiet / normal / peak  ->  edit ONE value
->  upload  ->  re-check dB and zoneReq  ->  keep or undo
```

Strobe reminder: flashing between about 3 and 30 Hz can trigger seizures in photosensitive people. If the lights are used in public, keep a way to turn the strobe off (`STROBE_DB = 1000.0f`).
