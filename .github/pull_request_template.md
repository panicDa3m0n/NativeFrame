### What

### Why
<!-- The measurement or the problem that motivates the change. -->

### Tested
<!-- OS, CPU, GPU and driver; tests run and their result; the deliberate mistake the new test catches. For a performance change: real frame times (APS5_PRESENT_RATE, no profiler) with the switch off and on, same binary, same session, as CONTRIBUTING.md describes. "Not tested" is a valid answer. -->

### Checklist
<!-- Rules behind each item: CONTRIBUTING.md -->
- [ ] One topic, based on current `main`
- [ ] Code follows `docs/dev/CONVENTIONS.md`; unimplemented paths throw
- [ ] Generic behaviour, not specific to one title
- [ ] Performance change: behind an `APS5_NO_*` switch, documented in `docs/dev/ENVIRONMENT.md`, with a test that fails on a deliberate mistake
- [ ] No game files, executables, dumps or frame captures in the repository or in this pull request
- [ ] Depends on: <!-- #PR, or none -->
- [ ] Fixes: <!-- issue it closes as Fixes #N, or none -->
- [ ] Unblocks: <!-- title ID and compatibility issue it moves forward, e.g. PPSA01234 #12, or none -->
- [ ] GPUs tested: <!-- vendor, model, driver and OS for driver, recompiler or VideoOut changes; otherwise n/a -->
- [ ] AI-assisted: <!-- yes / no -->
