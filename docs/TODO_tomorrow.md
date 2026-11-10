# Tomorrow

1. **Measure the 1.28 GiB floor.** Rerun the same 7-permutation sweep with
   `max_cores = 1`. Flat vs 8 workers = fixed per-cursor cost; ~8x = something
   copies the trades vector. No fix before that number exists.

2. **Decide sweep speed.** `merge1.mkn` is 8.4 GB on a 7.6 GiB box, so ~21 % of
   every pass is disk. Phase 3 (index seek) will not help — the index is 19 MB
   against 58.8 GB of payload reads. Only two things would: one-decode-many-
   strategies (not designed), or Phase 5 partitioning (only when `A > 1`).

3. **Optional:** reject `tick_size` / `lot_step` as grid axes in `AppConfig`
   validation, and fold `TestResult` into a smaller `SweepResult`.

Nothing above blocks a commit of the current state.

Known test gap: the end-price fallback's `last_price` and `history.back()`
branches are unreachable from a fixture (any print leaves a nonzero bar close),
so only the invariant "never close at 0.0" is pinned.
