# TTGO T-Display — bare ESP-IDF starter

Working environment for the LILYGO TTGO T-Display on COM5. No Arduino, no
PlatformIO, no display library — the ST7789 driver in `main/st7789.c` is
hand-written datasheet opcodes over `spi_master`.

This is the starter, not the destination: [greenbox OS](../greenbox-os/README.md)
is what got built on top of the board facts below — a launcher that runs
relocatable programs through a syscall table, with its own
[architecture notes](../greenbox-os/ARCHITECTURE.md) and
[build requirements](../greenbox-os/TOOLCHAIN.md).

## Build & flash

`idf.py` is **not** in this directory. It lives at `C:\esp\esp-idf\tools\idf.py`
and `export.ps1` defines it as a PowerShell function scoped to one shell. Use the
wrapper instead — it sets the environment up every run:

```powershell
.\go.ps1 build
.\go.ps1 -p COM5 -b 115200 flash
.\go.ps1 -p COM5 monitor        # Ctrl+] quits
.\go.ps1 size-components
```

Doing it by hand needs all three lines in the **same** terminal:

```powershell
. C:\esp\esp-idf\export.ps1
cd C:\Users\mieczu\greenbox\tdisplay
idf.py -p COM5 -b 115200 flash monitor
```

Keep `-b 115200`. This board's CP2102 drops bytes at higher rates — see below.

## Board facts (all verified on the hardware, not copied from a datasheet)

| | |
|---|---|
| MCU | ESP32-D0WDQ6-V3 rev 3.0, 240MHz dual core |
| Flash | 4MB (Boya `0x68`/`4016`) |
| MAC | `3c:61:05:0c:96:5c` |
| USB | Silicon Labs CP2102, `VID_10C4&PID_EA60`, serial `021631B2` |
| Panel | ST7789, IPS, 135×240 native portrait |
| SPI | SCLK 18, MOSI 19, DC 16, CS 5, RST 23 |
| Backlight | GPIO4, active HIGH (no user LED on this board) |
| Buttons | **LEFT = GPIO0**, **RIGHT = GPIO35**, both active LOW |

### SPI trap: `SPI_DEVICE_NO_DUMMY` is mandatory here

This board wires **MOSI to GPIO19**, but VSPI's native IOMUX MOSI is **GPIO23**.
So the bus is routed through the GPIO matrix, and above **26.7MHz** the driver
rejects the device outright rather than allow unreliable *reads*:

```
E spi_hal: When work in full-duplex mode at frequency > 26.7MHz, device cannot read correct data.
E spi_master: spi_bus_add_device(427): assigned clock speed not supported
```

`spi_bus_add_device` then returns `ESP_ERR_NOT_SUPPORTED`, the handle stays NULL,
and every later transfer fails with the far less helpful:

```
E spi_master: check_trans_valid(789): invalid dev handle
```

The panel is write-only (`miso_io_num = -1`), so the read concern doesn't apply.
Setting `.flags = SPI_DEVICE_NO_DUMMY` keeps the full 40MHz. The alternatives are
dropping to 26.67MHz or rewiring to GPIO23.

**Always check `spi_bus_add_device`'s return value** — otherwise the real cause
scrolls past and you only see "invalid dev handle" forever.

### Other traps worth remembering:

- **GPIO35 is input-only and has no internal pull-up.** Nothing on GPIO34–39 does.
  It works purely because the board has an external one. `INPUT_PULLUP` on it is
  a silent no-op.
- **GPIO0 is the boot strap pin.** Held low at reset, the chip enters the serial
  bootloader instead of your app. That's how flashing works — but it also means
  holding the left button while resetting looks like a hang.

### CGRAM offsets

The glass is 135×240 but the controller has 240×320 of RAM, so the visible
window sits at an offset that changes per rotation. `st7789_set_rotation()`
carries the table:

| rot | MADCTL | col off | row off | size |
|-----|--------|---------|---------|------|
| 0 | 0x00 | 52 | 40 | 135×240 |
| 1 | 0x60 | 40 | 52 | 240×135 |
| 2 | 0xC0 | 53 | 40 | 135×240 |
| 3 | 0xA0 | 40 | 53 | 240×135 |

Wrong offsets show up as the image shifted with a wrapped strip down one edge.

Being IPS, the panel needs `INVON` (0x21). Skip it and everything renders as a
colour negative.

## The demo

- LEFT decrements, RIGHT increments
- hold both ~1s to cycle rotation
- bottom strip mirrors live button state

## API

```c
st7789_init(0);                  // 0 = native portrait
st7789_fill(C_BLACK);
st7789_fill_rect(x, y, w, h, C_RED);
st7789_rect(x, y, w, h, C_YELLOW);
st7789_text(x, y, "hello", C_WHITE, C_BLACK, 2);   // built-in 5x7, scaled
st7789_blit(x, y, w, h, pixels);                   // RGB565 buffer
st7789_set_rotation(1);
st7789_backlight(false);
```

## Serial quirk

This CP2102 intermittently loses a byte on bulk esptool **reads** —
`Corrupt data, expected 0x1000 bytes but received 0xfff bytes` — at 921600 and
460800, and even at 115200 on one long 4MB read. Writes are fine. To dump flash,
read in 256KB chunks with retry and concatenate.
