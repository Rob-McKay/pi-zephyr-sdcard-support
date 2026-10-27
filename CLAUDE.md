# Working on this repository

Follow [`docs/CODING.md`](docs/CODING.md) for all code. In short:

- Every source file starts with `Copyright (c) <year> Rob McKay` and
  `SPDX-License-Identifier: Apache-2.0`, except patches to other projects
  and the commit message for them in `upstream/`
  (`scripts/check-headers.py --fix` adds them).
- Every function (C, Python, shell) has a Doxygen comment: `@brief`,
  every `@param`, `@return`/`@retval`, and `@par Global state` naming the
  static-storage variables and kernel objects it changes itself, and
  `errno` if it sets it.
- A public function's interface is documented at its declaration in the
  header; each definition gets a plain `/* Interface: <header>.h. */`
  comment, plus implementation notes only where needed and a
  `Global state:` note (the variables are the implementation's).
  `static` functions are documented at their definition.
- Each function has a single responsibility; split one that mixes levels
  or needs "and" in its brief.
- Keep global state to the entry points: state in one struct, passed to
  the helpers; fake devices find theirs through the device's `data`,
  threads get theirs as their argument; tests use ztest fixtures (the SD
  card suites share `struct sdq`).
- Every macro argument with a binary, ternary or assignment operator is
  written in its own parentheses: `MIN((n - 1), max)`
  (`scripts/check-macro-args.py --fix <build dir>`).
- No warnings, clean clang-tidy; registers only through the HAL in
  `hal/`; no unbounded string functions.
- Fixes to the SDHost driver or Zephyr's SD stack are patches in
  `zephyr/patches/` that could go upstream; nothing is posted upstream
  without the repository owner's go-ahead.

`make test` checks all of these (`scripts/test.sh`); the hardware tests
are built by `make sd-hw-test`, `make sd-qual-test` and the rest. Builds
and tests run in the Docker image (`make image`).
