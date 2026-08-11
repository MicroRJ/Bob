# Changelog

## 2026-08-11

- Split graph execution from build behavior. Graph nodes can now run arbitrary callbacks and publish results; command execution, incremental checks, and compiler dependencies live in the build layer.

- Replaced lexical include scanning with compiler-generated dependency files. Bob now stores dependencies per task in `.bob/state.elf` and uses them for incremental rebuild checks.

## 2026-07-27

- Added a new output policy. Bob now prints program output by default. Verbosity does not control task output; it controls Bob's internal output. To suppress task output, use `--quiet` or `-q`.
