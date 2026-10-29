# Zephyr PR #123216: adding the test to the fix

[#123216](https://github.com/zephyrproject-rtos/zephyr/pull/123216)
is one commit. Its first version had a title Zephyr's commit check
rejects (no `area:` prefix, over 75 characters, no body). **On 27
October 2026 it was amended** to `sd: check read and write ranges for
overflow`, with a body and a `Signed-off-by`, and passes the check. **On
29 October it was amended again** (head `c1790d7bb63c`): read, write and
erase now share a `card_range_valid()` helper. It still has the fix only
(`subsys/sd/sd_ops.c`, the same change as
`zephyr/patches/zephyr/sd-ops-bounds-overflow.patch`).

Two things in the 29 October version Zephyr's checks or a reviewer may
flag:

- The helper's declaration is 106 columns: checkpatch warns `LONG_LINE`
  (limit 100).
  Wrapped, as `clang-format` would:

  ```c
  static inline bool card_range_valid(const struct sd_card *card, uint32_t start_block,
  				    uint32_t num_blocks)
  ```

- The body says "The range check in now goes through"; "in" should be
  "is". `123216-commit-message.txt` has it corrected.

If a reviewer asks for a test, these files make it one commit with the
fix and the test:

| File | What |
|---|---|
| `0001-tests-sd-add-test-for-read-write-range-checks.patch` | Adds `tests/subsys/sd/bounds` (new files only) |
| `123216-commit-message.txt` | The commit message: the PR's 29 October title and body, a paragraph for the test, and your `Signed-off-by` as on the PR |

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

Checked on Zephyr `main` `f70d5b0f2` with the 27 October fix: the
combined commit passed Zephyr's `gitlint` rules and `checkpatch` (0
errors, 0 warnings); the test passes with the fix and fails without it
(`test_wrapping_range`). Checked again on v4.5.0 with the 29 October fix
(`c1790d7bb63c`): the test still passes with it and fails without it;
`checkpatch` now gives the one `LONG_LINE` warning above.
Before pushing you can run it as CI does:
`west twister -T tests/subsys/sd/bounds -p native_sim`.
