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
  Findings 2, 4 and 6 reproduced on hardware, and 12 was found there.
- **Fix:** `zephyr/patches/zephyr/sdhost-stop-open-read.patch` stops a
  multi-block read that runs on into the next block (finding 12), and a
  failed one (finding 2 for reads).
- **On a real Pi 3A+ with a 64 GB SDXC card, with the fix** (21 October
  2026, [log](docs/reviews/logs/2026-10-21-sd-qual-pi3a-sdxc64-run8-corrected.log)):
  every qualification test passed except finding 4 (a 314 ms data timeout)
  and a wrapping bounds check in Zephyr's SD stack, since fixed here
  (`sd-ops-bounds-overflow.patch`; it passed on the Pi in the
  [ninth run](docs/reviews/pr110196-sdhost-review.md#7-results-on-a-real-pi-3a)).
  No run-ons in over 100,000 multi-block reads; a 10-minute soak wrote
  3.7 GB and read 4.2 GB. Reads reach 23.6 MB/s, writes 18.7 MB/s.
  Without the fix, every test that repeated multi-block reads failed.
- **A 2 GB card** (ninth run, reads only): reads all good, but Zephyr's SD
  stack left the bus 1 bit wide (3.1 MB/s), because it only widens the bus
  for High Speed cards. Not the driver's bug; not fixed yet.
- **Zephyr's own suites** on the same Pi and card: `sdhc` 5/5,
  `disk_access` 3/3.
- **Not posted upstream** yet ([UPSTREAM.md](docs/UPSTREAM.md)).

## Layout

| Path | What |
|---|---|
| `tests/sdcard_hw` | Qualification app for a real Pi 3A+: data integrity, sizes, bounds, concurrency, speed, soak ([TESTING.md](docs/TESTING.md)) |
| `tests/sdhost_hw` | Reproduces the review's findings one by one on a real Pi 3A+ |
| `tests/sdhost_review` | The driver against a register model of the controller (`native_sim`): findings 1-10 and 12, and the fix |
| `tests/sd_bounds` | Zephyr's SD stack range checks against a fake controller (`native_sim`), including ranges that wrap |
| `tests/hal_bcm2835` | Unit tests of the HAL headers |
| `hal/include/hal` | Register access for the tests (SDHost, GPIO) |
| `boards/raspberrypi/rpi_3a_plus` | The Pi 3A+ board, with the SDHost node |
| `zephyr/patches.yml`, `zephyr/patches/` | PR #110196 and the fixes (the driver, the firmware mailbox and the SD stack's range check), applied by `west patch apply` |
| `qemu/`, `scripts/run-qemu.sh` | Running the tests on QEMU's `raspi3ap` |
| `docs/` | [TESTING.md](docs/TESTING.md), [UPSTREAM.md](docs/UPSTREAM.md), [CODING.md](docs/CODING.md), the [review](docs/reviews/pr110196-sdhost-review.md) and hardware logs |

## Quick start

Everything runs in a Docker image with the pinned Zephyr, the patches
applied, the toolchain and QEMU:

```sh
make image              # once, and after any change to the patches
make test               # no hardware: model tests, HAL tests, doc check
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
