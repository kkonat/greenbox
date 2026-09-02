# greenbox OS

A small program launcher for the LILYGO TTGO T-Display (ESP32-D0WDQ6-V3, 4 MB
flash, 520 KB SRAM, no PSRAM). The OS owns the panel, the buttons, the clock
and storage; programs are a few kilobytes of relocatable machine code that
reach the hardware only through a syscall table.

```
greenbox-os/
  abi/greenbox_abi.h     the contract, compiled into both sides
  os/                    ESP-IDF project - the OS itself
    main/osgfx.c         the rasteriser guests draw with, behind api->gfx
  guests/                programs, built with the bare cross-compiler
    gsdk/                linker script, the tiny C runtime, the gfx surface
    clock/               the first program
    settings/            orientation and colour theme, for the whole OS
    astro/               a game, and the reason orientation is a request
    mandel/              three fractals, and the search for where to point them
    pinball/             a table, with Gumball and Darwin on the backglass
      sim/               the same guest, built for the host, for the physics
    wherouter/           a survey, and a hot-and-cold finder for one BSSID
    pacman/              the board, two buttons, and a camera that follows
      sim/               the same guest on the host, for the camera and the maze
    out/                 .gbx images, packed into the SPIFFS partition
  tools/mkguest.py       ELF -> .gbx converter
  TOOLCHAIN.md           what the build needs
  ARCHITECTURE.md        how the pieces fit, with diagrams
```

## The rest of the documentation

- **[ARCHITECTURE.md](ARCHITECTURE.md)** - the machinery. Task layout and the
  one [event queue](ARCHITECTURE.md#3-tasks-and-the-one-event-queue), how a
  `.gbx` [becomes running code](ARCHITECTURE.md#5-loading-a-guest), the
  [syscall boundary](ARCHITECTURE.md#6-the-syscall-boundary) and
  [ABI versioning](ARCHITECTURE.md#7-abi-versioning), the
  [measured footprint](ARCHITECTURE.md#10-measured-footprint), and
  [what happens when it goes wrong](ARCHITECTURE.md#11-what-happens-when-it-goes-wrong).
  Read it before changing the OS.
- **[TOOLCHAIN.md](TOOLCHAIN.md)** - what the build needs. Compilers, the
  [required `sdkconfig` settings](TOOLCHAIN.md#required-sdkconfig-settings), the
  [partition table](TOOLCHAIN.md#partition-table),
  [build order](TOOLCHAIN.md#build-order), and the
  [acceptance checks](TOOLCHAIN.md#acceptance-checks) that say a build came out
  right. Read it before building.
- **[../tdisplay/README.md](../tdisplay/README.md)** - the bare ESP-IDF starter
  for this same board, and where the hardware facts were established: the
  verified pin map, the ST7789 traps the driver here inherits, and how to
  [flash the original clock firmware back](../tdisplay/README.md#restoring-the-original-clock).

## Why guests are not just more firmware

A standalone IDF app that draws a clock costs about **180 KB** - measured, not
estimated - because it ships its own FreeRTOS, newlib, SPI driver and panel
init. Put those in the OS once and a clock becomes the code that is actually
unique to a clock: **2-4 KB**.

The price is that a guest cannot be an ordinary firmware image. It gets:

- no libc beyond the seven functions in `gsdk/gb_rt.c`
- no ESP-IDF headers, no direct register access
- one entry point, `int gb_main(const gb_api_t *api)`
- everything else through `api`

## How a guest gets loaded

Guests are copied into RAM and relocated, not executed in place.

1. `.text` and its literal pools go into the **executable IRAM heap** (~75 KB
   available, so a 4 KB program is free).
2. `.rodata`, `.data` and `.bss` go into **DRAM**. They are separated from the
   code on purpose: IRAM only tolerates aligned 32-bit access, and string
   tables get read a byte at a time.
3. Because the two halves land at two unrelated addresses, every absolute
   address inside the image is stored as an offset and patched at load. The
   patch sites come from `--emit-relocs`, filtered to `R_XTENSA_32`. Four bytes
   per site, a couple of hundred bytes for a clock.

The alternative - execute-in-place from a flash slot - saves that RAM but
requires each guest to be linked for a fixed address, which breaks every time
the OS changes size. XIP stays on the shelf for guests too large for IRAM; the
`gslot` partition is already reserved for it.

## What a guest draws with

The panel calls in the API are what a *utility* needs: fill a rect, print a
line, let the OS's 5x7 font do the rest. A game cannot use them. `api->pixel()`
is a windowed SPI transaction - three commands and four payload bytes of
address window, for one pixel - so a starfield drawn with it manages
single-digit frames per second, and anything painted straight onto the panel
tears against the band that went down the bus a moment ago.

Every game here therefore assembles its frame in RAM one 16-row band at a time
and blits whole rows. astro wrote the spans, circles, 3x5 font and sprite
blitter for that. pinball copied them - and said so in its header, with a
reason that was true as far as it went:

> they are copied here rather than shared because a guest is linked on its own:
> each program is a self-contained image with no OS symbols to bind against, so
> the alternative to a copy would be a fourth library directory that build.ps1
> would have to learn about.

pacman would have been the third copy. It is not, because that reasoning missed
the option the whole system is built on: **code a guest cannot link against, it
can still call.** The rasteriser is in the OS now - `os/main/osgfx.c`, reached
through `api->gfx` - and both games were moved onto it.

Two rules keep it from turning into a display server:

**Nothing in it touches hardware.** Every call writes into a `gb_surf_t` the
*guest* owns and passes in - its band buffer, its clip rectangle, its pixels.
So nothing in the table is wrapped in the syscall guard: there is no SPI bus to
be caught holding, no allocation to reclaim, and a guest deleted mid-disc
leaves the OS holding no state at all. Getting the finished band onto the glass
is still `api->blit()`, and that one is guarded like everything else on the
bus.

**Nothing in it is speculative.** The table is the intersection of what astro
and pinball had already written for themselves - spans, boxes, disc, ring,
ellipse, line, round-capped bar, the band font, the ASCII sprite blitter,
`isqrt`, `isin`, `mix565`, a PRNG. Every entry existed twice before it was
moved once. Nothing was added because it might be wanted.

What it cost and what it bought, measured with `nm` across the change:

| | text (IRAM) | data |
|---|---|---|
| pinball, before | 9,528 | 3,192 |
| pinball, after | **8,444** | **2,968** |
| astro, before | 11,324 | 952 |
| astro, after | **11,180** | **948** |

Pinball gets 1.3 KB back; astro gets 144 bytes. The difference between them is
the point: what a guest pays instead of a local call is one indirect call
through the table, and astro's drawing is per-pixel - thousands of `fb_px()` a
frame - so the call-site overhead eats nearly everything the removal saved.
Pinball's calls are coarser and it keeps the lot.

Which makes the honest summary of this change: **the size win is small and
depends on how a guest draws. The reason to do it is that there is now one
implementation.** The band-boundary bug in the circle outline - the one the
seeding comment in `osgfx.c` describes - was found once in astro and fixed by
hand in pinball's copy; a third copy would have been a third chance to miss it.
And pacman, which is the program that prompted all this, simply started by
drawing.

That the surface belongs to the guest rather than to the OS pays for itself in
one more place. There is no alpha anywhere in the API, and pacman wants
translucent arrows over the board: it reads its own band buffer back and mixes,
in two lines, with no compositing model anywhere in the OS.

## Buttons

| gesture | launcher | guest |
|---|---|---|
| L tap | previous entry | `GB_EV_L_SHORT` |
| R tap | next entry | `GB_EV_R_SHORT` |
| **L hold 3 s** | toggle the info screen | `GB_EV_L_LONG` **+ killed by the OS** |
| **R hold 1 s** | run the selection | `GB_EV_R_LONG` |

Both long presses fire the moment the hold is earned - neither waits for the
release. That used to be true only of the right button: at the old 450 ms
threshold the left one could not yet tell an escape from the start of a
3-second kill, so it had to wait and see, and escape only happened when you let
go. Making escape and kill one three-second gesture removes the question.

### Gestures are not the whole of it

The table above is a queue of things that have already happened: a tap is known
when the button comes up, a hold when its threshold is earned. Nothing in it
says when a hold *ends*, so nothing built on events alone can move while a
button is down and stop when it is let go - which is most of what a game wants
from two buttons.

So `api->buttons()` sits beside the queue and returns the debounced state right
now, as a mask of `GB_BTN_L` / `GB_BTN_R`. Menus keep using events, because a
menu wants gestures; `astro` steers with the state and never looks at a tap.
The one thing state cannot buy back is the left button at three seconds - that
is the escape, it arrives with the kill, and a guest cannot decline it.

The two holds are not the same length, and should not be. Three seconds is the
price of destroying something, and the left button charges it. Accept destroys
nothing, so the right button asks for a second - past any tap, short enough
that starting a program does not feel like an argument with the board.

Holding left over a running program draws on it. One second in - long enough
that a tap which lingered never flashes anything - the OS paints a bar along
the bottom of the panel labelled `quit` and fills it over the remaining two
seconds. Letting go before it fills cancels, and the bar is the only thing that
says so; three seconds of nothing happening looks exactly like a board that has
stopped listening.

That bar is drawn from the input task, over the guest, and it takes the syscall
guard with a timeout of a few milliseconds rather than waiting for it. The
input task is the one task that must never block on a guest: a program wedged
inside a syscall holds that guard indefinitely, and waiting there would break
the kill gesture in precisely the case it exists for. A dropped pass of the
overlay is the cheaper loss, and it costs nothing anyway - the fill is computed
from how long the button has been down, so the next pass that does get the
guard draws the bar where it should be by then.

Drawing over a guest is not the same as being able to keep it drawn. A program
that repaints its whole frame - `astro`, `pinball`, `mandel` while it animates -
clears those rows dozens of times a second, and an OS that answered by
repainting the bar just as often would be in a race it loses about half the
time, on a bus it is fighting the guest for. Half a quit bar is worse feedback
than none.

So the OS takes the rows instead of competing for them. For as long as the bar
is up, `st7789_reserve()` clips every drawing primitive to the panel *above* the
strip, and the OS lifts that clip only for its own painting. The guest is not
told and does not need to be: `width()` and `height()` keep reporting the whole
panel so nothing reflows, its draws into those rows are simply dropped on the
way to the glass, and the bar stands still until the finger comes off. That is
the whole of the priority - one writer can reach those scanlines, and it is not
the guest.

The rows go back when the hold ends, and again when the guest exits, so the
launcher is never drawn clipped. What the strip covered is gone either way -
MISO is not wired, so the panel cannot be read back and a cancelled hold erases
the strip to the theme background instead of restoring it; a guest that
repaints its frame fills it in again on its own schedule.

The left hold is now a single gesture that does both things. The input task
emits `GB_EV_L_LONG` and asks for the teardown in the same pass, so a guest
polite enough to return from `gb_main` exits cleanly and one that is not gets
killed anyway. That half runs above both the launcher and any guest, so it
works on a program that has stopped reading events: a guest gets 400 ms to
notice `should_stop()` and return, after which the task is deleted and its
allocations are reclaimed. The one case the OS will not force is a guest wedged
*inside* a syscall - deleting a task that holds the SPI bus would take the
panel down with it, so that memory is leaked and logged instead.

## Settings

Two things belong to the whole board rather than to any one program - which way
up it is, and what colour it is - so the OS keeps them, in `os/main/osconf.c`,
and the **settings** program is just a face on that record. Both survive a
reboot. Neither lives in a file on `/progs`: that partition is a SPIFFS image
the build generates and `idf.py flash` rewrites wholesale, so settings kept
there would be erased on every reflash. NVS is left alone by an app flash.

The settings screen is one flat row of cells - two orientations, then one
swatch per theme - walked with L and R, applied with R-hold. Everything
previews as the cursor passes over it: land on a swatch and the program
repaints in that theme, land on an orientation and the panel turns. The accent
bar under a cell marks what is actually stored, and the title says `unsaved`
whenever the cursor is somewhere else. Leaving without applying needs no undo,
because a theme preview never existed outside that program's own drawing and
the OS restores the stored orientation whenever a guest exits.

A swatch is drawn as a screen in miniature - title bar, accent tick, three
weights of text - rather than as a colour chip, because what a theme is for is
what a screen looks like in it.

### Orientation is a request, not a rule

The stored orientation is the **system** orientation: the launcher is drawn in
it, and the panel is already in it when `gb_main` is entered, so a program that
does nothing at all honours it for free.

A program whose layout only works one way is expected to decline. `astro` is a
vertical scroller and calls `set_rotation` to force portrait; 135 rows of
playfield is a corridor, not a game. It still honours as much of the preference
as survives that: a board already set to portrait keeps the user's choice of
which way up, and only a board set to landscape leaves the question open, which
is the one case where `astro` offers its own flip. Declining costs a program
nothing - the OS puts the system orientation back when a guest exits, kill or
no kill - so the launcher always comes back the way the user left it.

### Themes

Five palettes - `midnight`, `amber`, `forest`, `slate`, `plum` - and seven
colour roles each: `bg`, `surface`, `fg`, `dim`, `muted`, `accent`, `warn`. The
launcher paints itself entirely out of those roles and has no per-theme special
case anywhere. Guests get the same table through `api->theme_get()`, so a board
set to amber does not go navy the moment a program starts - though a game is
still free to paint what a game paints.

Adding a theme is a row in the table in `osconf.c`. Nothing else knows how many
there are: the settings program asks, and the console lists whatever it finds.

## astro

The first program that is not a utility, and the one that says what the guest
model is actually good for. 12 KB of code and 20 KB of RAM buys a vertical
scroller with a parallax background - drifting nebulae, ring sections of a
gas giant, and dust that twinkles and moves in every direction - over which
asteroids tumble past a ship that can pick up shields and homing missiles. The
score is the distance covered before something hits you.

| gesture | effect |
|---|---|
| L held | thrust left, for as long as it is down |
| R held | thrust right |
| L held 3 s | the OS escape - see below |

Steering reads `api->buttons()`, not the event queue, for the reason above: an
event cannot tell you a button is still down.

The ship has mass. Thrust accelerates it and letting go does not stop it -
there is drag and nothing else, and at 400 px/s^2 a ship at full tilt takes
most of half a second to come to rest. Crossing the field means firing the
other way in time to arrive, which is the whole of the skill here: the rocks
are not hard to see coming, they are hard to be somewhere else for. Reversing
gets the drag as well as the thrust, so counter-firing always beats
accelerating and the ship stays steerable.

The left button at three seconds is the one thing the game cannot have back.
Crossing the field takes well under a second, so holding left that long means
parked against the wall rather than flying - but it still ends a run for what
looks like no reason, so the HUD says `RELEASE L` as it approaches.

Two decisions carry the rendering, both of them forced by the hardware:

- **Nothing is drawn through `api->pixel()`.** One pixel costs a windowed SPI
  transaction - three commands and four payload bytes to set an address window
  - so a starfield drawn that way would manage single-digit frames per second.
- **There is no framebuffer**, because 135x240 at 16 bits is 64 KB and the OS
  is using that memory. Frames are assembled a 16-row band at a time into 7.5
  KB and blitted, which makes every draw call a store into RAM and lets the
  panel see whole rows. Everything in `astro_gfx.c` clips to the current band,
  so drawing code is still written in screen coordinates.

The ring gets antialiased edges, and the way it gets them is worth a note:
blending the two end pixels of each scanline run - the obvious approach - fixes
an edge steeper than about 45 degrees and does nothing whatever for a shallower
one, which does not step across x within a row but across y between rows. On an
ellipse this eccentric both are on screen at once, standing vertical at the
apex and lying almost flat where the arc leaves the frame. So coverage comes
from the sweep: an edge crosses a row somewhere between where it sat on the row
above and where it sits now, and across that interval coverage runs from
nothing to everything. A sweep narrower than a pixel is widened to exactly one
pixel centred where it was, which turns the same formula back into the area
coverage of a step - so one routine handles a vertical edge, a flat one, and
everything in between.

Everything else follows from those. The HUD uses a 3x5 font of the program's
own rather than `api->text()`, because `api->text()` paints onto the panel and
would tear against a band that had already been sent. The asteroids are
rasterised per scanline from a bump table instead of blitted, because they come
in every size and they tumble. There are no floats anywhere: the rocks, the
ring geometry and the nebula falloff all run on an integer square root and a
32-entry sine table.

Powerups accumulate. A pickup adds stock that is carried until it is spent -
shields by being hit, missiles by being fired - and the status bar along the
bottom shows every powerup that exists with its count, dimmed into the bar at
zero rather than hidden, because an indicator that appears only once you
already have the thing cannot tell you the thing is there.

The caps are per powerup and nothing like each other: four absorbed hits is
already a lot of second chances, while a missile is spent in a third of a
second and a stock of four would be gone before the player noticed they had
it. Picking one up with full pockets is not wasted either - it raises that
powerup's level, which is what the per-level tables scale, and the level shows
as a small meter under its icon.

Acquisition ended up entirely table-driven: spawning, weighting, the pickup
test, the flash, the count, the bar and the upgrade rule all read the tables
and not one of them needs a case. Only *spending* a powerup knows what kind it
is, because only that differs. The header comment in `astro.c` lists the five
places a new one touches.

Missiles go straight up and nothing steers them. They used to home, and homing
removed the reason to line the ship up - which is the only aiming two buttons
and a lot of momentum can offer. They do hold their fire until something is in
the column ahead, which is trigger discipline rather than aiming: a round sent
into empty sky is a round gone, and a stock that empties itself while the
screen is clear is a stock the player never gets to decide anything about.

## mandel

Three fractals - the Mandelbrot set, a Julia set and a Lyapunov diagram - each
dropped somewhere worth looking at, in a palette invented on the spot. 9 KB of
code and no navigation at all.

| gesture | effect |
|---|---|
| L tap | another palette, on the same view |
| R tap | another place, in the same fractal |
| R hold | the next fractal |
| L hold | back to the launcher |

There is no zoom and no panning, and that is the interesting decision in the
program. Two buttons yield four gestures, one of which has to be "leave", so a
viewer that could be steered would be steered badly - three gestures is not
enough to find anything, and a fractal is mostly places not worth being. What
would have been navigation is spent on the search instead: every R tap costs a
fraction of a second of *looking* before it costs a second of drawing, and what
comes back has been measured rather than guessed.

### Finding somewhere worth rendering

Pointing this at a random point gives a dull picture nearly every time. The
inside of the set is one flat colour, the far outside is another, and the only
place worth a screen is the hair between them. Two things find it.

The first is a **bisection**. Take any point known to be inside and any point
known to be outside; the segment between them crosses the boundary, so halve it
until the ends are one screen-width apart, and the crossing is somewhere in the
middle at exactly the scale about to be drawn. That is a guarantee rather than
a hope, and it costs about forty escape tests. It works unchanged for all three
modes, because each has a notion of inside: in the set, or - for Lyapunov -
chaotic.

The second is a **preview**. Straddling the boundary is necessary and not
sufficient: a filament edge can be perfectly smooth and perfectly boring. So
each candidate is rendered 32 pixels wide through the same kernel that will
fill the screen - roughly six hundred samples, about a fiftieth of the work of
the real render - and scored on three things at once. How much of the value
range is actually used, so that a two-shade wash scores nothing. How often the
frame crosses between inside and outside, which is the boundary being present
rather than merely nearby. And how fast the value moves between neighbours,
measured relative to the range within the frame, so the number means the same
thing for an escape count and for a Lyapunov exponent. Scores run about 100 to
210, and the two ends are easy to tell apart by eye.

Candidates are proposed until one scores 175 or the search runs out of its 700
ms, at which point the best of them is used - so a demanding bar never means no
picture, only a longer look. The scale is drawn **once per search** rather than
per candidate, which matters more than it sounds: a search free to pick the
zoom as well would always return the deepest one it tried, since detail is what
the score measures and there is more of it further down. Fixing the scale first
turns the search into the question it should be asking - given this much of the
plane, where is the best of it - and is why the program shows the whole set as
often as it shows a filament.

### Fixed point, again

Q28, so one unit is 2^-28 and the range is +-8. Q8 would run out of resolution
before the first zoom; Q28 leaves three bits above the point, which is what the
iteration needs, since the escape test fires while |z| <= 2 and no coordinate
the kernel keeps is ever larger than 6. Every product is formed in int64 and
shifted back down, which on this core is two instructions rather than a call
into libgcc. The zoom stops at a half-width of 1.2e-4 - about 13000x - where a
pixel is still 270 units across and the shapes stay smooth.

The exception is the escape test itself, which stays in int64 on purpose: the
moment a point escapes, its square can reach 36, past what Q28 holds, and that
overshoot is exactly the value the smooth colouring needs. Escape counts are
integers, and an integer painted through a colour cycle gives the concentric
banding that makes a fractal look like a contour map; `n + 1 - log2(log2|z|)`
removes it for two table lookups per escaped pixel.

### Colour

Five colours, spaced along a ramp and blended between - the shape every palette
tool settles on, and the right one here for a reason specific to this program:
a fractal shows a palette as bands, so what the eye ends up judging is not the
five colours but the two hundred and fifty blends between them.

Which five is the whole question, and the classical answer is not usable. The
harmonies every generator ships - monochromatic, analogous, complementary,
split-complementary, triadic, tetradic - are hue offsets, and the interesting
ones put three or four saturated hues a third of the wheel apart. A blend
between those *is* a rainbow; run it through a fractal at two cycles to the
screen and it comes out psychedelic.

What replaced it is a walk rather than a harmony. A palette starts at a dark
hue and moves towards the hue of light as it brightens, because outdoors the
light is warm: embers to gold, sea to sand, moss to straw. Walking the other
way - green through blue into violet as it brightens - is a thing daylight
never does, and it reads at once as a machine choosing colours.

**The walk has to be long**, which cost two rewrites to learn. A short walk
gives five colours that are all the same colour, one hue dimmed and brightened,
which is a tonal ramp and not a palette. The starting hues are therefore chosen
for their *distance* from the light rather than for their own sake: a warm
palette begins at wine and comes round through red and ember, rather than
beginning at amber with nowhere left to go. Measured over rendered frames, an
image now averages about three distinct hue families where a monochrome one has
one.

That is also how magenta gets back in, having been thrown out once for having
nothing behind it outdoors. True of magenta alone; false of the road through
it, since violet to rose to ember is the most ordinary sight there is. The
difference is saturation, not hue, so the arc is walked in dust - any stop
landing near magenta gives up most of its saturation to a ceiling. What made
the old palettes psychedelic was never one hue, it was hues with no business
being adjacent.

**Value carries the rest.** All five stops climb from shadow to a tinted
near-white with saturation falling as they rise, which is the difference
between a colour that reads as lit and one that reads as painted. All five are
meant to be seen, and that is why the climb does not double back: an earlier
version spent a stop on returning to the dark end so the cycle would close, and
of the four left, the two dark ones read as near-black whatever hue they were
given and the brightest was nearly white. One visible colour out of five, which
is exactly why five stops kept looking like one.

**The cycle closes by folding.** The table is the climb in its first half and
the same climb backwards in its second, so every colour is passed twice per
cycle and there is no seam anywhere - the fold points are the ends of the ramp,
where the curve is already flat. What it costs is that bands come in symmetric
pairs, which on a fractal is not a tell, since the bands are already nested. It
is the same fold a sunset makes on water.

**Monochrome is kept at about one in twelve**, drawn from its own short list of
hues that can hold a whole screen alone - forest, teal, indigo, deep red. It
cannot use the ordinary starting hues, because two of those begin beside
magenta on purpose, meaning only to pass through on the way somewhere warm;
land a monochrome there and the entire picture is pink.

The stops are spaced unevenly, because five equal steps look mechanical and
letting one band run long while another turns quickly is what a sunset does.
The widths lean towards the light, since a quarter of the cycle spent near
black is a quarter spent showing nothing.

Two details of the blending matter as much as the colours. It runs in **linear
light** - square, mix, square-root back - because a straight average of two
sRGB values is darker than the light it claims to be mixing, and blended
naively red to blue sags through a muddy plum. And it uses **smoothstep** across
each segment, so the curve reaches every stop with zero slope and there is no
crease where two meet - which matters more here than in a gradient on a page,
because a fractal stretches some segments across a whole screen and squeezes
others into three pixels.

Value becomes position through a square root, and that is the difference
between a picture and a rash: escape counts pile up near the boundary, so a
linear mapping spends most of the palette in the fringe where the bands are
already thinner than a pixel. Once round the cycle across the range, or twice.

The interior gets its own colour - the darkest entry of the cycle, darkened
further - rather than black, because black beside a warm palette looks like a
hole cut in the picture rather than the floor of one.

### Three passes, and a byte per pixel

A **coarse** pass fills the screen in 4x4 blocks about a tenth of a second in,
so something is on the panel long before the rest of it finishes. The **fine**
pass then walks down a row at a time at one sample per pixel and leaves a
complete picture. The **refine** pass goes back over the pixels where the
picture steps between neighbours - the fringe beside a filament, the edge of
the set - and samples those five ways instead of one.

Measured on the board, for the whole range of what the search comes back with:

| view | to a complete picture | refined | then |
|---|---|---|---|
| whole set, 72 iterations | 152 ms | 14% of pixels | +154 ms |
| mid zoom, 170 iterations | 265 ms | 20% | +422 ms |
| deep zoom, 300 iterations | 1390 ms | 61% | +3188 ms |
| a new palette on any of them | 35 ms | as above | |

Which is why the refinement is a pass of its own and not part of the fine pass.
Between a seventh and two thirds of the screen wants the extra samples, so
folding it in would have multiplied the wait before anything appeared by four;
done afterwards it costs nothing that is being waited on, and an interrupted
refinement leaves a picture that is merely less smooth than it was going to be.
Every pass checks the buttons between rows, so a press during those three
seconds is answered when it is made.

The test for which pixels are worth it is asked in palette steps rather than in
iterations - is there a visible step in colour between these two pixels - which
means the same thing at every zoom, in every mode and under every palette.

What the blend averages is **colours**, not values, and that is the whole
reason it helps. Halfway along a filament the five values are scattered across
the range, and the colour of their average is not the average of their colours;
it is just another arbitrary colour, which is what the speckle already was.
Counted as isolated pixels - ones that differ sharply from all four neighbours,
which is what speckle is - averaging values removed 27% of them and averaging
colours removes 70%. Blending what is actually shown is also the only version
that can soften the edge of the set itself, where the samples that land inside
have no value to average at all.

The fine pass keeps one byte per pixel - a palette index, not a colour - and
that byte is what makes L tap instant: a new palette is a new lookup table over
the same 32 KB of indices, so the screen repaints without iterating anything,
in 35 ms. A refined pixel is a blend of five colours and has no index, so the
refinement paints to the panel and never into the buffer. A recolour therefore
repaints from the buffer at once and then refines again, in that order, which
is the same two things the eye wants in the order it wants them. It also keeps
the buffer meaning exactly one thing - what the fine pass drew - so the
refinement never compares a pixel against an already-refined neighbour.

Which is also why the cycle count and the phase belong to the scene and not to
the palette: leave them in the palette and a recolour would move the bands
under the indices already stored, and a view restored from NVS would come back
a similar picture rather than the same one.

The buffer is optional. If the OS cannot spare 32 KB the program re-renders
instead of repainting and skips the refinement, which is the picture the fine
pass drew. It has never had to: there is ~264 KB of free heap when a guest
starts - it was ~282 KB before the radio was linked in - and mandel asks for
44.7 KB of it.

The view survives a reboot, and only the 32-bit seed of the palette is stored
rather than the table, since the generator is reproducible.


## pinball

A three-ball table. The plunger is on the right button, the flippers are both
buttons, and the score sits on a backglass strip across the top 16 rows the way
a real one keeps it.

There were slingshots above the flippers to begin with. They came out: on a
109-pixel-wide playfield a kicker either side of the drain has nowhere to throw
the ball except back across the drain, and what reads as lively on a full-size
table reads here as the ball being taken away from you. The lower third is open
now, and the guides feed the flippers instead.

| gesture | effect |
|---|---|
| R hold, ball in the lane | winds the plunger back; a full second is full power |
| R release | launches with whatever it wound up |
| L held | left flipper, for as long as it is down |
| R held | right flipper, once the ball is in play |
| L tap | leave the attract or game-over screen |
| L hold 3 s | the OS escape |

The plunger is the one control that wants both halves of the input API. The
charge follows `api->buttons()`, because an event cannot say a button is still
down; the launch also fires on `GB_EV_R_LONG`, so that a hold past a second
does what it looks like it does rather than waiting for a release that may not
come. Below `PLUNGE_MIN` the ball stalls at the divider and drops back into the
lane - which a real table does, and which on a 135-pixel screen reads as the
game being broken - so the weakest plunge here still gets round the arch, and
the charge decides how far along it the ball is carried.

The left button at three seconds costs more here than in astro. Trapping a ball
on a raised left flipper is an ordinary pinball move and holding it that long
ends the game, and no guest can decline that gesture. So the backglass says
`RELEASE L` at 1.8 s, before the OS paints its own quit bar over the flippers
at two seconds.

### The table is a list of segments

The ball is a circle with a position and a velocity in Q8 fixed point; the
table is sixteen line segments, seven circles, three drop targets and two
flippers. Every contact is resolved the same way - closest point on the
surface, push out along the normal, reflect through it with a restitution that
depends on what was hit - and each frame is split into as many substeps as it
takes to keep the ball under two pixels of travel per test, because a 700 px/s
shot crosses a one-pixel wall in a fifth of a frame.

The flippers are the only surfaces that move, and that is exactly why they are
worth anything: a flipper reflects the ball's velocity *relative to the
surface*, so the same contact does nothing on a resting flipper and throws the
ball up the table on a swinging one. The surface velocity at the contact point
is the angular speed times the distance from the pivot, so catching a ball on
the tip sends it further than catching it at the base - not a special case,
just the arithmetic. Measured on the bench in `sim/`, a cradled ball leaves at
270 px/s from the base and 690 from the tip, which is the difference the game
is played on.

None of that worked at first, and the reason is worth keeping. A swing is 40
angle units in about 45 ms, so a flipper stepped once per frame crossed 27 of
them in one go - most of its travel - and a ball resting on it was simply on
the far side by the time anything was tested. The contact then resolved the
only way it could, pushing the ball out of the bar the shortest way, which for
a ball now underneath is straight down into the drain: cradling and flipping
did the exact opposite of what a flipper is for. So the sweep is stepped with
the ball, a couple of hundred microseconds at a time, which needs the angle in
Q8 - at whole units a substep that short rounds to no movement at all - and it
needs the substep count sized from where the flippers are *going* rather than
how fast they were moving last frame, because the frame that matters is the one
where the button has just gone down and the bar is still stationary.

The drain mouth is sized the same way, by what has to fit through it: not the
ball but the ball plus the bar's own radius on each side, 14 px rather than 7.
The tips started 11 apart, and a ball down the middle sat on both of them until
the stuck-ball nudge shoved it off. The pivots moved out to leave six pixels of
clearance - enough to fall through, narrow enough that the middle is still a
mistake.

### Flipper rubber is grippy, and that is the shot

Sweeping the bar correctly still did not make the shot every player wants -
letting the ball roll down to the tip and firing from there. Two things were
missing, and both are friction.

A contact only ever exchanged an impulse along the surface normal. For a ball
sitting beside the round end of the bar that normal points *sideways*, so the
hardest shot on the table came out as a shove towards the drain. Real rubber
grabs the ball and carries it along with the sweep, so friction is now applied
against the surface's own motion rather than against the table, whether or not
the ball was closing on it - a ball lying on a flipper is closing on nothing,
and is exactly the case that needs grip. The same friction is what holds the
ball on the bar: without it the ball tobogganed off the tip in a quarter of a
second, because a real playfield is tilted about six degrees and this one is
the whole of gravity. The rest angle came down for the same reason.

A flipper standing still grips much less than one that is moving, which is not
how rubber works but is how this has to be modelled. Friction against a static
surface mostly becomes spin, and spin is the one thing the ball does not have
here: it would come back out of the next contact as a curve, and with nowhere
to keep it, a grippy static flipper only eats speed the player earned. The two
coefficients were picked off the bench - measuring how long the ball spends in
the part of the bar worth shooting from, against how much speed the table loses
overall:

| idle grip | time in the strong zone | average ball speed | drains |
|---|---|---|---|
| 16 | 170 ms | 251 | 17 |
| 32 | 270 ms | 232 | 14 |
| **48** | **390 ms** | **239** | **11** |
| 58 | 510 ms | 226 | 13 |

Cradled and fired, the ball now leaves at 400 px/s from the base of the bar and
740 from the tip, and rolling down to the tip is a 390 ms window rather than a
race. Past the end it is gone, the way it is on any table.

Grip also found a hole in the walls. A flipper is not a wall, and a ball with
enough sideways speed can now be carried along one and straight past its pivot
- out through the side of the table on its way to being counted as drained. The
two trough walls under the pivots close that off.

The outer boundary is a closed loop apart from the drain between the flipper
tips. That closure is the invariant to preserve if anything in the geometry is
ever moved: a one-pixel gap is a ball that leaves the table and never comes
back. It is checked by `sim/harness.c`, which compiles the guest for the host
against a stub `gb_api_t` and drives it with a script that plunges and flips:
ten minutes of play in about a second, and it says whether the ball ever left
the table. Seven complete games, 20 000 frames, no escapes - and it is how the
launch geometry was caught being wrong the first time, when the divider tip
sloped uphill and every plunge dropped back into the lane. It also writes
frames out as PPM, which is the only way to look at a layout decision without
reflashing.

### Gumball and Darwin

Which of the two is on the backglass is drawn afresh at the start of every
ball, so a three-ball game is normally one of each. With them come the wash over
the felt, the colour of the rails and the lamps, the letter on the bumper caps,
and the three letters the top lanes spell - `G-U-M` or `D-A-R`, lit as the ball
rolls over them and worth a multiplier when all three are in.

Both faces were traced off a front-on reference rather than drawn by eye: the
reference was cropped to each head, box-filtered down to 34x30 cells, and every
cell resolved to whichever of the show's flat colours it was nearest. That is
why the palettes in `cast.c` are exact values.

Three things had to be taught to the quantiser, and each of them was a blemish
on a face before it was. **Black is counted at a third of its weight**, because
every outline in the show is black and every one of them is thinner than a cell
at this size - an honest mode handed whole features to their own outlines and
Gumball's ears came out as solid black wedges. The silhouette is drawn back on
afterwards, one cell thick, so nothing is lost by letting colour win inside the
face. **A cell whose colour barely occurs around it is an artefact**, not a
feature: downsampling lays a rim of half-way colours along every outline, and
those land as single stray cells - the black holes in a cheek, and a pink
speckle all over Darwin where orange met black. And **white is background or it
is an eye**, a difference that is not in the colour but in whether the border
can reach it.

What is left is what a quantiser cannot do at all. Gumball's eyebrows are a
pixel high on a forehead four pixels tall, and Darwin's tongue is the same
colour as that rim. Both go back as shapes, in the generator, where there is no
ambiguity about what they are.

The portrait is drawn into the felt rather than over it, tinted about 40% of
the way to the background colour. At full strength it reads as a picture
standing in front of the table and the ball vanishes into it; tinted, it reads
as printed on the playfield, which is what table art is. The ball got a dark
outline for the same reason: it crosses a lit face and a black inlane in the
same second.

## The radio

WiFi is in, in the only shape a guest can reach it: **listening**. There is no
association, no IP stack and no credential anywhere in the API. A program can
ask what is on the air and how strongly it arrives, and that is all it can do -
which is enough for a survey, a channel census or a hunt for one particular
radio, and is not enough to do anything to a network.

It is off at boot, brought up by the first guest that asks, and dropped again
by `guest_supervise()` when that guest ends, kill or no kill. Measured on the
board: 262,768 B free with it down, 227,972 B with it up and `wherouter`
running, so the radio's own share is about **21 KB** and the program's is the
rest. Switching it off gives all of it back to within a kilobyte.

Twenty-one is a much smaller number than the driver usually costs, and it is
the buffer sizes in `sdkconfig.defaults` that make it so. Nothing here carries
traffic - it listens to beacons and to other people's frames - so the static
receive buffers drop from ten to four, block acknowledgement comes out
entirely, and the transmit path is left with the minimum the driver will
accept. The IRAM optimisations stay off for the reason they were already off:
they are a static reservation out of the pool guests are loaded into.

Four calls, in `oswifi.c` behind `gapi.c`:

| call | what it is for |
|---|---|
| `wifi_power(on)` | choose the moment, or give the heap back without exiting |
| `wifi_scan(out, max, channel, dwell)` | a census, sorted strongest first |
| `wifi_watch(bssid, channel)` | park on one channel, keep every frame that BSSID sends |
| `wifi_watch_poll(out, max)` | drain what has arrived since last time |

`wifi_scan` takes a channel because most callers do not want all thirteen. A
full sweep is a beacon interval times thirteen - a second and a half - and a
program that scans one channel per frame gets the same coverage at eight frames
a second instead of one screen freeze in three.

### Nothing transmits

An **active** scan puts a probe request on every channel it visits, which turns
a survey into emission on channels the board has no idea whether it is allowed
to use: 12 and 13 exist in some regions and not others, and the ESP32's default
country is the conservative one. A **passive** scan listens for the beacons
that are already in the air. It costs a beacon interval per channel instead of
a round trip, and it answers exactly the question a survey is asking.

The watch is passive for that reason and for a second one. A probe response
tells you the AP heard *you*; what a finder wants to measure is the other
direction, which is the beacon it was already sending.

The price is that a hidden network stays hidden - its name would take a probe -
so it comes back flagged `hidden` with an empty SSID, and `wherouter` calls it
by the last three octets of its BSSID.

### Scan and watch are exclusive

One radio, one channel. A scan therefore cancels a watch, and the OS does
**not** put it back afterwards. That is stated in the ABI rather than hidden in
the implementation: a guest that interleaves the two - as `wherouter` does when
its target moves channel - decides for itself when the radio goes back to
looking at the thing it was looking at.

The watch itself is promiscuous mode with the filter set to management and data
frames, matching `addr2`, the transmitter address, at offset 10. Control frames
are excluded because not all of them have one: an ACK is ten bytes and carries
only a receiver. Frames land in a 64-entry ring that overwrites its oldest, so
a guest polling a few times a second never misses one and a guest that stops
polling loses the stale end rather than the fresh.

## wherouter

A signal-strength gauge for one radio, and a census to pick it out of. 6.7 KB
of code, and the first program that needed the OS to grow a new capability
rather than a new drawing trick.

| gesture | list | finder |
|---|---|---|
| L tap | previous row | change what the trace spans |
| R tap | next row | forget the best and worst marks |
| R hold | lock on and hunt it | back to the list |
| L hold | the OS escape | the OS escape |

Both orientations, and the program never calls `set_rotation` to get them - a
list is a list either way up, and the gauge lays itself out from the panel it
is handed. Landscape puts the number and the trend badge side by side; portrait
stacks them and spends what it saves on the trace, which is the shape that
orientation is genuinely better at.

### The list is a table, not a scan result

A scan result is one channel. Sweeping thirteen takes a second and a half,
which is long enough that a list drawn from the latest scan alone would show a
third of the networks in the flat and blink the rest in and out.

So the sweep is one channel per frame and it feeds a table that persists across
channels. Rows leave it by growing old - eight seconds without a beacon and a
row is drawn faded, twenty-five and it is forgotten - rather than by being
absent from one pass. What that buys is a list that is complete after the first
sweep and stays complete, on a screen that is still answering buttons at eight
frames a second.

Two things then keep it readable while it is moving.

Each row's strength is an **exponential average**, not the last beacon, because
consecutive beacons from a stationary AP land as much as 6 dB apart. Four
samples of memory is about a second and a half of standing still - short enough
that walking into the next room still visibly promotes a row.

And the list **re-sorts once per sweep**, not once per channel. Re-sorting
after every scan would be correct and unusable: rows changing places eight
times a second, under a cursor the user is trying to aim, over differences of
one decibel. Across the sort the cursor is pinned to a BSSID rather than to an
index - the row someone is looking at is the row they meant, wherever the sort
has just put it.

The bar under each name is **one pixel tall**. A row is two lines of
information and a rule under them; making the rule mean something costs no
height at all, and a column of them down the screen is a shape the eye reads
before it has read a single number.

### The finder is a difference between two averages

Locking onto a row parks the radio on that channel and hands over every frame
that BSSID transmits - about ten beacons a second on its own, more if anyone is
using the network. That is the reason it is a watch and not a repeated scan: a
one-channel scan spends a beacon interval to produce **one** number, and a
watch produces every number the AP emits. Ten samples a second is the
difference between a gradient you can see while walking and one you have to
stop and wait for.

Two averages run over those samples. The fast one has about half a second of
memory and is the big number on screen; the slow one has about three seconds
and is what the fast one is compared against. Walk towards the router and the
fast average climbs out of the slow one; stop, and they close up again within a
few paces' worth of time. Nothing has to know how fast you are walking or which
way you are facing - the two time constants do that work, and the badge just
reports which side of 1.5 dB the difference is on.

### It is not trilateration, and the metres are a guess

Trilateration wants three known positions and a distance from each. A board in
one hand has one position and no idea where it is. What one antenna can
honestly do is say whether the last two steps helped, and that turns out to be
enough to walk up to a router: **the gradient is the instrument, not the
number.**

There is a distance under the badge, from the usual log-distance path loss fit
- `-40 dBm` at one metre, exponent 2.7 - tabulated every 5 dB and interpolated,
since a guest has no `powf` and a curve with a factor of two in it does not
deserve more resolution. What makes it a guess rather than a measurement is the
reference: transmit power varies by 6 dB across ordinary routers, antenna gain
by as much again, and a body standing between the two costs 3-6 dB on its own.
So it moves the right way and it is worth about what "warm" is worth. It is
labelled `est` on screen for that reason.

802.11mc fine timing *would* give a real distance off a single exchange. The
ESP32-D0WDQ6 on this board cannot do it - FTM arrived with the S2, and every
part that has it is a later one.

### The trace wraps rather than scrolls

The last minute or so of signal runs across the bottom of the screen, and the
write head wraps around and overwrites the oldest column, the way a heart
monitor does.

Scrolling would mean redrawing every column of the trace on every sample. MISO
is not wired on this board, so "shift the picture left by one" is not an
operation the panel offers - only "draw the whole thing again, one pixel over".
At ten samples a second across two hundred columns that is the whole frame
budget spent moving pixels sideways. Wrapping costs three narrow writes: erase
the head, plot the sample, erase the two-column gap that runs ahead of it so
that which end is *now* is never in doubt. The price is that the oldest data
sits to the right of the newest, which takes one glance to learn.

A column is a slice of time, not a sample, and what it draws is the range -
highest and lowest - over that slice, as a vertical stroke. A single dot per
column would render the scatter between beacons as noise; the stroke renders it
as thickness, which is information: a thick trace is a signal fighting
something, a thin one is a clean path. L tap moves between three spans, and the
footer works out what each one is worth from the width the orientation left
rather than claiming a number that would be wrong on one of them.

An empty column is drawn as the running average carried across, not as a
dropout. At the fastest span a column is shorter than the gap between two
beacons, so an empty one is the ordinary case and marking it as a hole would
put a dead zone in every other pixel. A genuine dropout - nothing at all for
two and a half seconds - is marked at the floor, because when the thing is
being used to map a flat, the holes are the point of the exercise.

### When the AP moves

Access points change channel on their own when the band gets busy, and a watch
parked on the old one hears nothing. Five seconds of silence and the finder
starts walking the band looking for the same BSSID, one channel per frame, so
the buttons keep working while it does. Finding it re-arms the watch and
nothing else changes: same target, same marks, same trace, and the title says
which channel it is currently trying.

## pacman

The arcade board - 28x31 tiles, 240 pellets and four energizers, the tunnel on
row 14, the ghost house in the middle - on a panel 135 pixels wide, with two
buttons.

Both of those are problems, and the program is mostly the two answers.

### Two buttons, and a press is an intention

Pac-Man never stops moving, so a button cannot mean "go this way": there are
four ways and two buttons. It means *turn*.

| gesture | effect |
|---|---|
| L | turn left at the next place there is a left turn |
| R | turn right at the next place there is a right turn |
| L L | about face, immediately |
| R R | the same |
| R other way | cancels a turn that is waiting |
| L hold 3 s | the OS escape |

The first version made a press mean "turn here", measured against the one tile
Pac-Man was about to stand on, and it was unplayable. At seven tiles a second a
junction is open for about a tenth of a second, so a press a moment early found
no left turn available and did the only other thing it could - it spun him
round, away from the corner he was aiming at. The game became an exercise in
hitting a frame.

So a press is **remembered**. It looks at the tile ahead, and if the turn is
not possible there it stays pending and looks again at every tile centre until
it finds one, up to four tiles later. Pressing early is not merely forgiven, it
is how the game is meant to be played: aim at the corner from down the corridor
and he takes it when he gets there.

The about-face is the one thing that cannot be buffered - a reversal you wanted
a second ago is a reversal into the ghost you were running from - so it happens
immediately, and it is not a special case: a second press composes with the
first, and left-then-left is a half turn. When a buffered turn has nowhere to
go at all - a straight corridor with no opening within those four tiles, or a
wall dead ahead - the press falls back to the about-face on the spot. That is
the "or 180" in the rule, and it is what makes two buttons enough: no way round
is a way back.

Turning reads `api->buttons()` for a press edge rather than the tap in the
event queue, because a tap is only known when the button comes *up* - a
hundred milliseconds of nothing after you meant to turn. The menus still use
events, because a menu wants gestures.

Two translucent arrows sit over the board at the bottom corners, beside the
buttons themselves, showing where each button would send him if pressed now -
and drawn stronger when a turn is already waiting, because at that point the
arrow has stopped being advice and become a thing that has been asked for. Held
the other way up, the hints move to the other end of the panel and swap sides,
so each one stays next to the button it is talking about.

### The camera

Drawn to fit, the board is five pixels to the tile, Pac-Man is a two-pixel dot,
and the game is unplayable in the specific way where you cannot tell which dot
you are.

So the view is not fixed. Every frame the camera takes the bounding box of
Pac-Man and every ghost that is **out on the board**, adds a tile and a half of
air, and picks the zoom that fits it - never wider than the whole board, never
closer than about six tiles across. Chased into a corner by one ghost, you get
the corner. Spread out, you get the board.

"Out on the board" does real work there. Ghosts waiting in the house and eaten
ghosts walking home as eyes are left out of the box: neither can touch you,
both sit near the middle of the board, and counting them pins the camera at its
widest for the first ten seconds of every life - the ten seconds when there is
nothing to see.

Three details separate that from a camera that makes you seasick:

- **Out is urgent, in is not.** A ghost appearing at the far end of a corridor
  has to be on screen now, so the zoom follows outwards immediately. The room
  it leaves behind when it goes away is worth nothing, so zooming back in waits
  a third of a second to see whether the framing holds. Without the wait, every
  ghost that ducks behind a wall block pumps the zoom.
- **Everything moves exponentially**, closing a fixed fraction of the gap per
  frame scaled by the frame time, so a slow frame does not lurch and a chase
  into a corner reads as one movement rather than as a pan and a zoom that
  happen to overlap.
- **The smoothing may lag, but it may not lose anyone.** After the smoothing
  runs, a hard clamp refuses any frame where the box would not fit: it pulls
  the zoom back to exactly what fits and drags the view the minimum distance
  that puts the last of them back on screen. It binds rarely and for a frame or
  two, which is what makes it invisible as motion and reliable as a promise.

The clamp is worked in **pixels**, not in world units, and that is the fix
rather than a detail. Sprites have a floor of three pixels on their radius so
that a ghost at the far zoom is still a ghost - which means that below about
seven pixels to the tile, zooming out to fit a box makes the sprites *bigger*
in world units and invalidates the fit that was just computed. Asking how many
pixels are left once the two half-sprites at the ends have had theirs removes
the circularity in one pass.

The tunnel is the other half of the same idea. The board is a cylinder, so the
maze is drawn modulo 28 columns over whatever range the viewport covers - which
may run from -6 to 21 - and entity positions are folded to the copy nearest the
camera. Pac-Man walks into the tunnel and slides off one edge and back on at
the other with the camera tracking him the whole way, instead of the picture
jumping a board width sideways at the seam.

### What the simulator is for

Three questions cannot be answered on the panel, so `pacman/sim/` compiles the
guest for the host against a stub OS - and against the *real* `osgfx.c`, so the
frames it dumps are the pixels the board would draw.

- **Can two buttons drive Pac-Man everywhere?** The auto player is a
  breadth-first search to the nearest pellet that steers with nothing but the
  same two presses a person has, double press and all. It clears a board in
  about 50 seconds of play, which answers the question in a way that twenty
  patient minutes on the board would not.
- **Does the camera keep its promise?** Every entity is projected through the
  real transform every frame and checked against the viewport. The number the
  code is tuned to is zero: over 9,000 frames - four and a half minutes - the
  worst excursion past the edge is 0 px. It was 35 px before the clamp, and
  2 px before the clamp was rewritten in pixel space.
- **Does anything end up inside a wall?** Half a tile of drift at seven tiles a
  second looks exactly like a ghost that was always there. Zero frames.

`TURNBENCH=1 ./sim` is the regression test for the press-early rule: it presses
the left button from every distance between 0 and 5 tiles before a corner and
prints what happened. Everything inside the window takes the corner; the first
distance outside it does the about-face, which is the rule working rather than
failing.

## Building

Guests first - the OS packs `guests/out/` into its SPIFFS image. The full
prerequisites are in [TOOLCHAIN.md](TOOLCHAIN.md#hard-requirements), the
reasoning behind the two-build-system split is
[ARCHITECTURE.md 8](ARCHITECTURE.md#8-build-pipeline), and
[the acceptance checks](TOOLCHAIN.md#acceptance-checks) say whether the result
is right.

```powershell
cd greenbox-os\guests ; .\build.ps1
. C:\esp\esp-idf\export.ps1
cd ..\os ; idf.py build ; idf.py -p COM5 flash monitor
```

## Serial console

UART0 at 115200, for the things two buttons cannot do:

```
ls  run <name>  kill  settime <epoch>  tz <minutes>  free  reboot
conf                    show the orientation and the theme list
conf rot <0-3>          0,2 portrait  1,3 landscape
conf theme <n>          by index
l r L R                 inject tap-L, tap-R, hold-L, hold-R
```

Reading flash back over this cable needs care - see
[the CP2102 quirk](TOOLCHAIN.md#serial-quirk-on-this-board).

`conf` is the way back from an orientation that makes the screen hard to read,
and the only way to reach the upside-down rotations 2 and 3 - the settings
program offers the two shapes, not the four MADCTL values.

There is no 32.768 kHz crystal on this board, so the RTC drifts on the order of
minutes per day, which makes setting the clock a routine rather than a one-off.
The **clock** program does it with the buttons alone - R-hold enters setting
mode, the flashing field is the one L and R move, another R-hold swaps hours
for minutes, and every press is applied immediately - so `settime` is now the
scripted path rather than the only one. Both go through the same place: the OS
owns the wall clock, and guests reach it through `set_time` in the ABI.

## Not done yet

- **Anything that needs to associate.** The radio listens; it does not join a
  network. NTP would fix a clock that drifts minutes a day, and an HTTP
  endpoint would put a `.gbx` on the board without a serial cable. Both want an
  association, credentials to make one with, and somewhere to keep them - which
  is a larger change to the ABI than "here is what is on the air".
- **OTA of the OS itself.** `ota_1` and `otadata` are already in the partition
  table for it.
- **XIP guests** for anything that outgrows the IRAM heap. The radio took
  14.6 KB of static IRAM, but not out of that pool: it came off the IRAM tail,
  and the largest executable block a guest can be loaded into is still the same
  110,592 B it was before. The ceiling did not move.
