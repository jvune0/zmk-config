# LED order (Corne Choc Pro)

Each half has 23 WS2812 LEDs, one under each key. They are wired as a single chain, and an
LED's number is its position in the chain (zero-based). This number is used in the indicator
settings, e.g. `caps-lock-led` in `config/corne_choc_pro_left.overlay`.

The order was determined with the `&led_scan` command (see `src/led_scan.c`).

## Left half

The layout matches the grid in `config/corne_choc_pro.keymap`: 6 main columns, a seventh
inner column of two keys, and three thumb keys at the bottom.

| Row       | Col 0 | Col 1 | Col 2 | Col 3 | Col 4 | Col 5 | Col 6 (inner) |
|-----------|:-----:|:-----:|:-----:|:-----:|:-----:|:-----:|:-------------:|
| Top       | 18    | 17    | 12    | 11    | 4     | 3     | 21            |
| Middle    | 19    | 16    | 13    | 10    | 5     | 2     | 22            |
| Bottom    | 20    | 15    | 14    | 9     | 6     | 1     |               |
| Thumb     |       |       |       | 8     | 7     | 0     |               |

## Right half

The same chain, mirrored: LED 0 is the inner thumb key, 18–20 are the outer column.

| Row       | Col 7 (inner) | Col 8 | Col 9 | Col 10 | Col 11 | Col 12 | Col 13 |
|-----------|:-------------:|:-----:|:-----:|:------:|:------:|:------:|:------:|
| Top       | 21            | 3     | 4     | 11     | 12     | 17     | 18     |
| Middle    | 22            | 2     | 5     | 10     | 13     | 16     | 19     |
| Bottom    |               | 1     | 6     | 9      | 14     | 15     | 20     |
| Thumb     |               | 0     | 7     | 8      |        |        |        |
