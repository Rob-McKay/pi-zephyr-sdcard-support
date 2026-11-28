# Upstream Zephyr changes this repository carries

The SD card driver under test,
[PR #110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196), part
of the Raspberry Pi series (RFC
[#109880](https://github.com/zephyrproject-rtos/zephyr/issues/109880)),
**merged into Zephyr main on 22 November 2026** as `dde0e0565b0`, the same
as the PR head this repository tested. It merged without this
repository's review findings or fixes. This repository carries its own
fixes as patches on the pinned Zephyr: a main-branch commit of the v4.6
cycle (`06b50e7c66d2`, 28 November 2026). Release branches only get bug
fixes, so the Raspberry Pi drivers can only come from main; the pin moves
to the v4.6.0 tag once that is released (scheduled for April 2027).

## What is carried

`zephyr/patches.yml` lists the patches, in the order they apply.
`west patch apply` applies them to the pinned Zephyr (`west.yml`), and the
Docker image does it when it is built. So **after any change to the
patches, run `make image`**.

| Patch | Pull request | What it does | State when last refreshed |
|---|---|---|---|
| `sdhost-stop-open-read.patch` | none yet (on top of the driver) | Stops a multi-block read that runs on, and a failed one: review findings 12, and 2 for reads | Found and confirmed on a real Pi 3A+ (eighth run: every test passed, no run-ons in over 100,000 multi-block reads). Not yet sent upstream |
| `sdhost-busy-timeouts.patch` | none yet (on top of the driver and the patch above) | Waits as long as the spec allows for a card's busy after a write's CMD12 (at least 500 ms) and after other busy commands (500 ms; 10 s for an erase): review finding 1 | Found on a real Pi 3A+ (tenth run, a 2 GB card: writes and an erase reported as failed although they succeeded). Tested by `tests/sdhost_review`, and confirmed on the Pi (eleventh run: every write passed, with busy of up to 359 ms). Not yet sent upstream |
| `sdhost-data-timeout.patch` | none yet (on top of the driver and the patches above) | Reprograms the controller's data timeout (`SDTOUT`, counted in bus clock cycles) for 1 s whenever `set_io()` sets the clock: review finding 4 | Seen on a real Pi 3A+ at 50 MHz on two cards (314 ms; the 64 GB SDXC card in runs 4-9, a 32 GB card in run 14). Tested by `tests/sdhost_review`; not yet run on the Pi. Not yet sent upstream |
| `rpi-fw-bus-address.patch` | none yet | Gives the VideoCore the shared buffer's bus address (through the mailbox bus's `dma-ranges`, 0xC0000000 on BCM2837), not its ARM physical address, and makes the reply check compare against that address. Without it the firmware never answers on a real Pi 3A+; the tests read the core clock through it | Main's `f395ab2221e` (11 November 2026) took this patch's earlier other half, a 1 s reply timeout, and added a check that a reply carries the buffer's address, the physical one; this patch now changes that check too, or every reply would be rejected. Works on QEMU on the new pin (the core clock is read); not yet run on the Pi in this form. Not yet sent upstream |

### No longer carried

Each is in the pinned Zephyr, so its patch was removed: the range check
and the 4-bit bus on 19 November 2026, when the pin moved from v4.5.0 to
main, and the driver itself on 25 November 2026.

| Patch | Pull request | What it did | State when last carried |
|---|---|---|---|
| `pr110196-bcm2835-sdhost.patch` | [#110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196) SDHost (SD card) driver | The driver under test | **Merged into Zephyr main on 22 November 2026** as `dde0e0565b0` (committed by Anas Nashif), identical to the PR's last head `94b76f2d1684` (19 November 2026). That head, rebased onto main, added: a build-time check that `bus-width` is 1 or 4, and the binding now requires `bus-width` and leaves its allowed values to main's shared `sdhc.yaml`; no change in behaviour, and none of this repository's fixes. The patch was that commit as posted, on the main commit it was based on. Reviewed here: [reviews/pr110196-sdhost-review.md](reviews/pr110196-sdhost-review.md) |
| `sd-ops-bounds-overflow.patch` | [#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216) `sd: check read and write ranges for overflow` | Zephyr's SD stack refuses read and write ranges that wrap past block 0xffffffff (`subsys/sd/sd_ops.c`); not the SDHost driver | **Merged into Zephyr main on 17 November 2026** as `f513d993a397` (committed by Anas Nashif), the same change as this patch, and in the pin since 19 November. Tested by `tests/sd_bounds` (`make test`), and passed on a real Pi 3A+ with two cards (ninth run). **Sent upstream** 22 October 2026 by the repository owner: open, awaiting review (danieldegrasse, JordanYates, jfischer-no), area "Disk Access". Its commit was amended on 27 October to pass Zephyr's commit check (an `area:` title under 75 characters, and a body), on 29 October to move the check into a helper that read, write and erase share, and on 30 October (head `2748a5f8d2e3`) to rename it `card_block_range_valid()`, wrap its declaration within checkpatch's 100 columns and correct the commit message; this patch is that head's change. Passed on a real Pi 3A+ in its 29 October form (thirteenth and fourteenth runs); the 30 October changes don't change what it does. The PR had the fix only; the matching Zephyr test is ready in [`upstream/`](../upstream/0001-tests-sd-add-test-for-read-write-range-checks.patch) as a follow-up PR of its own. Independent of #110196 |
| `sdmmc-4bit-without-hs.patch` | none yet | Zephyr's SD stack widens the bus to 4 bits on cards without High Speed too (`subsys/sd/sdmmc.c`, `sdmmc_init_hs()`); every SD memory card supports 4 bits (PLS v9.10 §5.6). Not the SDHost driver | Fixed in Zephyr main on 4 November 2026 by `a8937199708`, independently, and in the pin since 19 November. Found on a 2 GB card on the Pi (ninth run); tested by `tests/sd_bus_width` (`make test`), and confirmed on the Pi (eleventh run: 4 bits, 12.0 MB/s reads instead of 3.1). Not yet sent upstream; independent of #110196 |

Main fixed the 4-bit bus itself in `a8937199708` ("negotiate bus
width independently of high speed", 4 November 2026): it moves bus-width
negotiation out of `sdmmc_init_hs()` rather than widening the bus there.
`tests/sd_bus_width` passes against it; the 2 GB card's `test_bus_4bit`
is the hardware check still to run.

The board support PR of the same series,
[#110189](https://github.com/zephyrproject-rtos/zephyr/pull/110189), has
already merged. This repository's `rpi_3a_plus` board adds the SDHost node
the driver needs.

## How long until they merge

Zephyr publishes no merge-time figure. A new driver for hardware Zephyr
already supports typically takes a few weeks to two or three months; a
small fix a few days to two weeks. Approvals from each area's maintainers,
the four-monthly release cycle and its feature freeze, and testing on real
hardware set the pace. #110196 merged on 22 November 2026, for v4.6.0,
about six months after it was opened (its commit is dated 26 May).

## Keeping the patches current

- **Moving the pin:** set `west.yml` to the new main commit, check the
  patches still apply on it (regenerate them if not), update each
  `sha256sum`, run `make image`, then `make test`, `make sd-test` and the
  hardware tests. If the driver has changed upstream, check whether that
  covers any of the fixes here.
- **A fix is sent upstream:** add its `merge-pr` to `patches.yml`, and
  follow the review there. If the review changes the fix, regenerate the
  patch to match.
- **A fix merges:** once the `west.yml` pin includes it, delete its entry
  and patch file, and move it to *No longer carried* above.
- **v4.6.0 is released** (scheduled for April 2027): move the pin to the
  tag, as for any pin move.

## Helping them merge

Results from real hardware are the most useful contribution:
`make sd-qual-test`, `make sd-hw-test` and `make sd-zephyr-hw-test`
([TESTING.md](TESTING.md)). With #110196 merged, the review's findings
and the driver fixes would go upstream as issues or pull requests
against the driver in main, not as a comment on the PR; the review's
[section 5](reviews/pr110196-sdhost-review.md#5-draft-upstream-comment)
has a draft comment written for the PR, which would need recasting.

The SD stack's range-check fix went upstream as
[#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216) (22 October 2026) and merged on 17 November 2026. Its test, `tests/subsys/sd/bounds`, is ready as a follow-up in [`upstream/`](../upstream/README.md), not yet posted. Nothing else from this repository has
been posted, and nothing is, without the repository owner's go-ahead.
(Main fixed the 4-bit bus itself, so that fix no longer needs posting.)
