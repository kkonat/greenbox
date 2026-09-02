# greenbox

Three bare ESP-IDF projects for one piece of hardware: the LILYGO TTGO
T-Display (ESP32-D0WDQ6-V3, 4 MB flash, 520 KB SRAM, no PSRAM, 135x240 ST7789,
two buttons). No Arduino, no PlatformIO, no display library — the panel driver
is hand-written datasheet opcodes over `spi_master`, and the buttons are read
off the GPIO input registers.

The centrepiece is **[greenbox OS](greenbox-os/README.md)**: a small program
launcher that owns the panel, the buttons, the clock and storage, and runs
*guests* — a few kilobytes each of relocatable machine code that reach the
hardware only through a syscall table. Guests are built with the bare
cross-compiler rather than as IDF components, packed into a SPIFFS partition as
`.gbx` images, and loaded into an IRAM heap at runtime. That is the whole idea:
adding a program does not mean reflashing an OS.

| tree | what it is |
|---|---|
| **[greenbox-os/](greenbox-os/README.md)** | the OS, the guest SDK, seven guest programs, and the `.gbx` toolchain |
| **[tdisplay/](tdisplay/README.md)** | the bare ESP-IDF starter this grew out of — the verified pin map and the ST7789 traps |
| **[display_probe_idf/](display_probe_idf/main/main.c)** | how the board was identified in the first place: sweep candidate pin/controller/geometry combinations until something appears, then press a button to confirm |

## What to try

Flash it (below), and the launcher comes up. Two buttons drive everything:

| gesture | in the launcher | in a guest |
|---|---|---|
| L tap | previous entry | passed through |
| R tap | next entry | passed through |
| **R hold 1 s** | run the selection | passed through |
| **L hold 3 s** | toggle the info screen | **quit — the OS kills the guest** |

Long presses fire the moment the hold is earned, not on release, and the
three-second quit is the one gesture a guest can never swallow. The details,
including the quit bar that draws over the guest's own scanlines, are in
[README - Buttons](greenbox-os/README.md#buttons) and
[ARCHITECTURE - Gestures](greenbox-os/ARCHITECTURE.md#4-gestures).

The seven guests, roughly in the order worth trying them:

| guest | what it is |
|---|---|
| **[astro](greenbox-os/README.md#astro)** | a vertical scroller over a parallax background of drifting nebulae and gas-giant rings; asteroids, shields, homing missiles. Hold L/R to thrust. Forces portrait, and is the reason orientation is a *request* |
| **[pinball](greenbox-os/README.md#pinball)** | a table built from line segments, with grippy flipper rubber that makes the shot, and Gumball and Darwin on the backglass |
| **[pacman](greenbox-os/README.md#pacman)** | the maze on two buttons, where a press is an intention rather than a turn, under a camera that follows |
| **[mandel](greenbox-os/README.md#mandel)** | three fractals in fixed point, rendered in three passes at a byte per pixel, with a palette you can cycle and views that survive a reboot |
| **[wherouter](greenbox-os/README.md#wherouter)** | a Wi-Fi survey, then hot-and-cold for one BSSID — a difference between two averages, not trilateration, and the metres are a guess |
| **[clock](greenbox-os/guests/clock/clock.c)** | the first program. No 32.768 kHz crystal on this board, so the RTC drifts minutes a day and setting it is routine: R-hold enters setting mode, L/R move the flashing field |
| **[settings](greenbox-os/guests/settings/settings.c)** | orientation and one of five colour themes, for the whole OS. The launcher paints itself entirely out of the theme's seven roles |

There is also a UART0 console at 115200 for the things two buttons cannot do —
`ls`, `run <name>`, `kill`, `settime`, `conf rot`, `conf theme`, and injecting
button events. See
[README - Serial console](greenbox-os/README.md#serial-console).

Two of the guests ([pinball](greenbox-os/guests/pinball/sim/harness.c) and
[pacman](greenbox-os/guests/pacman/sim/harness.c)) also build for the host, so
the physics and the camera can be worked on without a board attached.

## Build and flash

Guests first — the OS build packs `guests/out/` into its SPIFFS image.

```powershell
cd greenbox-os\guests ; .\build.ps1        # bare toolchain, no IDF env needed
. C:\esp\esp-idf\export.ps1
cd ..\os ; idf.py build ; idf.py -p COM5 flash monitor
```

That is the whole of it if the machine is already set up. If it is not, or if
something comes out wrong:

- **[TOOLCHAIN.md](greenbox-os/TOOLCHAIN.md)** — the
  [hard requirements](greenbox-os/TOOLCHAIN.md#hard-requirements), the
  [`sdkconfig` settings the OS depends on](greenbox-os/TOOLCHAIN.md#required-sdkconfig-settings),
  the [partition table](greenbox-os/TOOLCHAIN.md#partition-table), and the
  [acceptance checks](greenbox-os/TOOLCHAIN.md#acceptance-checks) that say a
  build is right.
- **[ARCHITECTURE.md - Build pipeline](greenbox-os/ARCHITECTURE.md#8-build-pipeline)**
  — why there are two build systems, and the three compiler flags
  (`-mlongcalls`, `-mtext-section-literals`, `--no-relax`) the relocatable-guest
  scheme rests on.
- **[tdisplay/README.md](tdisplay/README.md#build--flash)** — the same board
  without the OS, if you want to check the hardware on its own first.

One cable caveat that costs an afternoon if you meet it cold: this CP2102
intermittently drops a byte on bulk esptool **reads**, at every baud rate.
Writes and flashing are fine. Read flash back in 256 KB chunks with retry —
[the write-up is here](greenbox-os/TOOLCHAIN.md#serial-quirk-on-this-board).

## Where to read next

[greenbox-os/README.md](greenbox-os/README.md) is the way in — what the system
is, and a walk through each program and the problem it turned out to pose.
[ARCHITECTURE.md](greenbox-os/ARCHITECTURE.md) is the machinery: task layout
and the one event queue, how a `.gbx`
[becomes running code](greenbox-os/ARCHITECTURE.md#5-loading-a-guest), the
[syscall boundary](greenbox-os/ARCHITECTURE.md#6-the-syscall-boundary),
[ABI versioning](greenbox-os/ARCHITECTURE.md#7-abi-versioning), and
[what happens when it goes wrong](greenbox-os/ARCHITECTURE.md#11-what-happens-when-it-goes-wrong).
