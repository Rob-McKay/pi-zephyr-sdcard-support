# Coding conventions

These rules apply to all of the repository's own code: the tests, the HAL
headers and the Python scripts. The carried Zephyr patches
(`zephyr/patches/`) are upstream code and follow Zephyr's rules. `make test`
enforces what a tool can check:

| Rule | Checked by |
|---|---|
| 0. No compiler warnings, clean clang-tidy | the build itself (`cmake/strict.cmake`) |
| 0. Expression macro arguments in parentheses | `scripts/check-macro-args.py`, in every build (with clang-tidy) |
| 1. Doxygen comment on every function, the interface in the header | `scripts/check-docs.py` |
| 1. Changes to global state and `errno` documented | `scripts/check-globals.py`: C in every build (with clang-tidy), Python in `make test` |
| 2. Single responsibility | review, and size and complexity limits (`scripts/check-size.py`) |
| 3. Registers only through the HAL, which is fully unit tested | a search in `scripts/test.sh`, and `scripts/check-coverage.py` |
| 4. No unbounded string functions; buffer overflows trapped | clang-tidy, and `_FORTIFY_SOURCE=2` in the firmware |
| 5. Fixes to the driver are patches that could go upstream | review |
| 6. Apache-2.0 licence and copyright line in every source file | `scripts/check-headers.py` |

## 0. The build checks every file

Every target with our code calls `sd_strict_warnings()` from
`cmake/strict.cmake`, which:

- compiles with `-Wall -Wextra -Wpedantic -Werror`: any warning fails the
  build;
- runs clang-tidy on each source as it compiles (CMake's `C_CLANG_TIDY`),
  with the checks in `.clang-tidy`, all as errors. A finding fails the
  build like a compiler error. clang-tidy 20 or later must be installed
  (the Docker image has 23.1.3 from PyPI: `pip install clang-tidy==23.1.3`;
  Ubuntu 24.04's is 18, too old for `.clang-tidy`'s banned-function list).
  CMake picks the first new enough one on `PATH`.
  `-DSD_CLANG_TIDY=OFF` skips it for a quick local build only.

Zephyr's headers use GNU extensions, so they are included as system
headers (`-isystem`): the warnings and clang-tidy look only at this
repository's code. Upstream sources compiled into one of our
targets are marked with `sd_upstream_sources()`, which builds them as they
are and skips clang-tidy.

Everything is compiled by clang (`cmake/toolchain.cmake`): the Pi firmware
by Arm Toolchain for Embedded, the `native_sim` tests by the host's
clang. clang-tidy is from PyPI, and does not know ATfE's C library, so
`scripts/clang-tidy-cross.py` adds the C library's include directories to
a cross compile command as ATfE finds them.

The `.clang-tidy` set is `bugprone-*`, `cert-*`, `clang-analyzer-*`, `misc-*`,
`performance-*`, `portability-*` and `readability-*`. A few checks are off,
each for a reason given in the file: short names such as `i` and `n` are
idiomatic C, register and protocol constants are numbers, and Zephyr's
aggregated headers defeat include-what-you-use. Function complexity is
measured with macros ignored. `!`, `&&` and `||` yield `int` in C, so they
are not reported as conversions to `bool`; a positive literal such as
`0x7f` is not reported as a signed operand of a bitwise operator (a byte
promoted to `int` still is: write `0x7fU` or cast it to `unsigned int`).
The checks that ask for Annex K functions are off (section 4).

When a finding is a deliberate choice, suppress that one line, never the
check: `/* NOLINT(check-name): reason */`. The same applies to a
`#pragma GCC diagnostic` for a compiler warning. For example, Zephyr's
file-scope `K_*_DEFINE` macros can expand to an empty statement that
`-Wpedantic` reports.

### Macro arguments that are expressions go in parentheses

A function-like macro pastes its arguments in as text, so an expression
passed to one that doesn't parenthesise its parameter regroups: Zephyr's
`Z_MSGQ_INITIALIZER` computes `q_buffer + (q_max_msgs * q_msg_size)`,
and a count of `N + 1` becomes `N + 1 * size`. Not every macro we call
can be trusted to get this right, so every argument with a binary,
ternary or assignment operator outside brackets is written in its own
parentheses, whatever the macro:

```c
uint32_t n = MIN(piece, (blocks - b));
zassert_true((bus <= HS_MAX_HZ), "bus clock %u Hz over 50 MHz (review finding 3)", bus);
TC_PRINT("bus: %s\n", (hal_sdhost_bus_is_4bit(regs) ? "4-bit" : "1-bit"));
```

An argument that is a single operand needs none: a name, a literal, a
call, a member or element, a cast, a unary operator on one of those
(`-EINVAL`, `&x`, `!ok`), a type (`struct foo *`), or a designator. Nor
does a C library function that a fortified build makes a macro
(`memcpy()`, `snprintf()`): it is called as the function.
`scripts/check-macro-args.py` checks every macro call in our files and the
macro calls inside our own `#define`s, as each source compiles; with
`--fix <build dir>` it adds the parentheses. clang-tidy's
`bugprone-macro-parentheses` checks the other side: that our own macros
parenthesise their parameters.

## 1. Every function has a Doxygen comment

Each comment is short. It gives:

- `@brief`: one line saying what the function does;
- optionally, a few lines on anything a caller must know: ownership,
  locking, which thread may call it;
- `@param` for every parameter. Mark pure output pointers `@param[out]` and
  pointers that are read and written `@param[in,out]`;
- `@return` for the result, or `@retval` lines for distinct values (for
  example `0` and each negative errno). Omit both only for `void` functions;
- `@par Global state`, when the function itself changes state that
  outlives the call: a variable with static storage (file scope or a
  `static` local), including a kernel object such as a semaphore or queue
  it gives, puts to or resets, named with `@c` and what it holds.
  Locking and unlocking a mutex or spinlock leaves it as found and isn't
  listed. `errno` is listed only by a function that sets it explicitly
  (`errno = 0` before `strtol()`, say), not by one that calls a library
  function that might. Changes made through a pointer parameter are the
  caller's (`@param[out]`), and those a called function makes are its own
  to document.

Where it goes:

- **Public functions:** the interface (all of the above) at the
  declaration in the header. The definition in the `.c` file has a plain
  `/* */` comment, not a Doxygen one, that names the header
  (`/* Interface: common.h. */`), so a reader sees at once that the
  function is public and where its contract is. Notes on how it works go
  in the same comment, only where the code needs them, and so does its
  global state, as a `Global state:` note: the variables are the
  implementation's own.
- **`static` functions:** at the definition.
- **Test cases** (`ZTEST`): a one-line `@brief` saying what is checked.
- **Shell functions:** a `## @brief` block with `@param $1` and so on for
  each positional parameter used.

```c
/* common.h */
/**
 * @brief Write and read back blocks of the scratch area through the disk
 *        API, asserting both succeed and the data matches.
 *
 * @param[in,out] q The card; its buffers are used.
 * @param lba First block.
 * @param blocks Block count, at most SDQ_MAX_BLOCKS.
 * @param gen Generation to write.
 */
void sdq_write_verify(struct sdq *q, uint32_t lba, uint32_t blocks, uint32_t gen);

/* common.c */
/* Interface: common.h.
 * Global state: changes sdq.
 */
void *sdq_setup(void)
{

/**
 * @brief The driver's sys_write32(): write a model register.
 *
 * @param val Value written.
 * @param addr Register address.
 *
 * @par Global state
 * Changes @c hw (the model).
 */
static void fake_write32(uint32_t val, uintptr_t addr)
{
```


C:

```c
/**
 * @brief Split a URL into scheme, host, port and path.
 *
 * @param url NUL-terminated URL.
 * @param[out] out Parsed parts; strings point into @p out's own buffers.
 * @retval 0 Parsed.
 * @retval -EINVAL Not an http or https URL, or a part is too long.
 */
int url_split(const char *url, struct url_parts *out);
```

Python uses `##` comment blocks, which Doxygen reads as documentation
(it doesn't process `@` commands in docstrings):

```python
## @brief Build an ADTS header for one AAC frame.
#
# @param payload Raw AAC frame.
# @param channels Channel count.
# @return The header followed by the payload.
def adts_frame(payload, channels=2):
```

## 2. Every function has a single responsibility

A function does one job, at one level of abstraction. Split a function
when it:

- mixes levels: for example a command handler that parses arguments,
  changes state and formats output;
- has sections that each need their own comment to explain;
- needs "and" in its `@brief`.

Each extracted helper is named for its job, and gets its own Doxygen
comment. A function can still be long if it does one thing, such as a
table of constants or one state machine's `switch`.

`scripts/check-size.py` (lizard) fails a function over 50 lines of code or
cyclomatic complexity 15, as a backstop for review: past those a function
is almost always doing more than one job. A `switch` that maps keys or
commands one-to-one may be listed as an exception in the script, with the
reason (none are, today).

### Keep global state to the entry points

A module's state lives in one struct, with one `static` instance (or a
`static` local of the thread that owns it). Only the module's entry points
use the instance: its public functions, a thread's entry, and a callback
or interrupt handler that is given no context. Every other function takes
the state as a parameter (`struct rig *r`, `struct fake_hw *m`), so what it changes is in
its signature and it can be tested with a state of its own. In practice:

- a fake device's handler finds its state through the device's `data`
  (`DEVICE_DEFINE(..., &fake_record, ...)`), and a thread gets its state
  as its first argument;
- a function that only reads the state takes it `const`;
- tests use ztest fixtures (`ZTEST_F`, a setup function returning a
  `static` fixture) for their buffers and state, rather than file-scope
  or `static` local variables. The SD card suites share one, `struct sdq`
  (`tests/sdcard_hw/src/common.h`). A model the code under test reaches
  without a context (the SDHost and GPIO models' register hooks, the fake
  register block of the HAL test) stays global, and the functions that
  change it document it;
- a thread stack (`K_THREAD_STACK_DEFINE`) has to be global: the one
  function that starts threads on it says so.

The global state rule (section 1) then has little to list: the entry
points, and those changing what is global by nature (a model behind
register hooks, a sanitizer's counters).

## 3. Hardware is reached only through the HAL

Only the hardware abstraction layer, `hal/include/hal/`, reads or writes
registers (`sys_read32()`, `sys_write32()` and friends). The tests call its
functions instead. There is one header per hardware block
(`bcm2835_sdhost.h`, `bcm2835_gpio.h`), holding the block's register
offsets and bits and its accessors as `static inline` functions, one per job
a register does, named `hal_<block>_<job>()`. Accessors encode fields
themselves, so callers never build register words.

Every accessor is unit tested against a RAM-backed register block
(`tests/hal_bcm2835`), which checks it wrote the right value to the right
register and touched no other. `scripts/test.sh` requires 100% line coverage
of the HAL headers (`scripts/check-coverage.py`).

Exceptions:

- The hardware tests (`tests/sdhost_hw`, `tests/sdcard_hw`) read registers
  through the HAL, but they are themselves tests and run only on the Pi.
- The driver under test is upstream code with its own register access. The
  register model in `tests/sdhost_review` stands in for the hardware.

## 4. No unbounded string functions; overflows trapped at run time

These C library functions cannot be told the destination's size and are
banned, with the replacement to use:

| Banned | Use |
|---|---|
| `strcpy` | `strncpy`, and NUL-terminate yourself if the limit is reached: `strncpy(d, s, sizeof(d) - 1); d[sizeof(d) - 1] = '\0';` |
| `strcat` | `strncat` |
| `sprintf` | `snprintf` |
| `vsprintf` | `vsnprintf` |
| `gets` | `fgets` |

clang-tidy enforces the list in the build (`bugprone-unsafe-functions` with
only its `CustomFunctions` list in `.clang-tidy`; clang-tidy has no
`modernize-replace-disallowed-functions`). Where a source string is known to
fit, because its length was just checked, `memcpy(d, s, strlen(s) + 1)` is
also fine.

`memcpy()`, `memmove()`, `memset()`, `strncpy()`, `snprintf()` and the
rest are used as they are. The checks that would require C11 Annex K's
`memcpy_s()` family instead are off: neither glibc nor picolibc provides
Annex K, and the fortified C library functions below do the checking:

- `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`;
- `bugprone-unsafe-functions`' default list, and its aliases `cert-msc24-c`
  and `cert-msc33-c`;
- `clang-analyzer-security.insecureAPI.strcpy`, which repeats the
  `strcpy`/`strcat` ban.

Instead, the firmware is built with `-D_FORTIFY_SOURCE=2`
(`CONFIG_FORTIFY_SOURCE_RUN_TIME`, set by `cmake/atfe.conf`; Zephyr gives
GCC the define but not clang, so the module's `CMakeLists.txt` adds it).
clang and picolibc then check every copy into a buffer whose size the
compiler knows (`__memcpy_chk()`, `__strncpy_chk()`, ...): an overflow
calls `__chk_fail()`, a fatal error, instead of corrupting memory. It
needs optimisation, so the `-O0` `native_sim` unit tests run without it.

So the size passed to these functions must still be right: `sizeof` the
destination, the buffer size a function was given, or the space left from
an offset, after checking that the source fits. A function that writes
through a bare pointer takes its capacity as a parameter.

## 5. Fixes to the driver are patches that could go upstream

The driver is Zephyr's (PR
[#110196](https://github.com/zephyrproject-rtos/zephyr/pull/110196)), carried
as a patch. A fix found here is a separate patch on top of it in
`zephyr/patches.yml` (as `sdhost-stop-open-read.patch` is), written in the
driver's own style and to Zephyr's rules, so that it can be offered to the
PR as it is. Each fix has a test: a model test in `tests/sdhost_review` that
fails without it, and, where the hardware is needed, a test in
`tests/sdhost_hw` or `tests/sdcard_hw`. [UPSTREAM.md](UPSTREAM.md) says how
to refresh the patches.

## 6. Licence and copyright in every source file

The repository is under the Apache License 2.0, as Zephyr is. `LICENSE` is
the text as the Apache Software Foundation publishes it
(<https://www.apache.org/licenses/LICENSE-2.0.txt>), not a copy of
Zephyr's. Every source and build file starts with:

```c
/*
 * Copyright (c) 2026 Rob McKay
 * SPDX-License-Identifier: Apache-2.0
 */
```

or the same two lines as `#` comments (after any `#!` line), with the year
the file was written. Exempt: patches to other projects
(`zephyr/patches/`, `upstream/`, including the commit message prepared
there), which are under those projects' licences, and logs, documents, media and the SBOM.
`scripts/check-headers.py --fix` adds the lines to a new file.

