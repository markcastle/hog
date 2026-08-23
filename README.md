# hog

A single-file Windows CLI tool that answers one question: **what is hammering this machine right now?**

Task Manager is slow to open precisely when you need it, because it is a full WPF app that spins up COM, WMI and a UI thread on a box that is already choking. `hog` does none of that. It makes two calls into `ntdll`, sleeps for a fraction of a second, and prints a table. Cold start to first output is the sample interval plus a couple of milliseconds.

## Build

```
cl /O2 /MT src\hog.c                                :: MSVC
x86_64-w64-mingw32-gcc -O2 -s -o hog.exe src/hog.c  # MinGW
```

No SDK beyond the standard headers, no dependencies, no installer. The result is around 45 KB. Drop it in a folder on your `PATH` and forget about it.

Works on Windows 7 and later, x86 or x64. Admin is not required.

## Usage

```
hog                 top 10 by combined impact score
hog -w              watch mode, re-samples until Ctrl+C
hog -d -n 20        top 20 by disk throughput
hog -i 1000         sample over a full second instead of 400ms
```

Sort flags: `-c` CPU, `-d` disk, `-f` hard page faults, `-m` private memory.

## Reading the output

```
sampled 0.40s over 16 logical CPUs   cpu 62.3%   dpc 1.1%   isr 0.4%

    PID   CPU%  DISK MB/s      IO/s  HARDFLT/s   PRIV MB   THR  NAME
   9184   41.2       0.03         7          0     812.4    48  chrome.exe
   4012    2.1      58.70      1420          0      95.2    12  MsMpEng.exe
```

The header line is the part Task Manager will not show you. `dpc` and `isr` are kernel time spent in driver interrupt handling, which belongs to no process at all. If that number is high, no amount of staring at the process list will help you, because the culprit is a driver. The tool says so explicitly when it crosses 8%.

Columns are all **rates measured across the sample window**, not lifetime totals, with the single exception of `PRIV MB`. That distinction matters: a process with a huge lifetime CPU total might be completely idle at this instant.

`HARDFLT/s` is the one people overlook. Hard page faults are reads from the pagefile, and a machine that feels dead while showing modest CPU is usually hard faulting itself into the ground.

## How it works

Everything comes from one `NtQuerySystemInformation(SystemProcessInformation)` call, which returns a linked chunk of records covering every process on the system: CPU times, I/O transfer counts, fault counts, memory. This is the important design decision. The obvious alternative, `EnumProcesses` plus `OpenProcess` plus `GetProcessTimes` per PID, needs a handle to every process, which is slow, fails on protected processes, and does exactly the kind of per-process work you want to avoid when the box is already struggling.

Two snapshots are taken and matched on `(pid, createTime)` rather than PID alone, because PIDs get recycled and you do not want a dead process's counters attributed to its replacement.

The struct definitions are not in the public SDK. They have been layout-stable since Windows 7 and the offsets are asserted in the source comments, but this is the one part of the tool that could theoretically break on a future Windows release. If it ever does, it will produce obvious nonsense rather than crash.

The process sets itself to `HIGH_PRIORITY_CLASS` on startup, so it still gets scheduled promptly while something else is saturating the CPU.

## The impact score

The default sort uses a blunt heuristic that exists to float the likely culprit to row one, not to be scientifically defensible:

```
score = cpuPct + min(150, diskMBps * 2) + min(150, hardFaults / 10)
```

Roughly: 50 MB/s sustained disk, or 1000 hard faults a second, each counts about as much as pegging a full CPU core's worth of the machine. If your normal baseline is different, the divisors are a two-line edit in `impact_score()`.

## Things it deliberately does not do

- **Kill processes.** Look, then decide, then use `taskkill`. A tool that both diagnoses and kills is a tool you eventually use in a hurry and regret.
- **Show a UI.** Pipe it. `hog -n 50 > snapshot.txt` during an incident is the whole point.
- **Cover network I/O.** That needs a different information class and a lot more code. Disk and CPU cover the overwhelming majority of real "why is this slow" cases.

## Obvious extensions

- Log mode: append a CSV row every N seconds so you can catch the thing that only happens at 3am.
- Add `SystemFileCacheInformation` and commit charge to spot memory pressure at the machine level, not just per process.
- A `-t` flag that drills into per-thread times for a single PID via `SystemProcessInformation`'s thread array, which is already sitting in the buffer and currently ignored.
