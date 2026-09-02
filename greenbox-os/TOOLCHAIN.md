# Greenbox OS - toolchain requirements

Requirements for whoever sets up the build. Nothing here needs installing from
the internet unless a line says so explicitly; the machine already has what is
listed under "Already present".

[README.md](README.md) is the way in - what the OS is and what the programs do.
[ARCHITECTURE.md](ARCHITECTURE.md) explains *why* the build looks like this,
in particular [8. Build pipeline](ARCHITECTURE.md#8-build-pipeline) for the
compiler flags and [9. Where things are stored](ARCHITECTURE.md#9-where-things-are-stored)
for the partition layout below.

## Already present (verified on this machine)

| thing | where | version |
|---|---|---|
| ESP-IDF | `C:\esp\esp-idf` | v5.2.2 |
| Xtensa toolchain | `C:\Users\mieczu\.espressif\tools\xtensa-esp-elf\esp-13.2.0_20230928` | gcc 13.2.0 |
| IDF python env | `C:\Users\mieczu\.espressif\python_env\idf5.2_py3.11_env` | 3.11 |
| Board | TTGO T-Display on COM5 | ESP32-D0WDQ6-V3, 4MB flash, no PSRAM |

Environment for any IDF command:

```powershell
. C:\esp\esp-idf\export.ps1
```

## Hard requirements

1. **Bare ESP-IDF only.** No Arduino core, no PlatformIO, no display libraries
   (`TFT_eSPI`, `LovyanGFX`, `Arduino_GFX`). The panel driver is hand-written in
   `os/main/st7789.c` and stays that way.
2. **No new IDF components from the registry.** Everything the OS uses -
   `spiffs`, `nvs_flash`, `app_update`, `esp_partition`, `driver` - ships in
   IDF 5.2.2. If a task seems to need a managed component, raise it instead of
   adding one.
3. **No pip installs.** `tools/mkguest.py` parses ELF32 by hand precisely so
   that `pyelftools` is not needed. Run it with the IDF python or any python
   3.8+.
4. **Two separate build systems.** The OS is an IDF project. The guests are
   *not* - they invoke the bare cross-compiler directly with `-nostdlib`. Do
   not try to build guests as IDF components; that would drag IDF into a binary
   that must stay ~3 KB.

## Required sdkconfig settings

`os/sdkconfig.defaults` carries these. If you regenerate the config, keep them:

| setting | value | why |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | y | the board's actual flash |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | `partitions.csv` below |
| `CONFIG_COMPILER_OPTIMIZATION_SIZE` | y | -Os |
| `CONFIG_NEWLIB_NANO_FORMAT` | y | saves ~40 KB of printf |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` | y | chip is rated for it |
| `CONFIG_ESP_WIFI_IRAM_OPT` | **n** | keeps IRAM for guest code |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | **n** | as above |
| `CONFIG_ESP_WIFI_EXTRA_IRAM_OPT` | **n** | as above |
| `CONFIG_ESP_WIFI_SLP_IRAM_OPT` | **n** | as above |
| `CONFIG_FREERTOS_USE_TRACE_FACILITY` | y | the launcher reports task state |

The four WiFi IRAM options matter even before WiFi is enabled. They are static
reservations paid whether or not the radio ever runs, and together they take
about 31 KB out of the pool guest code is loaded into. The radio here listens
and never carries traffic, so what they buy is not wanted. Note the `ESP_WIFI_`
prefix: these were `ESP32_WIFI_` in older IDF, and the old spelling is silently
ignored rather than rejected.

The radio is also sized for listening rather than throughput - passive scans
and one BSSID in promiscuous mode, so the TX path is vestigial and the RX
buffers only ever hold other people's frames:

| setting | value |
|---|---|
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | 4 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 8 |
| `CONFIG_ESP_WIFI_TX_BUFFER_TYPE` | 1 |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 8 |
| `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED` / `_RX_` | n |
| `CONFIG_ESP_WIFI_NVS_ENABLED` | n |

With those the radio measures about 21 KB of heap while it is up. `NVS_ENABLED`
is off because there are no credentials to keep - NVS here is the OS settings
and the wall clock.

## Partition table

`os/partitions.csv`, 4 MB total. Guest slots are `data` type on purpose - if
they were `app` the bootloader would try to boot them.

```
nvs,      data, nvs,     0x9000,   0x4000
otadata,  data, ota,     0xd000,   0x2000
phy_init, data, phy,     0xf000,   0x1000
ota_0,    app,  ota_0,   0x10000,  0x100000
ota_1,    app,  ota_1,   0x110000, 0x100000
gslot,    data, 0x40,    0x210000, 0x80000
storage,  data, spiffs,  0x290000, 0x170000
```

There is deliberately no `factory` partition: with only OTA slots present the
bootloader falls back to `ota_0` when `otadata` is blank, which is what a fresh
flash produces. `ota_1` is reserved for rollback-safe OS updates over WiFi
later; `gslot` is reserved for the future XIP loader and is unused in v1.

## Build order

Guests must be built before the OS, because the OS build packs
`guests/out/*.gbx` into the SPIFFS image.

```powershell
# 1. guests  (bare toolchain, no IDF env needed)
cd greenbox-os\guests
.\build.ps1                     # -> guests\out\clock.gbx

# 2. OS
. C:\esp\esp-idf\export.ps1
cd greenbox-os\os
idf.py build
idf.py -p COM5 flash monitor
```

## Serial quirk on this board

The CP2102 (`VID_10C4&PID_EA60`, serial `021631B2`) intermittently drops a byte
on bulk **reads** - `expected 0x1000 bytes but received 0xfff` - at 460800 and
921600, and even at 115200 on a single 4 MB read. Writes and flashing are fine.
If you need to read flash back, do it in 256 KB chunks with retry and
concatenate. Do not "fix" this by lowering the flash baud rate for writes. The
same cable, and the same symptom, is written up in
[../tdisplay/README.md](../tdisplay/README.md#serial-quirk).

## Acceptance checks

The OS build is correct when:

- `idf.py size` reports the app under 1 MB (the `ota_0` size).
- Boot log contains `guest: exec heap %u B` with a value above 40000.
- The launcher lists `clock` and long-press-R starts it.
- `tools/mkguest.py --info guests/out/clock.gbx` reports a total under 8 KB and
  a nonzero relocation count.
