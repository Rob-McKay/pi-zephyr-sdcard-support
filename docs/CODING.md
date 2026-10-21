# Coding conventions

These rules apply to all of the repository's own code: the tests, the HAL
headers and the Python scripts. The carried Zephyr patches
(`zephyr/patches/`) are upstream code and follow Zephyr's rules. `make test`
enforces what a tool can check:

| Rule | Checked by |
|---|---|
| 0. No compiler warnings, clean clang-tidy | the build itself (`cmake/strict.cmake`) |
| 1. Doxygen comment on every function | `scripts/check-docs.py` |
| 2. Single responsibility | review |
| 3. Registers only through the HAL, which is fully unit tested | a search in `scripts/test.sh`, and `scripts/check-coverage.py` |
| 4. No unbounded string functions; buffer overflows trapped | clang-tidy, and `_FORTIFY_SOURCE=2` in the firmware |
| 5. Fixes to the driver are patches that could go upstream | review |

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

## 1. Every function has a Doxygen comment

Each comment is short. It gives:

- `@brief`: one line saying what the function does;
- optionally, a few lines on anything a caller must know: ownership,
  locking, which thread may call it, side effects;
- `@param` for every parameter. Mark pure output pointers `@param[out]` and
  pointers that are read and written `@param[in,out]`;
- `@return` for the result, or `@retval` lines for distinct values (for
  example `0` and each negative errno). Omit both only for `void` functions.

Where it goes:

- **Public functions:** at the declaration in the header, and not repeated
  at the definition.
- **`static` functions:** at the definition.
- **Test cases** (`ZTEST`): a one-line `@brief` saying what is checked.

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
