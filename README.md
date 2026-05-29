# Cardputer ADV BASIC Interpreter

A powerful, self-contained BASIC interpreter for the **M5Stack Cardputer ADV** (ESP32-S3).

It runs entirely on the device and turns the Cardputer into a portable retro computing platform with graphics, sound, GPIO control, sprites, and persistent storage.

## Features

- **Classic BASIC** with line numbers, `PRINT`, `INPUT`, `IF`/`THEN`, `GOTO`, `GOSUB`/`RETURN`, `END`, `RUN`, `LIST`, `NEW`, `CONT`
- **FOR/NEXT** loops with `STEP` and nesting support
- **DATA / READ / RESTORE** for storing constants, level data, or sprite bitmaps
- **User-definable sprites** via `DEFSPRITE` + `DATA` (monochrome bitmaps)
- **Built-in sprites** (ball, spaceship, alien) + custom sprites
- **Graphics commands**: `CLS`, `PLOT`, `LINE`, `CIRCLE`, `RECT`, `FILLRECT`, `LOCATE`, `COLOR`, `SPRITE`
- **Sound**: `BEEP` / `TONE` + `PLAY "C D E F G A B C5"` music command
- **GPIO access**: `PINMODE`, `DWRITE`, `PWM`, `DIGITALREAD()`, `ANALOGREAD()`, `INKEY()`
- **Extended math**: `PI`, `SQRT`, `POW` / `^`, `SIN`, `COS`, `TAN`, `ATAN`, `EXP`, `LOG`, `FLOOR`, `CEIL`, `ROUND`, `ABS`, `RND`
- **SD card support**: All `.bas` files are automatically saved into and loaded from the `/BASIC` folder on the SD card. Use `DIR`, `FILES`, or `LS` to list programs.
- Full keyboard REPL with backspace and editing on the Cardputer’s 56-key keyboard
- Small 240×135 display output mixing text and graphics

## Getting Started

### Requirements
- M5Stack Cardputer ADV
- Arduino IDE with M5Stack board support installed
- Libraries:
  - `M5Unified`
  - `M5Cardputer`
  - `SD` (built-in)

### Installation
1. Clone or download this repository.
2. Open `Cardputer_ADV_BASIC_v1.1.ino` in Arduino IDE.
3. Select board **M5Cardputer**.
4. Compile and upload.
5. Insert a formatted microSD card (optional but recommended for `LOAD`/`SAVE`).

On boot you will see the `>` prompt. Type commands directly or enter numbered lines to build programs.

## Example Programs

### Hello World + Loop
```basic
10 CLS
20 FOR I = 1 TO 10
30 PRINT "Hello Cardputer! "; I
40 NEXT I
50 END
```

### Graphics + Sprite
```basic
10 CLS
20 SPRITE 0, 100, 50          ' built-in ball
30 SPRITE 1, 140, 60          ' built-in ship
40 PAUSE 2000
50 CLS
60 END
```

### Custom Sprite from DATA
```basic
1000 DATA 8,8, 60,126,255,255,255,255,126,60   ' 8x8 ball bitmap
1010 RESTORE 1000
1020 DEFSPRITE 10
1030 CLS
1040 SPRITE 10, 80, 40, 0x07FF   ' cyan custom sprite
1050 PAUSE 3000
1060 END
```

### Music
```basic
10 PLAY "C D E F G A B C5"
20 PAUSE 500
30 PLAY "C5 B A G F E D C"
```

### GPIO Blink (connect LED to safe header pin, e.g. GPIO 5)
```basic
10 PINMODE 5,1
20 DWRITE 5,1
30 PAUSE 300
40 DWRITE 5,0
50 PAUSE 300
60 GOTO 20
```

## Command Reference (Selected)

| Category     | Commands / Functions                              |
|--------------|---------------------------------------------------|
| Program      | `RUN`, `LIST`, `NEW`, `CONT`, `SAVE "name"`, `LOAD "name"`, `DELETE "name"`, `RENAME`, `CAT`, `DIR` / `FILES` / `LS` |
| Control      | `IF` cond `THEN` line, `GOTO`, `GOSUB`, `RETURN`, `END`, `FOR`/`NEXT` |
| Data         | `DATA` values, `READ` var1,var2, `RESTORE`        |
| Graphics     | `CLS`, `PLOT` x,y, `LINE` x1 y1 x2 y2, `CIRCLE`, `RECT`, `FILLRECT`, `LOCATE`, `COLOR`, `SPRITE` id,x,y |
| Sprites      | `DEFSPRITE` id (uses current DATA), `SPRITE` id,x,y,color |
| Sound        | `BEEP` freq,dur, `PLAY` "notes"                   |
| GPIO         | `PINMODE` pin,mode, `DWRITE` pin,val, `PWM` pin,duty, `DIGITALREAD(pin)`, `ANALOGREAD(pin)` |
| Math         | `ABS()`, `SIN()`, `COS()`, `TAN()`, `ATAN()`, `SQRT()`, `POW()`, `EXP()`, `LOG()`, `FLOOR()`, `CEIL()`, `ROUND()`, `RND()`, `PI`, `INKEY()` |
| Misc         | `PAUSE` ms, `INPUT` ["prompt";] var               |

## Notes

- The display is small (240×135). Use `CLS` frequently for clean output.
- `DATA` values are collected automatically when you type `RUN`.
- User sprites are defined with `DEFSPRITE` after positioning the data pointer with `RESTORE`.
- **File I/O**: `OPEN "file" FOR INPUT/OUTPUT AS #n`, `CLOSE #n`, `PRINT #n, expr`, `INPUT #n, var`, `EOF(n)` are supported for runtime file access from BASIC programs.
- GPIO pins on the exposed header are safe to use. Avoid internal pins used by display, audio, keyboard, and IMU.
- Programs are stored in RAM. Use `SAVE`/`LOAD` with a microSD card for persistence.
- This is an educational / hobby interpreter — not all edge cases of full BASIC are implemented.

## License

MIT License — feel free to use, modify, and share.

Enjoy turning your Cardputer ADV into a portable BASIC machine!
