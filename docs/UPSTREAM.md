# Upstream Zephyr changes this repository carries

The SD card driver under test is not in mainline Zephyr yet. It is
[PR #110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196), part
of the Raspberry Pi series (RFC
[#109880](https://github.com/zephyrproject-rtos/zephyr/issues/109880)). This
repository carries it, and its own fixes, as patches on the pinned Zephyr.

## What is carried

`zephyr/patches.yml` lists the patches, in the order they apply.
`west patch apply` applies them to the pinned Zephyr (`west.yml`), and the
Docker image does it when it is built. So **after any change to the
patches, run `make image`**.

| Patch | Pull request | What it does | State when last refreshed |
|---|---|---|---|
| `pr110196-bcm2835-sdhost.patch` | [#110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196) SDHost (SD card) driver | The driver under test | Approved, milestone v4.6.0. Reviewed here: [reviews/pr110196-sdhost-review.md](reviews/pr110196-sdhost-review.md) |
| `sdhost-stop-open-read.patch` | none yet (on top of #110196) | Stops a multi-block read that runs on, and a failed one: review findings 12, and 2 for reads | Found and confirmed on a real Pi 3A+ (eighth run: every test passed, no run-ons in over 100,000 multi-block reads). Not yet sent upstream |
| `sdhost-busy-timeouts.patch` | none yet (on top of #110196 and the patch above) | Waits as long as the spec allows for a card's busy after a write's CMD12 (at least 500 ms) and after other busy commands (500 ms; 10 s for an erase): review finding 1 | Found on a real Pi 3A+ (tenth run, a 2 GB card: writes and an erase reported as failed although they succeeded). Tested by `tests/sdhost_review`, and confirmed on the Pi (eleventh run: every write passed, with busy of up to 359 ms). Not yet sent upstream |
| `rpi-fw-bus-address.patch` | none yet | Firmware mailbox on a real Pi 3A+, which the tests use to read the core clock | Not yet sent upstream |
| `sd-ops-bounds-overflow.patch` | [#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216) `sd: sd_ops: make read/write range check overflow-aware` | Zephyr's SD stack refuses read and write ranges that wrap past block 0xffffffff (`subsys/sd/sd_ops.c`); not the SDHost driver | Still in Zephyr main on 21 October 2026. Tested by `tests/sd_bounds` (`make test`), and passed on a real Pi 3A+ with two cards (ninth run). **Sent upstream** 22 October 2026 by the repository owner: open, awaiting review (danieldegrasse, JordanYates, jfischer-no), area "Disk Access". The PR has the fix only; the matching Zephyr test is ready in [`upstream/`](../upstream/0001-tests-sd-add-test-for-read-write-range-checks.patch). Independent of #110196 |
| `sdmmc-4bit-without-hs.patch` | none yet | Zephyr's SD stack widens the bus to 4 bits on cards without High Speed too (`subsys/sd/sdmmc.c`, `sdmmc_init_hs()`); every SD memory card supports 4 bits (PLS v9.10 §5.6). Not the SDHost driver | Still in Zephyr main on 21 October 2026. Found on a 2 GB card on the Pi (ninth run); tested by `tests/sd_bus_width` (`make test`), and confirmed on the Pi (eleventh run: 4 bits, 12.0 MB/s reads instead of 3.1). Not yet sent upstream; independent of #110196 |

The board support PR of the same series,
[#110189](https://github.com/zephyrproject-rtos/zephyr/pull/110189), has
already merged. This repository's `rpi_3a_plus` board adds the SDHost node
the driver needs.

## How long until they merge

Zephyr publishes no merge-time figure. A new driver for hardware Zephyr
already supports typically takes a few weeks to two or three months; a
small fix a few days to two weeks. Approvals from each area's maintainers,
the four-monthly release cycle and its feature freeze, and testing on real
hardware set the pace. #110196 is approved for v4.6.0 and should land with
it.

## Keeping the patches current

- **#110196 changes:** regenerate its patch from the PR's head on the
  pinned Zephyr, then check `sdhost-stop-open-read.patch` still applies
  on top (regenerate it if not). Update each `sha256sum`, run
  `make image`, then `make test`, `make sd-test` and the hardware tests.
- **#110196 merges:** move the `west.yml` pin to a Zephyr commit that
  includes it, delete its entry and patch file, and check the rest still
  apply. If the fix went in with it, delete that too.
- **A fix is sent upstream:** add its `merge-pr` to `patches.yml`, and
  follow the review there. If the review changes the fix, regenerate the
  patch to match.
- **A fix merges:** once the `west.yml` pin includes it, delete its entry
  and patch file (the range check: when #123216 merges).

## Helping them merge

Results from real hardware are the most useful contribution:
`make sd-qual-test`, `make sd-hw-test` and `make sd-zephyr-hw-test`
([TESTING.md](TESTING.md)). The review's
[section 5](reviews/pr110196-sdhost-review.md#5-draft-upstream-comment)
has a draft comment for the PR.

The SD stack's range-check fix is upstream as
[#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216) (22 October 2026). Nothing else from this repository has
been posted: the review, the hardware results and the driver fixes go to
#110196, and the bus-width fix to its own PR, only with the repository
owner's go-ahead.
