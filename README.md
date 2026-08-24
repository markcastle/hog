# hog

Your machine just turned into molasses. Task Manager is spinning up forever. You need an answer **now**.

**hog** is a tiny Windows CLI that answers one question: *what is hammering this box right now?*

No WPF. No COM. No WMI. Two calls into `ntdll`, a short nap, one table. Cold start to first output is basically just the sample interval.

~45 KB. No installer. No admin. Windows 7+.

---

## ⚡ Quick start

### 1. Build it

Pick whichever compiler you already have:

```bat
:: MSVC (Developer Command Prompt)
cl /O2 /MT src\hog.c

:: MinGW
x86_64-w64-mingw32-gcc -O2 -s -o hog.exe src/hog.c
```

Drop `hog.exe` somewhere on your `PATH`. Done.

### 2. Run it when things feel wrong

```bat
hog
```

You get the top 10 processes by a combined "who is the villain?" score. That's usually enough.

### 3. Dig a little when you need to

```bat
hog -w              :: live view (htop-style), Ctrl+C to quit
hog -d -n 20        :: top 20 by disk throughput
hog -i 1000         :: sample a full second instead of 400ms
hog -c              :: sort by CPU
hog -f              :: sort by hard page faults
hog -m              :: sort by private memory
```

Incident tip: `hog -n 50 > snapshot.txt` while the pain is happening. Look later. Don't panic-kill from a guess.

---

## 👀 What you're looking at

```
sampled 0.40s over 16 logical CPUs   cpu 62.3%   dpc 1.1%   isr 0.4%

    PID   CPU%  DISK MB/s      IO/s  HARDFLT/s   PRIV MB   THR  NAME
   9184   41.2       0.03         7          0     812.4    48  chrome.exe
   4012    2.1      58.70      1420          0      95.2    12  MsMpEng.exe
```

### The header (the part Task Manager hides from you)

| Field | Plain English |
| --- | --- |
| `cpu` | Whole-machine CPU busy during the sample |
| `dpc` / `isr` | Kernel time stuck in driver interrupt work — **belongs to no process** |

If `dpc`/`isr` climbs past ~8%, hog tells you outright. Staring harder at the process list won't help. Suspect a driver.

### The columns

Almost everything is a **rate over the sample window**, not a lifetime total. `PRIV MB` is the exception (current private memory).

That matters: a process with a huge lifetime CPU number can be completely idle *right now*.

| Column | What it means |
| --- | --- |
| `CPU%` | Share of the machine's CPU this process ate during the sample |
| `DISK MB/s` | Disk bytes read+written, as MB/s |
| `IO/s` | Disk I/O operations per second |
| `HARDFLT/s` | Hard page faults/sec (reads from the pagefile) |
| `PRIV MB` | Private memory right now |
| `THR` | Thread count |
| `NAME` | Process image name |

`HARDFLT/s` is the one people overlook. Modest CPU + a machine that feels dead? Often it's thrashing the pagefile.

---

## 🧠 How it works

```mermaid
flowchart LR
  A[Snapshot A<br/>all processes] --> B[Sleep<br/>default 400ms]
  B --> C[Snapshot B<br/>all processes]
  C --> D[Match on<br/>pid + createTime]
  D --> E[Diff counters<br/>CPU · disk · faults]
  E --> F[Score & sort]
  F --> G[Print table]
```

One `NtQuerySystemInformation(SystemProcessInformation)` call returns every process: CPU times, I/O counts, faults, memory. No `OpenProcess` per PID, so protected processes don't break the run, and a dying machine doesn't get poked to death.

Processes are matched on `(pid, createTime)`, not PID alone — Windows recycles PIDs, and you don't want a dead process's counters blamed on whoever got the number next.

hog bumps itself to `HIGH_PRIORITY_CLASS` so it still gets scheduled while something else is saturating the CPU.

> The ntdll structs aren't in the public SDK. They've been layout-stable since Windows 7. If a future Windows release ever breaks them, you'll get obvious nonsense rather than a crash.

---

## 🎯 The impact score

Default sort is a blunt heuristic. Job one: float the likely culprit to row one. Not peer-reviewed science.

```text
score = cpuPct
      + min(150, diskMBps * 2)
      + min(150, hardFaults / 10)
```

Roughly: **50 MB/s sustained disk**, or **~1000 hard faults/sec**, each counts about as much as pegging a full core's worth of the machine.

Hate the weights? They're a two-line edit in `impact_score()`.

---

## ⌨️ Flags (cheat sheet)

| Flag | Default | Meaning |
| --- | --- | --- |
| `-n TOP` | `10` | How many rows to print |
| `-i MS` | `400` | Sample interval (minimum 50) |
| `-w` | off | Live in-place refresh (htop-style) until Ctrl+C |
| `-c` | — | Sort by CPU |
| `-d` | — | Sort by disk throughput |
| `-f` | — | Sort by hard page faults |
| `-m` | — | Sort by private memory |

No sort flag → combined impact score.

---

## 🚫 What hog will not do

- **Kill processes.** Look, then decide, then `taskkill`. A tool that diagnoses *and* kills is a tool you eventually regret at 2am.
- **Show a UI.** Pipe it. Snapshot it. Stay light.
- **Cover network I/O.** Different APIs, more code. Disk + CPU catch most real "why is this slow?" cases.

---

## ✅ Requirements

- Windows 7 or later (x86 or x64)
- No admin required
- No dependencies beyond the normal Windows headers
- Binary lands around **45 KB**

---

## 🧩 Ideas on the shelf

- Log mode: append a CSV row every N seconds for the 3am ghost
- Machine-level memory pressure (commit charge / file cache), not just per-process
- `-t PID` thread drill-down (the thread array is already in the buffer; hog currently ignores it)
