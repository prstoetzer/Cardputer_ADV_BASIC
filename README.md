# Cardputer ADV BASIC

A classic line-numbered BASIC interpreter for the **M5Stack Cardputer ADV** (ESP32-S3). Write, edit, and run BASIC programs directly on the device using the built-in keyboard and screen — no PC required. Save and load programs from a microSD card, draw graphics, play sounds, read the GPIO, and work with strings.

It's a single Arduino sketch with no external dependencies beyond the standard M5Stack libraries.

> **Heads up:** This targets the Cardputer **ADV** (ESP32-S3FN8 / Stamp-S3A). It will very likely run on the original Cardputer too, since the boards are nearly identical, but the SD pin mapping is verified against the original Cardputer docs — double-check it on your unit if SD init fails.

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [REPL commands](#repl-commands)
- [Language reference](#language-reference)
  - [Variables](#variables)
  - [Operators](#operators)
  - [Program flow statements](#program-flow-statements)
  - [I/O and console statements](#io-and-console-statements)
  - [Graphics statements](#graphics-statements)
  - [Sound statements](#sound-statements)
  - [GPIO statements](#gpio-statements)
  - [Timing statements](#timing-statements)
  - [Data statements](#data-statements)
  - [File statements](#file-statements)
- [Functions](#functions)
  - [Math functions](#math-functions)
  - [String functions](#string-functions)
  - [Input and hardware functions](#input-and-hardware-functions)
- [SD card storage](#sd-card-storage)
- [Examples](#examples)
- [Limits](#limits)
- [Limitations](#limitations)
- [Contributing](#contributing)
- [License](#license)

## Requirements

- M5Stack Cardputer ADV (or original Cardputer)
- [Arduino IDE](https://www.arduino.cc/en/software) with the **ESP32 board package (3.x)**
- Libraries (install via the Library Manager):
  - `M5Unified`
  - `M5Cardputer`
- A microSD card formatted **FAT32** (recommended; the interpreter runs without one, but you can't save or load programs). Cards of **32 GB or smaller** are the most reliable.

## Installation

1. Install the ESP32 board package and the M5Unified / M5Cardputer libraries in the Arduino IDE.
2. Open `Cardputer_ADV_BASIC_v1_2.ino`.
3. Select the correct board (e.g. **M5Cardputer** or a generic **ESP32S3 Dev Module**) and the right serial port.
4. Click **Verify**, then **Upload**.

On boot you'll see the banner and a `>` prompt. You're ready to type BASIC.

## Quick start

Type a program line by line at the prompt, then `RUN`:

```basic
10 PRINT "WHAT IS YOUR NAME?"
20 INPUT N$
30 PRINT "HELLO, " + N$
40 FOR I = 1 TO 3
50 PRINT "COUNT "; I
60 NEXT I
70 END
```

```
> RUN
```

## How it works

- Lines that **start with a number** are stored in the program (in line-number order). Entering an existing line number replaces that line; entering a line number with nothing after it deletes that line.
- Lines **without** a number run immediately. This is how you issue commands like `RUN`, `LIST`, and `SAVE`, and you can also use it as a calculator (`PRINT 2 + 2`).
- Keywords and variable names are **case-insensitive** (`print`, `PRINT`, and `Print` are the same). Text inside double quotes keeps its case.
- One statement per line — there is no `:` multi-statement separator.
- Press **Ctrl+C** (or **Enter**, **Fn**, or **Esc**) to break a running program. `CONT` resumes it.

## REPL commands

Typed without a line number:

| Command | Description |
|---|---|
| `RUN` | Run the current program from the start |
| `LIST` | List the program to the screen |
| `NEW` | Erase the program and all variables |
| `CONT` | Resume a program after a break or `STOP` |
| `SAVE "name"` | Save the program to `/BASIC/name.bas` on SD |
| `LOAD "name"` | Load a program from `/BASIC/` |
| `DIR` / `FILES` / `LS` | List files in `/BASIC/` and show free space |
| `DELETE "name"` | Delete a program file |
| `RENAME "old","new"` | Rename a file |
| `CAT "name"` | Print a file's contents to the screen |

## Language reference

### Variables

There are two fixed sets of variables; no declaration is needed.

- **Numeric**: single letters `A`–`Z`, each a floating-point number. `LET` is optional, so `A = 5` and `LET A = 5` are identical.
- **String**: `A$`–`Z$`, each holding text. Example: `LET A$ = "HI"`.

Numbers print in a clean BASIC style: integers show without a decimal point (`5`, not `5.000000`) and fractions drop trailing zeros.

### Operators

In order of precedence (highest first):

| Operator | Meaning |
|---|---|
| `^` | Exponentiation (`2 ^ 8` = 256) |
| `*` `/` | Multiply, divide |
| `+` `-` | Add, subtract; `+` also concatenates strings |
| `=` `<>` `<` `>` `<=` `>=` | Comparisons (return 1 for true, 0 for false) |
| `NOT` | Logical NOT |
| `AND` `OR` | Logical AND / OR |

A leading minus negates a value or variable (`-A`, `-(X+1)`). Comparisons and logic also work on strings (`A$ = "YES"`, `A$ < B$`), compared lexicographically.

### Program flow statements

| Statement | Example | Notes |
|---|---|---|
| `REM` | `REM this is a comment` | Rest of the line is ignored |
| `LET` | `LET X = 5` | Optional; assignment also works without it |
| `IF ... THEN` | `IF X > 0 AND Y > 0 THEN 100` | THEN may be a line number (jump) or a statement to run |
| `GOTO` | `GOTO 50` | Jump to a line number |
| `GOSUB` | `GOSUB 200` | Call a subroutine |
| `RETURN` | `RETURN` | Return from `GOSUB` to the line after the call |
| `FOR ... TO ... STEP` | `FOR I = 1 TO 10 STEP 2` | `STEP` is optional (defaults to 1); step cannot be 0 |
| `NEXT` | `NEXT I` / `NEXT` / `NEXT J,I` | Loops can be nested; `NEXT J,I` closes several at once |
| `END` | `END` | Stop the program |
| `STOP` | `STOP` | Stop the program (resume with `CONT`) |

### I/O and console statements

| Statement | Example | Notes |
|---|---|---|
| `PRINT` (or `?`) | `PRINT "X="; X, A$` | `;` joins items with no gap and suppresses the trailing newline; `,` inserts a column gap. Mixes strings and numbers freely. |
| `INPUT` | `INPUT N` / `INPUT "Name"; N$` | Reads a line from the keyboard. A `$` variable stores text; a numeric variable parses a number. An optional quoted prompt may precede the variable, separated by `;`. |
| `CLS` | `CLS` | Clear the screen |
| `LOCATE` | `LOCATE 10, 20` | Move the text cursor to pixel x, y |
| `COLOR` | `COLOR 65535` | Set the **text** color (16-bit RGB565). Graphics commands are not affected by this. |

### Graphics statements

The screen is **240 × 135** pixels. All graphics shapes draw in **white**; `COLOR` changes text color only (the exception is `SPRITE`, which takes its own color argument).

| Statement | Example | Arguments |
|---|---|---|
| `PLOT` | `PLOT 50, 30` | x, y — single pixel |
| `LINE` | `LINE 0,0,100,60` | x1, y1, x2, y2 |
| `CIRCLE` | `CIRCLE 60, 40, 20` | x, y, radius (outline) |
| `RECT` / `BOX` | `RECT 10,10,40,20` | x, y, width, height (outline) |
| `FILLRECT` / `FILL` | `FILLRECT 10,10,40,20` | x, y, width, height (filled) |
| `SPRITE` | `SPRITE 0, 50, 40, 2016` | id, x, y, [color] — color is optional (default white) |
| `DEFSPRITE` | `DEFSPRITE 5` | Define sprite `id` from the next `DATA` (see [User-defined sprites](#user-defined-sprites)) |

Built-in sprite IDs `0` (ball, 8×8), `1` (ship, 16×8), and `2` (alien, 8×8) are available without defining anything.

### Sound statements

| Statement | Example | Notes |
|---|---|---|
| `BEEP` / `TONE` | `BEEP 440, 200` | Frequency in Hz, duration in ms |
| `PLAY` | `PLAY "C D E F G A B"` | Plays a note string. Letters `A`–`G`; append `#` for sharp (`C#`) and a digit `0`–`9` for octave (`C4`); spaces and commas are short rests. |

### GPIO statements

| Statement | Example | Notes |
|---|---|---|
| `PINMODE` | `PINMODE 2, 1` | mode: `0` = INPUT, `1` = OUTPUT, `2` = INPUT_PULLUP, `3` = INPUT_PULLDOWN |
| `DWRITE` / `DIGITALWRITE` | `DWRITE 2, 1` | Drive a pin high (non-zero) or low (0) |
| `PWM` | `PWM 2, 128` | 8-bit duty cycle (0–255) at 5 kHz on the given pin |

### Timing statements

| Statement | Example | Notes |
|---|---|---|
| `PAUSE` / `DELAY` / `SLEEP` | `PAUSE 500` | Wait the given number of milliseconds (interruptible with the break keys) |

### Data statements

| Statement | Example | Notes |
|---|---|---|
| `DATA` | `DATA 10, 20, 30` | Inline constants, read in line order |
| `READ` | `READ A, B, C` | Read the next value(s) from the data pool into variables |
| `RESTORE` | `RESTORE` | Rewind the data pointer to the first `DATA` value |

### File statements

These work on files inside the `/BASIC/` folder (see [SD card storage](#sd-card-storage)).

| Statement | Example | Notes |
|---|---|---|
| `OPEN` | `OPEN "scores.txt" FOR OUTPUT AS #1` | Modes: `FOR INPUT`, `FOR OUTPUT` (truncates), `FOR APPEND`. File numbers `#1`–`#3`. |
| `CLOSE` | `CLOSE #1` / `CLOSE` | Close one file, or all open files if no number is given |
| `PRINT #n,` | `PRINT #1, A$, X` | Write to an open file (strings and numbers, one record per `PRINT`) |
| `INPUT #n,` | `INPUT #2, A$` | Read one line from an open file into a variable |

## Functions

Functions are used inside expressions, e.g. `PRINT SQRT(2)` or `Y = SIN(A) * R`.

### Math functions

| Function | Description |
|---|---|
| `PI` | The constant π (no parentheses) |
| `RND` or `RND()` | Random number in the range 0–1 |
| `ABS(x)` | Absolute value |
| `INT(x)` | Floor toward negative infinity |
| `FLOOR(x)` | Round down |
| `CEIL(x)` | Round up |
| `ROUND(x)` | Round to nearest integer |
| `SQRT(x)` / `SQR(x)` | Square root |
| `POW(b, e)` | b raised to the e power (same as `b ^ e`) |
| `EXP(x)` | e raised to x |
| `LOG(x)` | Natural logarithm |
| `SIN(x)` `COS(x)` `TAN(x)` | Trig functions; the argument is in **degrees** |
| `ATAN(x)` | Arctangent, returned in **degrees** |

### String functions

| Function | Description |
|---|---|
| `LEFT$(s, n)` | Leftmost n characters |
| `RIGHT$(s, n)` | Rightmost n characters |
| `MID$(s, start[, len])` | Substring from `start` (1-based); to end if `len` omitted |
| `CHR$(n)` | Character for ASCII code n |
| `STR$(n)` | Number converted to a string |
| `LEN(s)` | Length of a string (returns a number) |
| `ASC(s)` | ASCII code of the first character (returns a number) |
| `VAL(s)` | Numeric value parsed from a string (returns a number) |

Strings concatenate with `+`, for example `A$ = "X=" + STR$(N)`.

### Input and hardware functions

| Function | Description |
|---|---|
| `INKEY` or `INKEY()` | Next key code from the buffer, or 0 if none (non-blocking) |
| `INKEY$` | Next key as a one-character string, or `""` if none |
| `DIGITALREAD(pin)` / `DREAD(pin)` | Read a digital pin (sets it to INPUT) |
| `ANALOGREAD(pin)` / `AREAD(pin)` | Read an analog pin |
| `EOF(n)` | Returns 1 at end of open file `#n`, otherwise 0 |

## SD card storage

All file access is **sandboxed inside a `/BASIC/` folder** on the card, which the interpreter creates automatically on startup if it isn't already there.

- Programs saved with `SAVE` get a `.bas` extension and live in `/BASIC/`. Data files created with `OPEN` keep whatever name and extension you give them, also inside `/BASIC/`.
- File names are confined to the sandbox: any leading path, `..`, or subdirectory in a name is stripped, so only the final name is used (e.g. `LOAD "/etc/passwd"` resolves to `/BASIC/passwd`). There is no way to read or write outside `/BASIC/`.
- `DIR` lists everything in `/BASIC/` and reports free and total space.

If you have files from an older version sitting in the card's root, move them into `/BASIC/` so the interpreter can see them.

## Examples

**Bouncing dot**

```basic
10 X = 10
20 Y = 10
30 DX = 2
40 DY = 1
50 CLS
60 PLOT X, Y
70 X = X + DX
80 Y = Y + DY
90 IF X > 230 OR X < 1 THEN DX = -DX
100 IF Y > 130 OR Y < 1 THEN DY = -DY
110 PAUSE 10
120 GOTO 50
```

**String handling**

```basic
10 INPUT "Enter a word"; W$
20 PRINT "Length: "; LEN(W$)
30 PRINT "Upper first letter: "; LEFT$(W$, 1)
40 PRINT "Reversed first 3: "; MID$(W$, 1, 3)
```

**DATA / READ with a user sprite**

```basic
10 DEFSPRITE 5
20 SPRITE 5, 100, 50, 2016
30 DATA 8, 8
40 DATA 24, 60, 126, 255, 255, 126, 60, 24
```

**Read a key without blocking**

```basic
10 K = INKEY()
20 IF K > 0 THEN PRINT "KEY CODE: "; K
30 GOTO 10
```

### User-defined sprites

A sprite is 1 bit per pixel, MSB first, row by row, supplied through `DATA`: first the **width**, then the **height**, then the bitmap bytes. `DEFSPRITE id` reads the next sprite definition starting from the current data pointer, then `SPRITE id, x, y[, color]` draws it.

## Limits

| Resource | Limit |
|---|---|
| Program lines | 300 |
| `GOSUB` nesting | 10 |
| `FOR` nesting | 8 |
| Open files | 3 (`#1`–`#3`) |
| Screen | 240 × 135 pixels |

## Limitations

These are deliberate, to keep the interpreter small and fast on the device:

- Variables are **single-letter only** (`A`–`Z` numeric, `A$`–`Z$` string). No arrays, no long names.
- **No multi-statement lines** — one statement per line number (no `:` separator).
- `RESTORE` rewinds to the beginning only; it doesn't take a line number.
- `READ` and `DEFSPRITE` share one data pointer (classic behavior), so mixing them consumes from the same sequence.
- Graphics primitives draw in white; only text color is adjustable via `COLOR`.

## Contributing

Issues and pull requests are welcome. If you hit a bug, an example program that reproduces it is the most helpful thing you can include.

## License

See the repository's `LICENSE` file. The M5Unified and M5Cardputer libraries are licensed separately by M5Stack.
