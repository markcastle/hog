# hog: handoff to Claude Code

Everything needed to pick this up as a real project rather than a single loose `.c` file.

## Suggested layout

```
hog/
  CMakeLists.txt
  README.md
  CLAUDE.md
  src/
    main.c            arg parsing, output formatting
    ntquery.c/.h      the ntdll wrapper and struct definitions
    snapshot.c/.h     take/diff/match snapshots
    score.c/.h        the impact heuristic, isolated so it is testable
  tests/
    test_score.c      pure functions, no Windows needed
    test_match.c      pid recycling, process born mid-interval
  .github/workflows/
    build.yml         MSVC and MinGW matrix, plus a cross-compile smoke test
```

The split that matters is pulling `impact_score()` and the snapshot diffing out of `main.c`. Both are pure functions over plain structs, so they can be unit tested on any platform without a Windows box in the loop. Everything that touches `ntdll` stays behind `ntquery.h`.

## Build

CMake is the pragmatic choice here since you want both MSVC and MinGW to work:

```cmake
cmake_minimum_required(VERSION 3.20)
project(hog C)
set(CMAKE_C_STANDARD 11)
add_executable(hog src/main.c src/ntquery.c src/snapshot.c src/score.c)
if(MSVC)
  target_compile_options(hog PRIVATE /W4 /O2)
  set_property(TARGET hog PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_compile_options(hog PRIVATE -Wall -Wextra -O2)
  target_link_options(hog PRIVATE -s)
endif()
```

`/MT` and `-s` are both deliberate. Static CRT means no redistributable, and stripping keeps it under 50 KB, which is the entire selling point.

Worth adding early: a CI job that cross-compiles with `x86_64-w64-mingw32-gcc` on Linux. It catches struct and warning regressions in seconds without needing a Windows runner.

## Things worth fixing first

1. **Assert the struct offsets in code, not comments.** `_Static_assert(offsetof(HOG_PROCINFO, UniqueProcessId) == 80, ...)` on x64. This is the single most fragile part of the tool and the assertion turns a silent garbage-output failure into a compile error.
2. **32-bit build verification.** The offsets differ on x86 and are currently unverified.
3. **Ctrl+C handling in watch mode.** Right now it dies mid-print. A `SetConsoleCtrlHandler` that sets a flag gives clean output.
4. **`-j` JSON output.** Makes it pipeable into anything, and makes the tests trivial.

## Roadmap

- **Log mode.** Append CSV every N seconds so you can catch the 3am problem. This is arguably the highest-value feature, because the original complaint is about not being there when it happens.
- **Per-thread drill-down.** The thread array is already sitting in the `SystemProcessInformation` buffer immediately after each process record and is currently skipped. `-t PID` would expose which thread is spinning, which is the thing Task Manager genuinely cannot do.
- **Network I/O.** Needs `SystemProcessInformation` supplemented with the TCP/UDP tables from `iphlpapi`, matched by owning PID.
- **Machine-level memory pressure.** Commit charge and standby cache, so you can distinguish "this process is hungry" from "the whole box is out of RAM".
- **Optional service names.** Resolving `svchost.exe` to the services it hosts is the difference between a useless row and an actionable one.

## Kickoff prompt

Paste this into Claude Code in an empty directory, with `src/hog.c` and `README.md` dropped in alongside:

> This directory contains `src/hog.c`, a working single-file Windows CLI tool that samples the process table twice via `NtQuerySystemInformation` and reports which processes are causing a slowdown. `README.md` explains the design.
>
> Restructure it into a proper CMake project: split the ntdll wrapper, snapshot diffing, scoring heuristic and CLI into separate translation units, keeping the pure logic testable without Windows. Add `_Static_assert` offset checks for the undocumented structs on both x86 and x64. Set up GitHub Actions building with MSVC and MinGW, plus a Linux cross-compile smoke test.
>
> Do not change observable behaviour in this pass. Get the structure and CI green first, then we add features.

The "do not change behaviour in this pass" line matters. The tool works today, and separating the restructure from the feature work means any breakage has exactly one possible cause.

## A CLAUDE.md worth having

```md
## hog

Tiny Windows CLI that identifies what is slowing the machine down.

### Non-negotiables

- Binary stays under ~64 KB. No new dependencies without a real argument.
- Startup must stay near-instant. No COM, no WMI, no registry walks on the hot path.
- No process termination. This tool observes, it does not act.
- Anything touching undocumented ntdll structs lives in `ntquery.*` and carries
  a `_Static_assert` for its offsets.

### Testing

Pure logic in `score.c` and `snapshot.c` must build and test on Linux.
Windows-only code stays behind `ntquery.h`.
```
