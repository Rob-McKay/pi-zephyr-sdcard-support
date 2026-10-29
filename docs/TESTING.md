# SD card qualification on a real Pi 3A+

`make sd-qual-test` builds a test application,
[`tests/sdcard_hw`](../tests/sdcard_hw), that tests the SD card driver from
Zephyr [PR #110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196)
on a real Raspberry Pi 3A+ and card, as thoroughly as can be done without
special equipment.

It goes through Zephyr's disk access API (`disk_access_read()`,
`_write()`, `_erase()`, `_ioctl()` on disk `"SD"`), the same path a file
system would use. The SD stack and the driver run unmodified;
the only change is that the driver is built with
`-fsanitize=signed-integer-overflow`, and every test fails if that fires.

It complements `make sd-hw-test` ([`tests/sdhost_hw`](../tests/sdhost_hw)).
That test sends raw requests to the driver to reproduce the
[code review's findings](reviews/pr110196-sdhost-review.md) one by one.
This one asks whether the card works: right data, every size, every
address, under load, for a long time.

## What you need

- A Raspberry Pi 3A+.
- A **spare** microSD card of at least 128 MB, best a few you can compare:
  an SDHC card (32 GB or less) and an SDXC card (over 32 GB), of different
  brands. **Its contents are destroyed**: flashing replaces the partition
  table, and the tests write the last 64 MB of the card.
- A 3.3 V USB serial adapter on header pins 8 (TX, GPIO 14), 10 (RX,
  GPIO 15) and 6 (GND), and a terminal that logs, e.g. `picocom` or
  `screen`.

## Running it

1. `make image` once, if you haven't since pulling (the image applies the
   driver patch).
2. `make sd-qual-test`. It writes `out/sd-qual-test/sdcard.img`: the test
   firmware, the Pi's boot files and `zephyr.crc`, the CRC-32 of
   `zephyr.bin` as the host computed it.
3. Either **flash the image** to the card, with, e.g., Raspberry Pi
   Imager ("Use custom"), or `sudo dd if=out/sd-qual-test/sdcard.img
   of=/dev/sdX bs=4M conv=fsync`; or **copy the files** in
   `out/sd-qual-test/boot/` onto a card formatted as one FAT32 partition
   (as a PC or the SD Association's formatter makes it), with its last
   64 MB free. A Raspberry Pi OS card won't do: its Linux partition
   fills the end of the card, so the write tests are skipped.
4. Open the serial port at 115200 baud and log it, e.g. `picocom -b 115200
   /dev/ttyUSB0 | tee sd-qual-$(date +%F)-<card>.log`.
5. Put the card in the Pi and power it on. The run ends with `PROJECT
   EXECUTION SUCCESSFUL` or `PROJECT EXECUTION FAILED`. It takes about 15
   minutes with the default 10-minute soak.

Options (make variables):

| Variable | Default | Effect |
|---|---|---|
| `SD_QUAL_SOAK_SECONDS` | 600 | Length of the soak (suite 9); 0 skips it. For a release, run an hour or overnight |
| `SD_QUAL_SCRATCH_MB` | 64 | Size of the area at the end of the card the write tests use (64-1024) |
| `SD_QUAL_SEED` | from the clock | Random seed. Each run prints the seed it used; set it to repeat a failure exactly |
| `RPI_CORE_FREQ` | 250 | VPU core clock in MHz, which the SD clock is divided from. At 400 the console runs at 184320 baud |
| `SD_QUAL_TEST_FLAGS=--qemu` | | Also run it on QEMU's emulated Pi 3A+ with a 256 MB card, as a smoke test |

`make sd-qual-test SD_QUAL_TEST_FLAGS=--qemu` passes on QEMU. That shows the
tests themselves work, not the driver: QEMU completes transfers at once,
and doesn't model the clock, timeout and bus width registers (suite 1
skips those checks there).

## Safety

The tests write only the scratch area, the last `SD_QUAL_SCRATCH_MB` of
the card, and only where no file can be:

- no partition reaches it (the flashed image has one 64 MB partition, so
  on a real card that area is free); or
- the only partition reaching it is FAT32, and the test reads its FAT and
  finds every cluster there free. FAT32 never reads a free cluster, so
  the files and the file system are untouched. On QEMU, a 256 MB FAT32
  card went through the whole run and `fsck.vfat` found it clean, with
  every file unchanged.

Otherwise (a Linux or exFAT partition, or files at the end of a FAT32
one) the write tests are skipped and say why; the read-only tests still
run. Use a spare card anyway: a driver bug could write somewhere else.

## What it tests

The suites run in this order. Every test starts by checking the card still
answers (status, and block 0 as it read at the start). If not, it prints a
`RECOVERY:` line and power-cycles the card, so one failure doesn't fail
every later test. If that doesn't work it prints `RECOVERY FAILED` and the
test runs anyway, and fails on the card. (An assertion there would show
as SKIP: ztest reports a failure in a test's setup as a skip.) The
summary suite at the end fails if any recovery was needed.

| Suite | Tests | Checks | Review finding |
|---|---|---|---|
| 1 identify | `test_disk_ready`, `test_ioctls` | The disk is OK, has 512-byte blocks and the whole card's capacity; the sync and erase-size ioctls answer | |
| | `test_bus_4bit` | The bus is 4 bits wide. Every SD memory card supports 4 bits; unpatched, Zephyr's SD stack widened the bus only for High Speed cards, so a Default Speed card failed this (ninth run, 2 GB card). `sdmmc-4bit-without-hs.patch` fixes it | |
| | `test_bus_clock_in_spec` | The real SD clock (firmware core clock / divider) is between 400 kHz and 50 MHz | 3 |
| | `test_data_timeout_covers_busy` | The hardware data timeout is at least 500 ms, an SDXC card's write busy limit | 4 |
| 2 bootfs (read-only) | `test_1_layout`, `test_2_config_txt` | The MBR and FAT32 boot partition parse (walked by hand, no file system library); `config.txt` reads as the firmware booted from it | |
| | `test_3_files_read_consistently` | Every file in the root directory reads the same a cluster per request, a block per request, and into a misaligned buffer | |
| | `test_5_multi_block_read_stress` | 1000 multi-block reads (2-256 blocks) at random places in the partition's first 1 MB match a block-at-a-time copy: the CMD18 + CMD12 stop works every time | 12 |
| | `test_4_image_crc_matches_host` | `zephyr.bin`'s CRC-32 matches the host's (`zephyr.crc`): a few hundred KB read exactly as written | |
| 3 integrity | `test_1_patterns` | Zeros, ones, checkerboard, walking ones and zeros, counting and random bytes round-trip | |
| | `test_2_transfer_sizes` | 30 transfer sizes from 1 block to 1 MB, either side of each power of two | |
| | `test_3_chunking` | One big write reads back in 1-, 7- and 64-block pieces, and many 1-block writes read back in one | |
| | `test_4_misaligned_buffers` | Write and read buffers at every byte offset 0-7; nothing written outside the buffer | |
| | `test_5_neighbours_untouched` | A write changes only its own blocks | |
| | `test_6_overwrite` | Rewrites return the newest data | |
| | `test_7_address_bits` | Blocks a power of two apart (1, 2, 4, ... blocks) and the card's last block keep distinct data: no address bit stuck or aliased | |
| | `test_8_random_mix` | 1000 random writes and reads (1-256 blocks) in an 8 MB region, checked against a shadow copy, then the whole region | 2 |
| 4 bounds | `test_1_first_blocks`, `test_2_last_blocks` | Blocks 0-63 read the same twice; the card's last block and last 64 blocks read and write | |
| | `test_3_past_the_end_refused` | Reads and writes past the end are refused (-EINVAL) before reaching the card, and the card still works after | |
| | `test_4_wrapping_range_refused` | A read of block 0xffffffff is refused too. Unpatched, Zephyr's SD stack checks `start + count`, which wraps, so the request reaches the card (not the driver's bug); `sd-ops-bounds-overflow.patch` fixes it (sent to Zephyr as [#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216)), and `tests/sd_bounds` checks the same without hardware. Failed in runs up to the eighth, which predate the patch; passed in the ninth, on two cards | |
| | `test_5_whole_range_sampled` | One block per MB across the whole card reads | |
| 5 erase | `test_1_small_erase_exact`, `test_2_single_block_then_write` | Erased blocks read as all 0x00 or all 0xff; their neighbours keep their data; writing after an erase works | |
| | `test_3_large_erase` | An 8 MB erase, which holds the card busy, completes (its time is printed) | 1 |
| 6 perf | `test_1_sequential_read`, `test_2_sequential_write` | kB/s at 1, 8, 64, 256 and 2048 blocks a request; multi-block reads must reach 1 MB/s | 6 |
| | `test_3_random_read`, `test_4_random_write` | 500 random 4 KB reads over the whole card, and 500 writes over the scratch area: operations/s and min/avg/max latency; every write verified | 1 |
| 7 concurrency | `test_1_writers_and_reader` | Three writers, each verifying its own 1 MB region, and a reader re-checking blocks 0-63, at once; a lower-priority thread counts how often it gets the CPU | 6, 9 |
| 8 recovery | `test_1_power_cycles`, `test_2_write_then_power_off` | 20 power-off/init cycles through the disk API, and power-offs straight after writes: the data survives | |
| | `test_3_unanswered_command` | After a command the card doesn't answer (Zephyr's CID ioctl sends CMD2 in transfer state), the card still reads | 2 |
| 9 soak | `test_1_random_workload` | Random writes (45%), reads (52%) and erases (3%) of 1 block to 1 MB anywhere in the scratch area, every read checked, for `SD_QUAL_SOAK_SECONDS`; progress every 30 s | 1, 2 |
| | `test_2_final_verify` | The whole scratch area holds what the soak last wrote | |
| zz summary | `test_no_recovery_needed` | No test left the card unusable during the whole run (named to run last) | 2 |

## Zephyr's own tests, as the PR reports them

PR #110196 reports two of Zephyr's test suites passing on a Pi Zero 2 W:
`tests/drivers/sdhc` (5/5) and `tests/drivers/disk/disk_access` (3/3).
`make sd-zephyr-hw-test` builds the same two, unmodified, for the Pi 3A+,
with this repository's board and toolchain, as one card image each:

| Image | Tests | What they do |
|---|---|---|
| `out/sd-zephyr-hw-test/drivers_sdhc/sdcard.img` | `test_card_presence`, `test_host_props`, `test_reset`, `test_set_io`, `test_card_if_cond` | The host controller API directly: card detect, the host's properties, a reset, bus settings (clock, width, power), and CMD8 to a card |
| `out/sd-zephyr-hw-test/drivers_disk_disk_access/sdcard.img` | `test_read`, `test_write`, `test_erase` | The disk layer: reads, writes and erases of 1, 8, 29 and 31 blocks at the card's first block, its middle and its last block, plus a request past the end |

Run each like the qualification image: flash it, boot, log the console.
`make sd-test` runs the same suites on QEMU.

**These are destructive, more so than the qualification.** `disk_access`
writes and erases block 0, the partition table, and blocks in the middle
of the card: after it runs, the card no longer boots and anything on it
is damaged. Use a spare card, and flash it again before every run.

The comparison with the PR's results is the point: `disk_access` makes
multi-block reads of up to 31 blocks, the kind that hit review finding 12
on the 3A+. A pass here and a failure in `make sd-qual-test`'s stress test
would mean the race needs more reads, or more load, to show.

## Results so far

Four runs on 20 October 2026 (a 64 GB SDXC card; the fourth with writes)
showed writes, data integrity and erase working, and reproduced
review findings 2, 4 and 6 and found a new one, 12: about one open-ended
multi-block read in a few hundred fails at 50 MHz, and the controller then
stays wedged for about 12 s. Zephyr's own `sdhc` and `disk_access`
suites pass on the same Pi and card (5/5, 3/3), as the PR reports for a
Zero 2 W: they make too few multi-block reads to hit the race. See the review's
[section 7](reviews/pr110196-sdhost-review.md#7-results-on-a-real-pi-3a).

A fix for finding 12 is now carried
(`zephyr/patches/zephyr/sdhost-stop-open-read.patch`). With it, a fifth
run passed everything except finding 4's check, including the 10-minute
soak (3.5 GB written, 4.2 GB read, no error). The driver logs
`read ran on past its last block: stopped, N words discarded, T ms`
whenever it stops a read that ran on, which should be rare: the eighth
run, with the corrected patch, logged none and passed everything but
finding 4's check. (A sixth run with a first version of the patch showed
it could lose a CRC error on a read's last block; see the review.)

On a 2 GB card with all six patches, every test has passed: first in the
eleventh run (22 October 2026), then in the twelfth (29 October 2026), the
first with the suites sharing `struct sdq` and the race-free checks in
suite 7. The twelfth run's random 4 KB writes stayed busy for up to
370 ms after CMD12, within the 500 ms the driver now allows (finding 1),
and suite 7's lower-priority thread ran 0 times (finding 6).

## Reading the results

ztest's per-test durations wrap every 223.7 s (it uses the 32-bit cycle
counter, at 19.2 MHz), so a 600 s soak shows as 152.6 s. The soak's own
progress lines are right.

- **PASS:** the check held on this Pi and card.
- **FAIL:** the line before says what went wrong. A data failure prints
  what the block held instead: another block's data (a misdirected
  transfer), an older generation (a lost write), erased, or unknown data.
  Note the seed so the run can be repeated.
- **SKIP:** a precondition wasn't met, e.g. no scratch area; the line
  before says why.
- **`RECOVERY:`** lines mean a test left the card unusable and the harness
  power-cycled it. If emptying the controller's FIFO was needed, that is
  review finding 2.
- The numbers (bus clock, kB/s, latency, erase time, the lower-priority
  thread's count) are for the report; they depend on the card.

Failures in suite 1's clock and timeout checks, or in suite 5's large
erase, are expected while the review's findings 1, 3 and 4 are open: they
are those findings, measured through the normal API.

## Reporting

For each card, keep the whole serial log and note the card's make, model,
capacity and speed class, the Pi model, `RPI_CORE_FREQ`, the soak length
and the seed. A summary for the PR could be:

```
Pi 3A+ (BCM2837B0), core_freq 250, Zephyr <commit> + PR #110196 @ b3b4961
Card: <brand model, 32 GB SDHC, class 10 / U1>
make sd-qual-test, 10 min soak, seed <n>: <n> passed, <n> failed, <n> skipped
Bus: 4-bit, <n> Hz; SDTOUT <n> ms
Sequential read/write (1 MB requests): <n> / <n> kB/s
Random 4 KB read/write: <n> / <n> ops/s, max latency <n> / <n> ms
8 MB erase: <n> ms
Failures: <test: message>
```

Post results upstream only with the repository owner's go-ahead (see
[UPSTREAM.md](UPSTREAM.md)).

## Not covered

These need a person or equipment, so the app doesn't attempt them:

- **Card removal and insertion while running.** The 3A+ slot has no
  card-detect line, so the driver can't notice; pulling the card mid-write
  tests error recovery by hand.
- **Power loss during a write.** Needs a switchable supply; the data in
  flight may be lost, but the card should still initialise.
- **Temperature and supply voltage extremes.**
- **UHS speeds.** The SDHost runs at 3.3 V only, so Default and High Speed
  are all there is.
