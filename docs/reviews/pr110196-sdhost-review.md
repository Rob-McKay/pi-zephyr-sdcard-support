# Review: Zephyr PR #110196, BCM283x SDHost driver

| | |
|---|---|
| PR | [zephyrproject-rtos/zephyr#110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196), "drivers: sdhc: add Broadcom BCM283x SDHost controller driver" (Jonathan E. Peace) |
| Commit reviewed | `b3b4961`, as carried in [`zephyr/patches/zephyr/pr110196-bcm2835-sdhost.patch`](../../zephyr/patches/zephyr/pr110196-bcm2835-sdhost.patch) |
| Date | 17 October 2026 |
| Focus | Undefined behaviour (UB), implementation-defined behaviour (IB), race conditions, security |
| Method | Code reading of `drivers/sdhc/sdhc_bcm2835_sdhost.c` (all line numbers below), its devicetree and binding. Callers checked against the pinned Zephyr (`subsys/sd`, `include/zephyr/drivers/sdhc.h`, `drivers/pinctrl/pinctrl_bcm2711.c`). Zephyr's SD tests pass on QEMU (`make sd-test`), but QEMU completes transfers instantly and doesn't model the controller's timing. **On a real Pi 3A+ (20 October 2026):** findings 2, 4 and 6 reproduced, and a new one, 12, found: see [section 7](#7-results-on-a-real-pi-3a) |
| Status | **Not posted upstream.** [Section 5](#5-draft-upstream-comment) has a draft |
| Tests | On a real Pi 3A+ and card: [`tests/sdhost_hw`](../../tests/sdhost_hw) (`make sd-hw-test`), see [section 6.1](#61-on-a-real-pi-3a). Without hardware: findings 1-10 are reproduced against a register model by [`tests/sdhost_review`](../../tests/sdhost_review) (part of `make test`), see [section 6.2](#62-against-a-model-of-the-controller) |

## 1. Summary

The driver has **no memory-safety bugs**:

- Every buffer access is bounded by the caller's `blocks × block_size`.
- Nothing the card sends is used as a length or index.
- Unaligned buffers are packed byte by byte, so there are no unaligned-access UB or aliasing issues.
- All register and bit arithmetic is on unsigned types: no signed shifts, and no IB from shifts or sign conversion there.

The problems are in **timeouts and error recovery**. They are likely to show on real cards (slow writes, SDXC cards, a card removed mid-transfer) and can't show in QEMU.

| # | Severity | Area | Finding |
|---|---|---|---|
| 1 | High | Correctness | CMD12 after a multi-block write waits only 100 ms for busy |
| 2 | High | Robustness, integrity | No recovery after an error or timeout; stale FIFO data can reach the next read |
| 3 | High (conditional) | Correctness | Input clock fixed at 250 MHz in devicetree; SD bus overclocked if the core clock is higher |
| 4 | Medium | Correctness | `SDTOUT` never reprogrammed for the bus clock |
| 5 | Medium | UB / IB | `SDHC_TIMEOUT_FOREVER` (-1) mishandled; `timeout_ms * 1000` signed overflow |
| 6 | Medium | Availability | Busy-waits for whole transfers (up to 10 s) without yielding |
| 7 | Medium | Dependency | `bias-pull-up` not applied on BCM2710 by the current pinctrl driver |
| 8 | Low | Robustness | FIFO fill level not clamped |
| 9 | Low | Races | No internal locking; relies on callers to serialise |
| 10 | Low | API | Block size and count not validated against hardware limits |
| 11 | Info | API | Clock 0 can't stop the clock (no gate); `f_min` is fine (corrected) |
| 12 | High | Correctness, robustness | Stopping an open-ended multi-block read depends on winning a race against the card; lost on real hardware, it wedges the controller (found on hardware) |

## 2. Findings

### 1. CMD12 after a multi-block write waits only 100 ms (High)

**Where:** `SDHOST_CMD_TIMEOUT_MS` (line 135); `sdhost_send_stop()` (lines 497-525, the wait at 507).

**What happens:**

- After an open-ended CMD25 write, CMD12 is sent with `SDCMD_BUSYWAIT`. `NEW_FLAG` stays set until the card releases DAT0, which it holds while programming.
- The SD Physical Layer spec allows a write busy of up to 250 ms (SDSC/SDHC) or 500 ms (SDXC) ([PLS §4.6.2.2](#ref-write)), and a slow or worn card can use all of it.
- The wait is bounded at 100 ms, so the request returns `-ETIMEDOUT` although the data was written.
- `NEW_FLAG` is then still set, so every later request fails at line 562 ("previous command never completed") until something resets the controller.

R1b commands issued by the SD stack (lines 599-608) are bounded the same way, by `cmd->timeout_ms` (`CONFIG_SD_CMD_TIMEOUT`, 200 ms by default). An erase (CMD38) can be busy for seconds: its timeout scales with the number of blocks erased, 250 ms per block when the card gives no erase parameters ([PLS §4.6.2.3](#ref-erase)).

**Fix:**

- Bound the stop's busy wait by the data timeout (`data->timeout_ms`, i.e. `CONFIG_SD_DATA_TIMEOUT`), and never use less than 500 ms after a write. The spec recommends more than 500 ms even for cards limited to 250 ms ([PLS §4.6.2.2](#ref-write), application note).
- For R1b commands, use at least the spec's busy limits.
- On timeout, recover (finding 2).

### 2. No recovery after an error or timeout (High)

**Where:** lines 562-567, 628-633, 680-683, and the PIO error returns at 351-355 and 363.

**What happens:**

- A command timeout leaves `NEW_FLAG` set.
- A failed PIO transfer leaves words in the FIFO and the data state machine mid-transfer, and sends no CMD12 for an open-ended multi-block transfer, so the card stays in data state.
- The driver returns the error and leaves all of that state in place. Later requests either fail, or a read silently takes the leftover FIFO words as the start of its own data.
- That last case is an **integrity** problem: wrong data is reported as a successful read. Nothing checks that the FIFO is empty before a read starts.

**Fix:**

- On any error or timeout: send CMD12 if the transfer was multi-block, drain the FIFO, and return the state machine to idle, or run `sdhost_soft_reset()` and let the SD stack re-initialise.
- Before a read's data phase, check that the SDEDM fill level is 0. Fail if it isn't, rather than return stale words.

### 3. Input clock fixed at 250 MHz (High, conditional)

**Where:** `clock-frequency = <DT_FREQ_M(250)>` in the `sdhost` node of `bcm2710.dtsi`; used in `sdhost_calc_clk_div()` (lines 426-432) and `set_io` (line 463).

**What happens:**

- The SDHost clock is the VideoCore core clock divided by `SDCDIV + 2`. The divider is computed from the devicetree constant, not the real rate.
- If the firmware runs the core clock higher than 250 MHz, or scales it at run time, the SD bus runs faster than requested. For example, at 400 MHz a 50 MHz request gives 80 MHz, beyond what the card was told and beyond the spec's 50 MHz High Speed maximum ([PLS §4.3.11](#ref-hs), [§5.3.2](#ref-ds)).
- That leads to CRC errors and, at worst, corrupt writes.
- `config.txt` settings such as `core_freq` / `core_freq_min`, or `enable_uart` behaviour, decide this. A devicetree constant can't know them.

This project pins `core_freq=250` and `core_freq_min=250` in `config.txt`, so **it is safe here**. The PR's Zero 2 W board doesn't appear to pin the clock.

**Fix:** read the core clock from the firmware at init (Zephyr's `rpi_fw` driver already supports `GET_CLOCK_RATE`), as Linux does through its firmware clock driver. Failing that, document the `config.txt` requirement in the binding.

### 4. `SDTOUT` never reprogrammed for the bus clock (Medium)

**Where:** `SDTOUT_RESET_VALUE` (line 119), written once in the reset table (line 159).

**What happens:**

- `SDTOUT` counts SD clock cycles. The fixed `0x00f00000` is about 0.31 s at 50 MHz, 0.63 s at 25 MHz and about 39 s at 400 kHz.
- At 50 MHz, an SDXC card's legitimate 500 ms write busy ([PLS §4.6.2.2](#ref-write)) exceeds it and raises `SDHSTS_REW_TIME_OUT`. That is a spurious write failure, reported as `-ETIMEDOUT` at lines 352-355 or 410-414.

**Fix:** reprogram `SDTOUT` in `set_io` whenever the divider changes, for a fixed time of at least 500 ms. As I understand it, Linux sets it to half the clock rate, i.e. 500 ms.

### 5. Timeout arithmetic: UB and IB (Medium)

**Where:** lines 204-205, 288-289, 309-310, 396-397, 554-555.

**What happens:**

- In the SDHC API `timeout_ms` is a signed `int`, and `SDHC_TIMEOUT_FOREVER` is `-1` (`sdhc.h:39`). The driver treats any non-zero value as a finite number of milliseconds:
  - **Command path:** `-1 * 1000 = -1000` reaches `WAIT_FOR`. `k_us_to_cyc_ceil32()` converts it to an unsigned 64-bit value and truncates the result to 32 bits. "Forever" becomes an arbitrary, wrapped cycle count: possibly near zero, possibly minutes.
  - **Data path:** `deadline = k_uptime_get() + (-1)` is already in the past, so the first FIFO stall fails at once (line 359).
- **UB:** `timeout_ms * 1000` is signed `int` multiplication and overflows for timeouts above 2,147,483 ms (about 36 minutes). Only a configuration value could reach that, but it is undefined behaviour.
- **IB:** passing an `int` timeout through to `WAIT_FOR`'s unsigned conversion depends on how negative values convert. That conversion is well defined (modulo 2^n), but the resulting bound is not what the caller meant.

No caller in the pinned Zephyr passes `SDHC_TIMEOUT_FOREVER` today (checked in `subsys/sd`), so this is latent. Other drivers handle it explicitly (`sdhc_esp32.c:1446`, `intel_emmc_host.c:333`).

**Fix:** treat `timeout_ms < 0` as forever: loop without a deadline, or use `K_FOREVER` semantics with a yield. Do the µs conversion in `int64_t` (or `uint32_t` after a range check).

### 6. Busy-waits for whole transfers (Medium)

**Where:** `WAIT_FOR(..., k_busy_wait(10))` (line 205); `WAIT_FOR(..., NULL)` (line 289); the `continue` spin in the PIO loop (line 365), bounded only by the deadline.

**What happens:**

- Every wait spins the CPU, up to the data timeout (10 s by default) on a slow or failing card.
- The BCM2710 port runs on one core, so a transfer starves every thread at a lower priority than the caller for that whole time, as well as threads of equal priority when time slicing is off.
- In an application, a settings write from a high-priority thread could stall time-critical work such as audio streaming. It also burns power.
- **Seen on hardware** ([section 7](#7-results-on-a-real-pi-3a), fourth run): while three writers and a reader used the card, a lower-priority thread asking to run every 1 ms ran 0 times in 10 s.

**Fix:**

- Spin only for short waits, then sleep (`k_usleep()` / `k_yield()` in the delay statement). Note that line 278 explains why the data state machine poll must not be slowed during multi-block reads, so keep that one tight but bounded.
- In the longer term, use the controller interrupt; the binding already declares it.

### 7. `bias-pull-up` not applied on BCM2710 (Medium, dependency)

**Where:** the PR's `sdhost_gpio48` pinctrl group (`bias-pull-up` on CMD and DAT0-3), applied by `drivers/pinctrl/pinctrl_bcm2711.c` (lines 26-63 in the pinned Zephyr).

**What happens:**

- The pinctrl driver programs pulls through the BCM2711 registers `GPIO_PUP_PDN_CNTRL` at offsets 0xE4 and up.
- The Pi 3 family's SoCs (the BCM2837, the 3A+'s BCM2837B0 and the Zero 2 W's RP3A0) use the legacy `GPPUD` + `GPPUDCLK0/1` sequence at 0x94/0x98/0x9C instead.
- On a Pi 3 or Zero 2 W, the requested pull-ups are therefore **not applied**, and the driver writes to undocumented offsets. It works only because the boot firmware leaves the SD pull-ups enabled.

This isn't code in this PR, but the PR's devicetree depends on it. PR #110190 in the same series ("GPIO legacy pull control") presumably fixes it. One trap for that work: the two schemes encode pull direction oppositely. BCM2711 uses 01 for pull-up and 10 for pull-down; legacy `GPPUD` uses 10 for pull-up and 01 for pull-down.

**Fix:** land #110190 first, or note the ordering in this PR.

### 8. FIFO fill level not clamped (Low)

**Where:** lines 327-330.

**What happens:** `fifo_used` is a 5-bit field (0-31) but the FIFO holds 16 words. If it ever reads above 16, `SDDATA_FIFO_WORDS - fifo_used` wraps. That's defined for `uint32_t`, but then the write path pushes up to `words_left` words into a full FIFO. It can't overrun memory, because the loop is bounded by `words_left`, but it causes FIFO errors and lost data.

**Fix:** `fifo_used = MIN(fifo_used, SDDATA_FIFO_WORDS)`.

### 9. No internal locking (Low, races)

**Where:** `request` (527), `set_io` (440), `reset` (434); the read-modify-write of `data->hcfg` (481-484).

**What happens:**

- A command is a multi-register sequence: SDHBCT, SDHBLC, SDARG, SDCMD, then PIO, then CMD12. Two concurrent callers would interleave them and corrupt both transfers.
- Today Zephyr's SD stack serialises card I/O with `card->lock` (`subsys/sd/sd_ops.c:557, 724`), so the disk path is safe.
- Direct users of the SDHC API (tests, shell commands, a second subsystem) are not.
- There is no ISR, so there are no ISR/thread races.

**Fix:** a `k_mutex` around `request`, `set_io` and `reset` costs little and makes the driver safe on its own.

### 10. Block size and count not validated (Low)

**Where:** lines 543-552, 621-622.

**What happens:** `block_size` (beyond the multiple-of-4 check) and `blocks` aren't checked against `max_blk_len` (1024) or the widths of `SDHBCT` / `SDHBLC`. Out-of-range values are truncated by the hardware, and the transfer then times out instead of failing with `-EINVAL`. This doesn't affect memory safety.

**Fix:** reject `block_size > 1024` and counts beyond the `SDHBLC` field.

### 11. Small API details (Info)

- `ios->clock == 0` selects the slowest divider (line 462) rather than stopping the clock, which is what a 0 clock asks for in the SDHC API. SDHost has no clock gate, so this may be the best it can do; worth a comment.
- *Corrected:* an earlier version of this review called `f_min = clock_freq / SDCDIV_MAX_CDIV` (line 723) optimistic. It isn't. The slowest real clock is `clock_freq / (SDCDIV_MAX_CDIV + 2)`, so the reported `f_min` is slightly above it, and a request for exactly `f_min` gives divider 2045 and that exact rate. No defect.

### 12. Stopping a multi-block read depends on winning a race (High, found on hardware)

**Where:** `sdhost_data_fsm_settled()` (lines 253-269), `sdhost_wait_xfer_complete()` (lines 282-294), and the CMD12 after the PIO loop (lines 680-686).

**What happens:**

- An open-ended read (CMD18) is stopped with CMD12 only after the PIO loop has read the last word and the data FSM has settled. The card doesn't wait: once a block is sent, it starts the next one.
- The driver's own comment (above line 282) describes the window. The FSM has to be caught in READWAIT and pushed on with `FORCE_DATA_MODE` before the next block starts; a slower poll "can miss that window, and the FSM then runs on into the next block and stays in READDATA with the FIFO full".
- On a real Pi 3A+ the window was missed: `data FSM still busy after read, edm 0x00010902`. That is FSM READDATA with 16 words of the next block in the FIFO. It happened after about 940 good 8-block reads, and again early in a run of 4 KB random reads ([section 7](#7-results-on-a-real-pi-3a)). Any interrupt or higher-priority thread during that window loses the race. The board runs a 10 kHz system tick with time slicing (its defaults, which Zephyr's own tests get too), and a real application adds its own interrupts, such as USB and network.
- The failed read then leaves the card sending and the controller mid-transfer, with no CMD12 sent (finding 2). Every later command fails ("NEW_FLAG never cleared", then "previous command never completed"), until a power cycle through the disk API eventually brings it back, after about 12 seconds.

**Fix:**

- Don't depend on the poll's timing. Use closed-ended reads: CMD23 (`SET_BLOCK_COUNT`) before CMD18 where the card supports it (SCR `CMD_SUPPORT`). Or program the controller's block count (`SDHBLC`) so it stops the data phase itself.
- If the FSM has run on, recover rather than fail: send CMD12 anyway, drain the FIFO until the FSM settles, and discard the extra words (finding 2's recovery).

**Carried here** as [`zephyr/patches/zephyr/sdhost-stop-open-read.patch`](../../zephyr/patches/zephyr/sdhost-stop-open-read.patch), the second option, on top of the PR:

- After the last requested word of a CMD18, the driver no longer waits for the FSM to settle. It waits only until the FSM is idle, in READWAIT, or has run on: **words in the FIFO**. By then the requested blocks' CRCs have been checked, so their status is read first. READDATA alone isn't a run-on: the FSM stays there a moment after the last word, while that block's CRC arrives (see the sixth run, below).
- Then it issues CMD12 at once, and empties the FIFO, discarding the words, until both the command and the data phase are done. Waiting for either alone would deadlock: with the FIFO full, the controller stops the bus clock, so CMD12 can't complete.
- A block the stop cuts short ends with the controller's data timeout (SDTOUT, 314 ms at 50 MHz); that status is cleared. So a run-on read costs up to about a third of a second instead of failing and taking the card away for 12 s. The driver logs `read ran on past its last block: stopped, N words discarded, T ms`.
- A failed open-ended read (a CRC error part-way, a PIO timeout) is stopped the same way, which fixes finding 2 for reads (test `f2a` now passes). Writes, and single-block reads (`f2b`), still have finding 2.
- CMD23 would avoid the run-on altogether, but needs the SD stack to send it; this fix stays inside the driver.

`tests/sdhost_review` models the run-on (`test_f12_read_running_on_is_stopped`): before the fix it fails with the same `data FSM still busy after read, edm 0x00010902` as the Pi; after, it passes.

**On the Pi** (fifth run, [section 7](#7-results-on-a-real-pi-3a)), with the patch, finding 12 is gone. The run made well over 100,000 multi-block reads, including a 10-minute soak, with no failure and no recovery.

**A flaw in the first version, found by the sixth run and corrected:** it counted READDATA after the last word as a run-on. Logging every such case showed 22,439 of them, every one with 0 words discarded in 0-1 ms: nearly all were the last block's CRC still arriving, not a run-on. In that case the first version read the status before the CRC check and then cleared it, so **a CRC error in the last block of a multi-block read could have been lost**, and wrong data returned as good. None occurred in the runs, but `tests/sdhost_review` now models the CRC tail (`test_multi_block_read_crc_error_on_last_block`): the first version fails it with the Pi's exact "0 words discarded" signature, and the corrected one passes. The corrected patch counts only words in the FIFO as a run-on, and logs only then. **On the Pi** (eighth run) it passed everything but finding 4's check, with no recovery and **no run-on at all** in well over 100,000 multi-block reads. So on this card the failures are gone because CMD12 now goes out promptly after the last block; the stop-and-drain path for a real run-on is exercised only by the model (`test_f12_read_running_on_is_stopped`), not yet by hardware.

## 3. What's good

- Unknown data-phase opcodes are refused rather than guessed (line 241): a wrong direction on a write would leave the card holding unsent bytes.
- Status bits are cleared write-1-to-clear using only the bits read, so no event that arrives in between is lost (349, 407, 413, 517, 574, 641).
- The registers are mapped uncached, with the reason documented (743); pinctrl errors are propagated (745-749).
- The data state machine handling (`sdhost_data_fsm_settled`) and auto-CMD12 are carefully reasoned and commented.
- Byte-wise FIFO packing (376-385) handles unaligned and odd buffers safely.

## 4. Impact on applications

- Finding 3 doesn't apply to this repository's card images: their `config.txt` pins the core clock at 250 MHz. An application that leaves the core clock to the firmware is exposed.
- **Findings 1 and 2 are the ones most likely to hit an application that writes to the card**, such as one keeping settings there: a slow write turns into a "failed" write and a wedged controller.
- Before relying on the driver for writes, fix 1, 2, 4 and 5, either upstream or as a follow-up patch in `zephyr/patches.yml`.
- Until then, write only from a low-priority thread, and handle `-ETIMEDOUT` by resetting and retrying.

## 5. Draft upstream comment

Not posted. To be posted on #110196 together with the hardware results (see [To do](#to-do)):

> Thanks for this driver. I've applied it to a Pi 3A+ board port (BCM2837B0,
> same BCM2710 SoC family) and it passes tests/drivers/sdhc, tests/subsys/sd/sdmmc
> and tests/drivers/disk/disk_access on QEMU raspi3ap; hardware results to
> follow. From reading the code, a few things I think may bite on real cards:
>
> 1. CMD12 after an open-ended write waits on busy with
>    `SDHOST_CMD_TIMEOUT_MS` (100 ms), but write busy can run to
>    250 ms (500 ms SDXC: "Write busy after CMD12 is maximum 250ms (500ms
>    for SDXC and SDUC)", Physical Layer Simplified Spec v9.10, 4.6.2.2);
>    on timeout NEW_FLAG stays set and later requests
>    fail with "previous command never completed". Suggest the data timeout
>    for the stop's busy wait, plus recovery on timeout.
> 2. Error paths leave the FIFO/FSM/NEW_FLAG as they are and send no CMD12
>    on a failed multi-block transfer, so a later read can return stale FIFO
>    words. A soft reset (or stop + drain) on error and an empty-FIFO check
>    before a read would cover it.
> 3. `SDTOUT` stays at 0x00f00000 cycles (~0.31 s at 50 MHz) for every
>    clock; reprogramming it with the divider (~500 ms) would avoid spurious
>    REW_TIME_OUT on SDXC writes.
> 4. `SDHC_TIMEOUT_FOREVER` (-1) is treated as a finite timeout (the
>    data-path deadline is already past; `-1 * 1000` wraps in WAIT_FOR), and
>    `timeout_ms * 1000` can overflow `int`.
> 5. `clock-frequency` is fixed at 250 MHz; if the firmware runs the core
>    faster, the bus is overclocked. Reading the rate from the firmware
>    (`GET_CLOCK_RATE`) or documenting the `config.txt` requirement would help.
>
> Minor: clamp the FIFO fill level, a mutex around request/set_io/reset,
> validate block size/count, and `bias-pull-up` depends on #110190 for
> BCM2710's legacy pull registers. Happy to send patches for any of these.
>
> On a real Pi 3A+ with a 64 GB SDXC card (4-bit, 50 MHz), SDTOUT reads
> 314 ms (point 3). Open-ended multi-block reads occasionally fail with
> "data FSM still busy after read, edm 0x00010902": the FSM ran on into the
> next block before CMD12, as the comment above sdhost_wait_xfer_complete()
> warns. Across four runs it happened in nearly every test making
> repeated multi-block reads, after anywhere from 5 to about 940 good
> ones. Writes, data integrity and erase otherwise look right: no write
> failed, and no read returned wrong data.
> The controller then stays wedged ("NEW_FLAG never cleared", "previous
> command never completed") until a re-init some seconds later (point 2).
> CMD23 (or the controller's block count), plus a stop-and-drain recovery
> when the FSM has run on, would avoid it.
>
> tests/drivers/sdhc (5/5) and tests/drivers/disk/disk_access (3/3) pass
> on the 3A+ too, as on your Zero 2 W; they make too few multi-block reads
> to hit the race, which a 1000-read stress test does every time.
>
> With a small driver change (issue CMD12 as soon as the last requested
> word is in, and empty the FIFO until both the command and the data
> phase are done; patch attached), the same card ran a 10-minute random
> read/write/erase soak (3.5 GB written, 4.2 GB read, every read checked)
> and every other test with no failure.
>
> Tests that reproduce these on a Pi 3A+ (link to tests/sdhost_hw), and
> against a register-level model of the controller (link to
> tests/sdhost_review), are available if they're useful for the fixes.

## 6. Tests reproducing the findings

There are two suites. The hardware suite is the confirmation; the model suite runs anywhere (it's part of `make test`) and covers finding 8, which can't be provoked on hardware.

### 6.1 On a real Pi 3A+

[`tests/sdhost_hw`](../../tests/sdhost_hw) runs the PR's driver, unmodified, on a Pi 3A+ (BCM2837B0) against a real card. The only change is that the driver library is built with `-fsanitize=signed-integer-overflow`, for finding 5. Card init goes through Zephyr's SD stack; the tests then call `sdhc_request()` directly, so each one sees exactly what the driver returns. The tests read the real SDHost and GPIO registers, and ask the firmware for the real core clock.

**Running it:**

1. `make sd-hw-test` builds the suite and writes `out/sd-hw-test/sdcard.img`.
2. Flash the image to a **spare** card. The tests write the last 16 MB of the card, and only if no partition reaches that far: the image's one partition is 64 MB, so on any real card that area is free. The write tests are skipped otherwise.
3. Leave header pin 37 (GPIO 26) unconnected; finding 7 uses it.
4. Connect a 3.3 V USB serial adapter to GPIO 14/15 (header pins 8 and 10, plus a ground), open it at 115200 baud, e.g. `picocom -b 115200 /dev/ttyUSB0 | tee sd-hw-test.log`, and power the Pi on. The run ends with `PROJECT EXECUTION SUCCESSFUL` or `FAILED` in about a minute.

`make sd-hw-test SD_HW_TEST_FLAGS=--qemu` also boots the suite on QEMU's emulated Pi 3A+. That's a smoke test only: QEMU completes transfers instantly, has no GPIO pulls and doesn't model `SDTOUT`.

**How to read the results:** the `sdhost_hw_basic` suite checks the driver works on this Pi and card (card init, pattern blocks read back, a multi-block read and stop), and must pass. In `sdhost_hw_review`, each test asserts what a correct driver should do. There is no `ZTEST_EXPECT_FAIL` here: **FAIL means the finding reproduced on this hardware**, PASS that it didn't, SKIP that the precondition wasn't met (the line before says why). Each test prints what it measured, and the driver's own error messages appear inline.

| # | Test | What it does on the hardware | Reproduces when |
|---|---|---|---|
| 1 | `test_f1_multi_block_write_busy` | 127 writes of 128 KB, out of order across the scratch area. After a failure, polls CMD13 to time how long the card really stayed busy | A write fails: the card was busy over 100 ms after CMD12. Depends on the card: likeliest on a slow, nearly full or SDXC card |
| 2 | `test_f2_failed_read_leaves_card_in_data_state` | Makes a 4-block CMD18 fail (see below), then asks the card its state with CMD13 | Card state is 5 (sending data): no CMD12 was sent |
| 2 | `test_f2_next_read_after_failure` | Makes a CMD18 of pattern blocks fail, then reads another pattern block normally | The read fails, or succeeds with the wrong block's data |
| 3 | `test_f3_bus_clock_within_request` | Reads the core clock from the firmware and `SDCDIV`, and works out the real bus clock | Bus runs faster than the stack asked for. Only with `core_freq` above 250: run `make sd-hw-test RPI_CORE_FREQ=400` (the console is then at 184320 baud). The firmware may refuse to move the core clock while `enable_uart=1`; the test prints the clock it got |
| 4 | `test_f4_data_timeout_covers_write_busy` | Reads `SDTOUT` and the real bus clock | The hardware data timeout is under the card's write busy limit (250 ms SDHC, 500 ms SDXC, [PLS §4.6.2.2](#ref-write); SDXC means over 32 GB, [PLS §3.3.2](#ref-capacity)): an SDXC card at 50 MHz gives 314 ms |
| 5 | `test_f5_read_with_timeout_forever` | Single-block read with `SDHC_TIMEOUT_FOREVER` | The read fails: the deadline is already past at the first FIFO stall |
| 5 | `test_f5_timeout_multiplication_overflow` | CMD13 with a 3,000,000 ms timeout | The sanitizer catches the signed overflow at line 204 |
| 6 | `test_f6_read_starves_lower_priority` | Reads 1 MB while a lower-priority thread wants the CPU every 1 ms; prints the read speed | The other thread hardly runs during the read |
| 7 | `test_f7_pinctrl_pull_up_on_bcm2837` | Checks GPIO 26 follows the BCM2837B0's own pull registers (else skips: something is connected), then asks pinctrl for a pull-up and reads the pin | The pin stays low: the pull-up never reached it |
| 9 | `test_f9_concurrent_requests` | Two time-sliced threads at the same priority, 20 reads of 32 pattern blocks each | Any read fails or returns the wrong data |
| 10 | `test_f10_block_size_over_limit` | CMD17 with a 2048-byte block | The driver sends it instead of returning `-EINVAL` |

Finding 2 needs a read to fail. The tests use finding 5 for that (a `SDHC_TIMEOUT_FOREVER` read), falling back to a 1 ms timeout on a 128 KB read, and skip if neither fails.

Not shown on hardware: finding 8 (a corrupt FIFO level can't be provoked), and finding 11 (informational). Between tests the harness empties the SDHost FIFO, sends CMD0 and re-initialises the card, so one reproduced finding doesn't break the next test. It prints a line when it had to empty the FIFO; that is finding 2 again, since neither the driver's reset nor `sd_init()` does it.

### 6.2 Against a model of the controller

[`tests/sdhost_review`](../../tests/sdhost_review) (native_sim, run by `make test`) compiles the PR's driver source, as applied from `zephyr/patches`, against a **model of the controller and card** (`src/sdhost.c`). Register reads and writes are redirected into the model. The model implements:

- commands and their busy period;
- the data FIFO and its 5-bit fill level;
- the data state machine and status bits;
- the clock divider, and the core clock it divides;
- fault injection: long busy, late response, slow data, CRC errors, a different core clock, a corrupt fill level.

Finding 7 uses the pinned `pinctrl_bcm2711.c` against a logged fake GPIO block (`src/pinctrl.c`).

**How to read the results:**

- **Sanity tests must pass.** They check the model with normal reads, writes, stops and clock settings.
- **Each finding's test asserts what a correct driver should do.** Today each one fails on that assertion and is marked `ZTEST_EXPECT_FAIL`, so ztest reports it as a pass and `make test` stays green.
- **When a fix lands, its test passes**, which ztest reports as a failure: drop the test's `ZTEST_EXPECT_FAIL` line then.
- Finding 11 is informational and has no test.

| # | Test | Scenario | Fails today because |
|---|---|---|---|
| 1 | `test_f1_write_busy_250ms_after_stop` | CMD25 of 2 blocks; card busy 250 ms after CMD12 (the SDHC limit, [PLS §4.6.2.2](#ref-write)) | Request returns `-ETIMEDOUT` after 100 ms although the write succeeded |
| 2 | `test_f2_failed_multiblock_read_is_stopped` | CRC error in block 2 of a 4-block CMD18 | No CMD12: the card is left in the data state |
| 2 | `test_f2_stale_fifo_after_timeout` | Read of block 5 times out (card answers after 300 ms), then block 6 is read | The block 6 read returns block 5's data, with success |
| 3 | `test_f3_core_clock_not_250mhz` | Core clock 400 MHz, 50 MHz requested | Bus runs at 80 MHz |
| 4 | `test_f4_data_timeout_follows_clock` | 50 MHz bus | Hardware data timeout is 314 ms (< 500 ms SDXC busy, [PLS §4.6.2.2](#ref-write)) |
| 5 | `test_f5_data_timeout_forever` | `SDHC_TIMEOUT_FOREVER` read, data after 50 ms | Read gives up at the first FIFO stall |
| 5 | `test_f5_timeout_multiplication_overflow` | Command timeout 3,000,000 ms; driver built with `-fsanitize=signed-integer-overflow` | Signed overflow (UB) caught at line 204 (`timeout_ms * 1000`) |
| 6 | `test_f6_slow_read_starves_lower_priority` | 300 ms read while a lower-priority thread wants the CPU every 1 ms | The other thread runs 0 times during the read |
| 7 | `test_f7_pull_up_uses_bcm2837_registers` | Pull-up on GPIO 49 (SD CMD) | Driver writes 0xe4+ (BCM2711-only); GPPUD/GPPUDCLK1 never written |
| 8 | `test_f8_fifo_level_out_of_range` | Fill level reads 20 during a write | 128 words written into a full FIFO |
| 9 | `test_f9_concurrent_requests` | Two equal-priority threads, 10 reads each, time slicing | Commands issued in the middle of the other thread's data phase |
| 10 | `test_f10_block_size_over_limit` | 2048-byte block (host limit 1024) | Accepted (returns 0), not `-EINVAL` |
| 12 | `test_f12_read_running_on_is_stopped` | A 4-block CMD18 runs on into a fifth block; the FIFO fills; with it full no command completes (the bus clock stops); CMD12 cuts the block short, and SDTOUT ends it | Fails with "data FSM still busy after read, edm 0x00010902", as on the Pi. Passes with `sdhost-stop-open-read.patch` |

The model is a reading of the Linux driver and the PR's comments, not of a datasheet: it covers the behaviour these findings depend on, not the whole controller. **The hardware suite (6.1) is the real confirmation.**


## 7. Results on a real Pi 3A+

**Run:** 20 October 2026, `make sd-qual-test` ([`tests/sdcard_hw`](../../tests/sdcard_hw), [TESTING.md](../TESTING.md)) Full console log: [`logs/2026-10-20-sd-qual-pi3a-sdxc64.log`](logs/2026-10-20-sd-qual-pi3a-sdxc64.log).

**Setup:**

- Pi 3A+, `core_freq=250`.
- A 64 GB SDXC card (124,735,488 blocks).
- The boot files were copied onto a card whose first partition fills it, so the scratch area was refused and every write test was skipped: this run tested reads only.
- The driver came up as `bus=4-bit`, `clk_in=250000000 Hz`, High Speed at 50 MHz (SDCDIV 3).

| Result | What it shows |
|---|---|
| Bus 4-bit, 50 MHz | Card init, the switch to High Speed and to the 4-bit bus all work |
| **SDTOUT 15,728,640 cycles = 314 ms** at 50 MHz | **Finding 4 reproduced**, at exactly the value the review predicted for an SDXC card at 50 MHz. That is under the 500 ms an SDXC write may be busy |
| Boot partition: 6 files, 4.35 MB, each read a cluster at a time, a block at a time and into a misaligned buffer, all CRCs equal | Single-block reads and misaligned buffers return correct data. With 512-byte clusters these were all single-block reads |
| One block per MB across the whole 64 GB (60,906 reads) | Single-block reads work across the whole address range |
| Read of block 0xffffffff: the driver logged `PIO r: sdhsts=0x00000080` (REW_TIME_OUT) and returned -116; the card worked after | The SD stack's bounds check wraps (`start + count`, `subsys/sd/sd_ops.c` lines 550 and 717), so the request reached the card. A Zephyr SD stack bug, not the driver's, since fixed here (`sd-ops-bounds-overflow.patch`). The driver handled the timeout of a single-block read cleanly |
| Sequential reads: 1 block a request at 670 kB/s; at 8 blocks a request, **failed** after about 7,500 blocks: `data FSM still busy after read, edm 0x00010902` | **New finding 12**: the CMD18 stop race was lost |
| The next command: `CMD17: NEW_FLAG never cleared`; the harness's power cycle (disk DEINIT/INIT): 55 x `CMD13: previous command never completed`, `Card busy when powering off`, then the re-init succeeds, about 12 s later | **Finding 2 reproduced**: no recovery after a failed transfer. The power-off first waits for the card to be ready, polling CMD13, which the wedged driver refuses until the SD stack gives up (about 11.7 s); only the power-off and re-init clear it |
| Random 4 KB (8-block) reads: **failed** the same way early on, with the same wedge after | Finding 12 again |
| CID ioctl: -116 (the SD stack sends CMD2, which a card in transfer state ignores); the card worked after | A single-command timeout is handled cleanly |

After this run the harness was changed: it no longer asserts in a test's `before()` (ztest would report that as a skip; the SKIPs in this run were the write tests' own, for want of a scratch area), it gained a read-only multi-block read stress test, and the script now also puts `zephyr.crc` in `out/sd-qual-test/boot/`.

**Second run**, the same day, same Pi and card, with the harness changes above ([log](logs/2026-10-20-sd-qual-pi3a-sdxc64-run2.log)). The card is now a Raspberry Pi OS layout (a 256 MB FAT partition, then a Linux partition to the end), so the write tests were again skipped; the tests refused to write into the Linux partition, as intended.

| Result | What it shows |
|---|---|
| `zephyr.bin` (3,457,028 bytes) read through FAT matches the host's CRC-32 | 3.4 MB read exactly as written (single-block reads: 512-byte clusters) |
| Multi-block read stress: **failed at read 853** of 1000 (32 blocks at 3004), same error, `edm 0x00010902` | **Finding 12 again**, now in a test made for it |
| Sequential reads: 1 block a request at 862 kB/s; 8 blocks a request failed at the **5th** read | Finding 12: it can happen almost at once |
| Random 4 KB reads: failed again | Finding 12 |
| Each failure wedged the driver; each recovery took 11.7 s and succeeded (3 in all) | Finding 2 |
| Block 0xffffffff reached the card again | The SD stack's wrapping bounds check (expected failure) |
| SDTOUT 314 ms | Finding 4 again |

Across both runs, finding 12 struck in every test that made multi-block reads: five times out of five. Between 5 and about 940 reads went through first.

**Third run**, same Pi and card, now formatted as one FAT32 partition with 32 KB clusters and the boot files copied on ([log](logs/2026-10-20-sd-qual-pi3a-sdxc64-run3.log)). The write tests were skipped again (the partition reaches the end of the card). Since then, the harness accepts a scratch area in free clusters of a FAT32 partition, so the next run on this card will write.

| Result | What it shows |
|---|---|
| Boot files read a cluster (64 blocks) at a time, a block at a time and misaligned, all CRCs equal; `zephyr.bin` matches the host's CRC | About 380 good 64-block reads before the stress test |
| Multi-block read stress: **failed at read 243** (29 blocks) | Finding 12 |
| Sequential reads: 1 block a request at 2,213 kB/s; 8 blocks a request failed at the 495th | Finding 12 |
| Random 4 KB reads: failed | Finding 12 |
| 3 recoveries, 11.7 s each | Finding 2 |

So far finding 12 has struck in all nine tests that repeat multi-block reads, across three runs. The good runs before a failure were between 5 and about 940 reads long.

**Fifth run, with the fix** (`sdhost-stop-open-read.patch`), same Pi, card and layout ([log](logs/2026-10-20-sd-qual-pi3a-sdxc64-run5-fixed.log)). **Every suite passed except finding 4's check**, and the expected SD stack failure:

| Area | Result |
|---|---|
| Finding 12 | **No read failed**, in well over 100,000 multi-block reads. No recovery needed |
| Boot files and multi-block stress | All three read methods agree; `zephyr.bin` matches the host's CRC; 1000 reads (2-256 blocks) at 20.7 MB/s |
| Integrity | Patterns, all 30 transfer sizes, chunking, misaligned buffers, neighbours, overwrites, address bits, random mix: all pass |
| Ends of the card | First and last blocks read and write; past-the-end requests refused |
| Erase | 8 blocks (read back as 0x00, neighbours intact), 1 block then rewrite, and **8 MB in 158 ms**, verified erased |
| Sequential read | 1,905 / 6,338 / 16,640 / 21,247 / **23,153 kB/s** at 1 / 8 / 64 / 256 / 2048 blocks a request |
| Sequential write | 551 / 4,306 / 13,932 / 11,396 / **21,459 kB/s** |
| Random 4 KB | Reads 1,166/s (latency 0.3 / 0.9 / 3.4 ms min / avg / max); writes 985/s (0.7 / 1.0 / 5.5 ms) |
| Concurrency | Three writers and a reader, 40 rounds, all data right. A lower-priority thread ran **0 times in 19.7 s** (finding 6) |
| Power cycles | 20 cycles, slowest init 41 ms; writes survive a power-off straight after |
| **Soak** | **600 s**: 47,512 writes (3.5 GB), 54,723 reads (4.2 GB), 3,194 erases, every read checked against a shadow copy; then the whole 64 MB area verified. **No error** |
| Finding 4 | SDTOUT still 314 ms: the only real failure left |

(ztest printed the soak's duration as 152.6 s: it times tests with the 32-bit cycle counter, which at 19.2 MHz wraps every 223.7 s. The soak's own clock, kernel ticks, is right: 600 s.)

**Sixth run**, with the first version of the patch logging every READDATA it saw after a last word ([log](logs/2026-10-20-sd-qual-pi3a-sdxc64-run6.log), condensed: the 22,439 identical warnings are folded into counts). The same results as the fifth: every suite passed except finding 4's check, the 600 s soak (2.1 GB written, 2.5 GB read) and final verify passed, with no recovery. The warnings all read 0 words discarded in 0-1 ms: 21,111 in the soak, 1,303 in the concurrency test, and only 2 in the 1000-read stress test and 3 in the performance tests. Writing them to the 115,200-baud console slowed those tests (soak 3.7 / 4.4 MB/s, from 6.2 / 7.4). They showed the first version's flaw described under finding 12.

**Seventh run** ([log](logs/2026-10-21-sd-qual-pi3a-sdxc64-run7.log), condensed): the same `zephyr.bin` as the sixth (CRC-32 `b93dbc96`), so the first version of the patch again, not the corrected one. The results repeat the sixth's: everything but finding 4's check passed, soak included (2.1 GB written, 2.5 GB read), with no recovery, and 22,863 READDATA sightings after a last word, all with 0 words discarded in 0-1 ms. Two runs of the same build agree.

**Eighth run, the corrected patch** (`zephyr.bin` CRC-32 `5690ddcd`, a different binary from the sixth and seventh runs') ([log](logs/2026-10-21-sd-qual-pi3a-sdxc64-run8-corrected.log)). Everything passed except finding 4's check (and the expected SD stack failure):

| Area | Result |
|---|---|
| Run-ons | **None logged**: no read found words of an unrequested block in the FIFO |
| Recoveries | None |
| Soak | 600 s: 47,863 writes (3.7 GB), 54,986 reads (4.2 GB), 3,158 erases, every read checked; 64 MB final verify passed. 6.4 / 7.4 MB/s, back to the fifth run's rate without the logging |
| Speed | Sequential reads to 23.6 MB/s, writes to 18.7 MB/s; random 4 KB 1,199 reads/s, 859 writes/s (max latency 3.2 / 16.1 ms) |
| Erase | 8 MB in 13 ms, verified |
| Concurrency | All data right, 19.1 s; the lower-priority thread ran 0 times (finding 6) |

**Ninth run, with the SD stack's range-check fix** (`sd-ops-bounds-overflow.patch`), 21 October 2026, on two cards.

*The same 64 GB SDXC card* ([log](logs/2026-10-21-sd-qual-pi3a-sdxc64-run9.log)): as the eighth run, and now `test_4_wrapping_range_refused` passes on hardware: the read of block 0xffffffff is refused before it reaches the card. Only finding 4's check failed (SDTOUT 314 ms at 50 MHz).

| Area | Result |
|---|---|
| Recoveries | None |
| Soak | 600 s: 48,038 writes (3.6 GB), 55,554 reads (4.3 GB), 3,158 erases; final verify passed |
| Speed | Sequential reads to 23.5 MB/s, writes to 20.9 MB/s; random 4 KB 1,218 reads/s, 603 writes/s (max latency 3.6 / 63.7 ms) |
| Concurrency | All data right, 22.0 s; the lower-priority thread ran 0 times (finding 6) |

*A 2 GB card* (3,862,528 blocks; at that size most likely standard capacity, SDSC, which is addressed in bytes rather than blocks) ([log](logs/2026-10-21-sd-qual-pi3a-sd2gb-run9.log)). Its own FAT16 partition (type 0x0e) fills the card, with the boot files copied onto it rather than the test image written, so the harness found no FAT32 boot partition and no free space for a scratch area: the write tests were skipped, and the boot-partition checks failed because the harness reads FAT32 only.

| Area | Result |
|---|---|
| **Bus width** | **Left at 1 bit**, at 25 MHz (Default Speed), so reads top out at 3.1 MB/s instead of about 12. Not the driver's bug: Zephyr's SD stack (`subsys/sd/sdmmc.c`, `sdmmc_init_hs()`) only switches to 4 bits after switching to High Speed, and returns early for a card without High Speed (or older than SD 1.10). 4-bit transfers are mandatory for SD memory cards and independent of the bus speed |
| Reads | 1,000 multi-block reads (127,185 blocks) and the whole address range sampled, all good, no run-ons and no recoveries: finding 12's fix works on a second card, and on the byte-addressed path |
| Bounds | Past-the-end and wrapping ranges refused, as on the SDXC card |
| Data timeout | SDTOUT 629 ms at 25 MHz, so finding 4's check passed: the same register value lasts twice as long at half the clock |
| Speed | Sequential reads to 3.1 MB/s; random 4 KB reads 543/s (max latency 2.2 ms) |

**Zephyr's own suites**, as the PR reports them (`make sd-zephyr-hw-test`), same Pi and card ([log](logs/2026-10-20-zephyr-sdhc-disk_access-pi3a-sdxc64.log)):

| Suite | Result |
|---|---|
| `tests/drivers/sdhc` | **5/5 pass** (card presence, host properties, reset, `set_io`, CMD8) |
| `tests/drivers/disk/disk_access` | **3/3 pass** (reads, writes and erases of 1-32 blocks at the start, middle and end of the card) in 0.27 s |

These match the PR's results on a Zero 2 W. They don't contradict finding 12: `disk_access` makes about a dozen multi-block reads in all, and on this Pi and card the stop race was lost after anywhere from 5 to about 940 of them. The suites check that each operation works once, not how often it fails under repetition, which is what `make sd-qual-test`'s stress test adds.

**Fourth run**, same Pi and card and layout, with the harness now writing into free FAT32 clusters at the end of the card ([log](logs/2026-10-20-sd-qual-pi3a-sdxc64-run4.log)). Every suite ran. The card needed recovering 14 times: every failure in the run was finding 12, on a read.

| Area | Result |
|---|---|
| **Writes** | **No write failed.** Sequential writes at 1, 8 and 64 blocks a request: 560, 3,684 and 10,346 kB/s (the larger sizes then stopped at a read-back). 500 random 4 KB writes: 556 per second, latency 0.8 ms minimum, 1.8 ms average, 74.8 ms maximum |
| **Data integrity** | Patterns, all 30 transfer sizes (1 block to 1 MB), chunked and misaligned transfers, neighbouring blocks, address bits and the card's last blocks all passed. **No read ever returned wrong data**: every failure was a reported error (-116), never silent corruption |
| **Erase** | An 8-block erase read back as 0x00, with the neighbouring blocks intact (that test then failed reading back the rest), and a single-block erase and rewrite passed. An 8 MB erase returned success in 5 ms; reading it back failed (finding 12), so it wasn't verified |
| **Multi-block read stress** | **Passed** for the first time: 1000 reads, 126,466 blocks at 20.6 MB/s |
| **Finding 12** | 14 times: in the boot file reads (the misaligned pass of `zephyr.bin`), overwrite, random mix, the first blocks, small and large erase checks, sequential and random reads, the read-back after sequential and random writes, the concurrency test, two power-cycle tests (one at the 8th cycle), the soak (after 11 operations) and its final check |
| **Finding 2** | 14 recoveries, all successful, 11.7 s each |
| **Finding 6** | With three writers and a reader on the card, a lower-priority thread that wanted to run every 1 ms ran 0 times in 10 s |
| **SD stack** | While a wedged request held the card, the other threads' requests failed with -EBUSY ("Could not get SD card mutex"): the stack waits `CONFIG_SD_DATA_TIMEOUT` for its lock, so one stuck transfer fails every concurrent user |

So the driver's write path, data path and erase look right on this card. What stops it being usable is finding 12: about one multi-block read in a few hundred fails, and each failure takes the card away for 12 s (finding 2).

**Still to run:** the soak, which can't get far while finding 12 stands.

## References

Card timings above are from the SD Association's **SD Specifications Part 1, Physical Layer Simplified Specification, Version 9.10** (1 December 2023), free from [sdcard.org](https://www.sdcard.org/downloads/pls/). "PLS §x" means a section of that document; page numbers are the printed ones.

| Ref | Section | What it says, as used here |
|---|---|---|
| <a id="ref-write"></a>PLS §4.6.2.2 | 4.6.2 "Read, Write and Erase Timeout Conditions", 4.6.2.2 "Write", pp. 95-96 | SDHC: "maximum length of busy is defined as 250ms for all write operation". SDXC and SDUC may be busy up to 500 ms, including "When multiple block write is stopped by CMD12, the busy from the response of CMD12 is up to 500ms". Summary: "Write busy after CMD12 is maximum 250ms (500ms for SDXC and SDUC)". Application note: "It is strongly recommended for hosts to implement more than 500ms timeout value even if the card indicates the 250ms maximum busy length" |
| <a id="ref-erase"></a>PLS §4.6.2.3 | 4.6.2.3 "Erase", p. 96 | The host should take the erase timeout from the SD Status parameters (4.10.2, `ERASE_SIZE`, `ERASE_TIMEOUT`, `ERASE_OFFSET`, pp. 128-129; calculation in 4.14, p. 171). Without them: "the number of write blocks (WRITE_BL) to be erased multiplied by 250 ms" |
| <a id="ref-capacity"></a>PLS §3.3.2 | 3.3.2 "Card Capacity Classification", p. 6 | SDHC: more than 2 GB up to and including 32 GB. SDXC: more than 32 GB up to and including 2 TB |
| <a id="ref-ds"></a>PLS §5.3.2 | 5.3.2 "CSD Register (CSD Version 1.0)", `TRAN_SPEED`, after Table 5-6, p. 227 | 25 MHz is "the mandatory maximum operating frequency of SD Memory Card" (Default Speed); in High-Speed mode `TRAN_SPEED` reads 50 MHz. Chapter 2 "System Features" (p. 3) lists the same: Default Speed up to 25 MHz, High Speed up to 50 MHz |
| <a id="ref-hs"></a>PLS §4.3.11 | 4.3.11 "High-Speed Mode (25 MB/sec interface speed)", p. 89 | High Speed: "the clock rate is increased to 50 MHz", entered with CMD6 |
| <a id="ref-od"></a>PLS §4.2.1, §4.4 | 4.2.1 "Card Reset", p. 41; 4.4 "Clock Control", p. 92 | Identification runs at the clock rate `fOD` (4.2, which points to 6.6.6). The Simplified Specification leaves out 6.6.6's parameter table, so the 400 kHz value appears only in passing: cards start "with a default driver strength with 400KHz clock frequency" (4.2.1), and during ACMD41 the host clocks the card at "100 KHz-400 KHz" (4.4) |

## To do

- [x] **First hardware runs** ([section 7](#7-results-on-a-real-pi-3a)): two, reads only; findings 2 and 4 reproduced, finding 12 found and reproduced in every multi-block read test.
- [ ] **Flash the qualification image** (`out/sd-qual-test/sdcard.img`) to
  a spare card, so the write, erase, concurrency, recovery and soak suites
  run too.
- [ ] **Test on a real Pi 3A+.** Run `make sd-hw-test` on a **spare**
  card ([section 6.1](#61-on-a-real-pi-3a)) and keep the console log.
  Try at least one SDXC card (over 32 GB) for findings 1 and 4, and a
  second run with `RPI_CORE_FREQ=400` for finding 3.
- [ ] **Widen the bus on cards without High Speed.** Zephyr's SD stack
  (`sdmmc_init_hs()` in `subsys/sd/sdmmc.c`, the same in Zephyr main)
  switches to 4 bits only after switching to High Speed, so a Default
  Speed card stays 1 bit wide at a quarter of the speed (ninth run, 2 GB
  card). Fix: set the bus width before the High Speed checks, as Linux
  does. A Zephyr SD stack fix, not the driver's.
- [ ] **Read FAT16 in the qualification harness**, so a card with its
  factory FAT16 partition (most cards of 2 GB or less) gets its boot
  partition checked; or write the test image to such cards, as
  [TESTING.md](../TESTING.md) says.
- [x] **Run Zephyr's suites as the PR reports them**, on the Pi 3A+
  (`make sd-zephyr-hw-test`): 5/5 and 3/3, as the PR reports on a Zero 2 W
  ([section 7](#7-results-on-a-real-pi-3a)).
- [ ] **Report upstream on #110196:** the hardware results, plus the
  review above (section 5), as a comment from your GitHub account. Offer
  patches for findings 1, 2, 4 and 5.
- [ ] Depending on the answer, fix 1, 2, 4 and 5 upstream or as a
  follow-up patch in `zephyr/patches.yml`. Rerun `make sd-test`, `make sd-hw-test` (the
  matching `tests/sdhost_hw` tests should then pass) and `make test` (the
  matching `tests/sdhost_review` tests should then pass "unexpectedly":
  remove their `ZTEST_EXPECT_FAIL` lines).
