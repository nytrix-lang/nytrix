<!-- nytrix-doc: {"audience":"contributor","featured":true,"group":"project","order":5,"summary":"Open contributor work and the evidence required to close it."} -->
# Open work

Use [Contributing](CONTRIBUTE.md) for commands and checks. Record completed work
under the latest version in the [changelog](CHANGELOG.md).

## Editor compile time

- [ ] Reprofile the large editor compile and improve inlining or register
  allocation only where a validated profile identifies cost. Compare baseline
  and candidate using the same source graph, compiler build, target, flags,
  and cache state. Record input/object hashes, compiler BuildID, inline-decision
  logs, and validated `cpu-clock:u` samples. A performance-only change must
  preserve emitted objects and inline decisions. Reject captures with lost
  samples or unvalidated instruction addresses.

The [performance guide](learn/performance.md#editor-compile-investigation)
explains the comparison and the latest recorded segment-lookup investigation.
The [measurement archive](learn/measurements.md) contains earlier
experiments; its historical task labels are not additional open work.

## Closing an item

Record the changed behavior, exact command and route, expected and observed
result, relevant regression coverage, and any platform limits. Compilation,
a cache hit, a skipped case, or a timeout is not execution evidence. Keep
large logs and profiles outside source control and link only the evidence
needed to understand an unresolved result.
