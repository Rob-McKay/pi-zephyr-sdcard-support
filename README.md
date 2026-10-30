# SD card support for the Raspberry Pi 3A+ on Zephyr

Tests, a code review and fixes for the BCM283x SDHost (microSD) driver
proposed for Zephyr in
[PR #110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196), on a
Raspberry Pi 3A+.

The aim is a driver that applications can rely on for storage, and
results and fixes that can be offered to the PR.

## Status

- **Review:** twelve findings, from undefined behaviour to a race that
  wedges the card ([review](docs/reviews/pr110196-sdhost-review.md)).
  Findings 1, 2, 4 and 6 reproduced on hardware, and 12 was found there.
- **Fixes:** `zephyr/patches/zephyr/sdhost-stop-open-read.patch` stops a
  multi-block read that runs on into the next block (finding 12), and a
  failed one (finding 2 for reads). `sdhost-busy-timeouts.patch` waits as
  long as the spec allows for a card's busy after a write or erase
  (finding 1). `sdhost-data-timeout.patch` sets the controller's data
  timeout to 1 s at every bus clock; it was 314 ms at 50 MHz (finding 4,
  not yet run on the Pi).
- **On a real Pi 3A+ with a 64 GB SDXC card, with the fix** (21 October
  2026, [log](docs/reviews/logs/2026-10-21-sd-qual-pi3a-sdxc64-run8-corrected.log)):
  every qualification test passed except finding 4 (a 314 ms data timeout)
  and a wrapping bounds check in Zephyr's SD stack, since fixed here
  (`sd-ops-bounds-overflow.patch`; it passed on the Pi in the
  [ninth run](docs/reviews/pr110196-sdhost-review.md#7-results-on-a-real-pi-3a)).
  No run-ons in over 100,000 multi-block reads; a 10-minute soak wrote
  3.7 GB and read 4.2 GB. Reads reach 23.6 MB/s, writes 18.7 MB/s.
  Without the fix, every test that repeated multi-block reads failed.
- **A 2 GB card, with all six patches** (22 October 2026,
  [log](docs/reviews/logs/2026-10-22-sd-qual-pi3a-sd2gb-run11.log)):
  **every test passed**, soak included. Zephyr's SD stack had left this
  card's bus 1 bit wide (3.1 MB/s); with `sdmmc-4bit-without-hs.patch` it
  runs at 4 bits, 12.0 MB/s reads and 10.5 MB/s writes. Its writes stay
  busy for up to 359 ms, which failed them before
  `sdhost-busy-timeouts.patch` (finding 1). The twelfth run (29 October
  2026, [log](docs/reviews/logs/2026-10-29-sd-qual-pi3a-sd2gb-run12.log))
  was the first of the restructured test suites, and the thirteenth
  ([log](docs/reviews/logs/2026-10-29-sd-qual-pi3a-sd2gb-run13.log)) the
  first on Zephyr v4.5.0: every test passed in both, with busy of up to
  375 ms (of the 500 ms allowed), and a lower-priority thread still never
  ran during transfers (finding 6).
- **A 32 GB High Speed card on v4.5.0** (29 October 2026, fourteenth run,
  [log](docs/reviews/logs/2026-10-29-sd-qual-pi3a-sd32gb-run14.log)):
  everything passed except finding 4's check (a 314 ms data timeout at
  50 MHz, harmless on this card, whose slowest write took 14.5 ms). Reads
  reach 24.0 MB/s, writes 22.6 MB/s; the soak wrote 3.3 GB and read 3.9 GB.
- **Zephyr's own suites** on the same Pi and card: `sdhc` 5/5,
  `disk_access` 3/3.
- **Upstream:** the SD stack's range-check fix is Zephyr PR
  [#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216)
  (`sd: check read and write ranges for overflow`, amended on 29 and 30
  October 2026 to share one range check between read, write and erase),
  open and
  awaiting review; the rest isn't posted yet
  ([UPSTREAM.md](docs/UPSTREAM.md)). Zephyr is pinned to the v4.5.0
  release.

## Layout

| Path | What |
|---|---|
| `tests/sdcard_hw` | Qualification app for a real Pi 3A+: data integrity, sizes, bounds, concurrency, speed, soak ([TESTING.md](docs/TESTING.md)) |
| `tests/sdhost_hw` | Reproduces the review's findings one by one on a real Pi 3A+ |
| `tests/sdhost_review` | The driver against a register model of the controller (`native_sim`): findings 1-10 and 12, and the fix |
| `tests/sd_bounds` | Zephyr's SD stack range checks against a fake controller (`native_sim`), including ranges that wrap |
| `tests/sd_bus_width` | Zephyr's SD stack initialising a fake card (`native_sim`): 4 bits with or without High Speed |
| `tests/hal_bcm2835` | Unit tests of the HAL headers |
| `hal/include/hal` | Register access for the tests (SDHost, GPIO) |
| `boards/raspberrypi/rpi_3a_plus` | The Pi 3A+ board, with the SDHost node |
| `zephyr/patches.yml`, `zephyr/patches/` | PR #110196 and the fixes (the driver, the firmware mailbox, and the SD stack's range check and bus width), applied by `west patch apply` |
| `qemu/`, `scripts/run-qemu.sh` | Running the tests on QEMU's `raspi3ap` |
| `scripts/check-*.py`, `cmake/strict.cmake` | The coding rules' checks ([CODING.md](docs/CODING.md), [CLAUDE.md](CLAUDE.md)): headers, docs, function size, global state and macro arguments, and the strict build |
| `docs/` | [TESTING.md](docs/TESTING.md), [UPSTREAM.md](docs/UPSTREAM.md), [CODING.md](docs/CODING.md), the [review](docs/reviews/pr110196-sdhost-review.md) and hardware logs |

## Quick start

Everything runs in a Docker image with the pinned Zephyr, the patches
applied, the toolchain and QEMU:

```sh
make image              # once, and after any change to the patches
make test               # no hardware: model tests, HAL tests, code checks
make sd-test            # Zephyr's SD tests on the emulated Pi 3A+
make sd-qual-test       # out/sd-qual-test/sdcard.img for a real Pi 3A+
make sd-hw-test         # out/sd-hw-test/sdcard.img: the review's findings
make sd-zephyr-hw-test  # Zephyr's sdhc and disk_access tests, one image each
```

The hardware tests write a card image: write it to a **spare** card, boot
the Pi with a serial console on GPIO14/15 (115200 8N1), and save the log.
[TESTING.md](docs/TESTING.md) has the details and how to read the results.

Without Docker, `west init -l` in this repository fetches the pinned
Zephyr; `west update` and `west patch apply` then prepare it.

## Licence

Apache-2.0, as Zephyr: see [LICENSE](LICENSE). Each file carries its SPDX identifier.
