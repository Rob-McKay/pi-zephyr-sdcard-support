# Zephyr test for the SD stack range-check fix

The fix, [#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216)
(`sd: check read and write ranges for overflow`), **merged into Zephyr
main on 17 November 2026** as `f513d993a397`. It had the fix only.

Its test is ready here as a follow-up PR of its own, if you want one:

| File | What |
|---|---|
| `0001-tests-sd-add-test-for-read-write-range-checks.patch` | One commit, `tests: sd: add test for read/write range checks`, adding `tests/subsys/sd/bounds` (new files only): the SD stack's block read and write calls against a fake SD host controller, which must refuse ranges past the end of the card, including ones that wrap past block 0xffffffff, before they reach it |

The patch's author line is a placeholder (`Your Name`). On a branch from
Zephyr main:

```sh
git am /path/to/0001-tests-sd-add-test-for-read-write-range-checks.patch
git commit --amend --reset-author --signoff --no-edit
```

then push the branch and open the PR. Before pushing you can run it as
CI does: `west twister -T tests/subsys/sd/bounds -p native_sim`.

Checked: the patch applies to Zephyr main as of 19 November 2026. On
v4.5.0 with the merged change (`sd-ops-bounds-overflow.patch`), the test
passes with the fix and fails without it (`test_wrapping_range`).
`checkpatch` finds nothing but the missing `Signed-off-by`, which the
`--signoff` above adds.

Nothing here is posted upstream without the repository owner's
go-ahead.
