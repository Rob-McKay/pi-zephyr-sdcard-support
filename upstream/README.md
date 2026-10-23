# Zephyr PR #123216: one commit with the fix and its test

[#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216)
must be a single commit. Its first version had the fix only, with a
title Zephyr's commit check rejects (no `area:` prefix, over 75
characters, no body). These files turn it into one commit with the fix
and the test:

| File | What |
|---|---|
| `0001-tests-sd-add-test-for-read-write-range-checks.patch` | Adds `tests/subsys/sd/bounds` (new files only) |
| `123216-commit-message.txt` | The commit message: `sd: sd_ops: make read/write range check overflow-aware`, a body covering the fix and the test, and your `Signed-off-by` |

On the PR's branch (`sd-ops-bounds-overflow`), with the fix already
committed:

```sh
git apply /path/to/0001-tests-sd-add-test-for-read-write-range-checks.patch
git add tests/subsys/sd/bounds
git commit --amend -F /path/to/123216-commit-message.txt
git push --force-with-lease
```

`git apply` adds only the test's files, so it doesn't matter that the
patch was made as a separate commit. The amend keeps you as the author.

Checked on Zephyr `main` `f70d5b0f2`: the combined commit passes
Zephyr's `gitlint` rules and `checkpatch` (0 errors, 0 warnings); the
test passes with the fix and fails without it (`test_wrapping_range`).
Before pushing you can run it as CI does:
`west twister -T tests/subsys/sd/bounds -p native_sim`.
