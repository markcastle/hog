/*
 * hog.c - find out what is hammering this machine, right now, fast.
 *
 * Takes two snapshots of every process via a single NtQuerySystemInformation
 * call, waits a short interval, then reports the deltas: CPU, disk bytes,
 * hard page faults. No process handles are opened, no WMI, no COM, no UI.
 * Cold start to first output is the sample interval plus a couple of ms.
 *
 * Build (MSVC):  cl /O2 /MT hog.c
 * Build (MinGW): x86_64-w64-mingw32-gcc -O2 -s -o hog.exe hog.c
 *
 * Runs on Windows 7 and later, x86 or x64, no admin required.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* ntdll bits. These structures are not in the public SDK headers but  */
/* have been layout-stable since Windows 7. We declare our own copies  */
/* under private names so nothing collides with winternl.h.            */
/* ------------------------------------------------------------------ */

typedef LONG NT_STATUS;

#define STATUS_INFO_LENGTH_MISMATCH ((NT_STATUS)0xC0000004L)
#define SystemProcessInformation              5
#define SystemProcessorPerformanceInformation 8

typedef struct _HOG_USTR {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} HOG_USTR;

typedef struct _HOG_PROCINFO {
    ULONG         NextEntryOffset;
    ULONG         NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG         HardFaultCount;
    ULONG         NumberOfThreadsHighWatermark;
    ULONGLONG     CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    HOG_USTR      ImageName;
    LONG          BasePriority;
    HANDLE        UniqueProcessId;
    HANDLE        InheritedFromUniqueProcessId;
    ULONG         HandleCount;
    ULONG         SessionId;
    ULONG_PTR     UniqueProcessKey;
    SIZE_T        PeakVirtualSize;
    SIZE_T        VirtualSize;
    ULONG         PageFaultCount;
    SIZE_T        PeakWorkingSetSize;
    SIZE_T        WorkingSetSize;
    SIZE_T        QuotaPeakPagedPoolUsage;
    SIZE_T        QuotaPagedPoolUsage;
    SIZE_T        QuotaPeakNonPagedPoolUsage;
    SIZE_T        QuotaNonPagedPoolUsage;
    SIZE_T        PagefileUsage;
    SIZE_T        PeakPagefileUsage;
    SIZE_T        PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} HOG_PROCINFO;

typedef struct _HOG_CPUPERF {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;      /* includes idle */
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
} HOG_CPUPERF;

typedef NT_STATUS (NTAPI *PFN_NTQSI)(ULONG, PVOID, ULONG, PULONG);
static PFN_NTQSI NtQSI;

/* ------------------------------------------------------------------ */

typedef struct {
    DWORD     pid;
    ULONGLONG created;     /* pid alone is not a stable key, pids recycle */
    ULONGLONG cpu100ns;
    ULONGLONG ioBytes;
    ULONGLONG ioOps;
    ULONG     hardFaults;
    ULONGLONG privBytes;
    ULONG     threads;
    char      name[64];
} REC;

typedef struct {
    REC      *v;
    size_t    n;
    ULONGLONG dpc100ns;
    ULONGLONG intr100ns;
    ULONGLONG idle100ns;
    ULONGLONG busy100ns;   /* kernel + user, idle already removed */
    LONGLONG  qpc;
} SNAP;

/* Sort keys */
enum { SORT_SCORE, SORT_CPU, SORT_DISK, SORT_FAULT, SORT_MEM };
static int g_sort = SORT_SCORE;

/* ------------------------------------------------------------------ */

static void *query(ULONG cls, ULONG *bytesOut)
{
    ULONG cap = 1u << 18;
    void *buf = NULL;

    for (;;) {
        void *nb = realloc(buf, cap);
        if (!nb) { free(buf); return NULL; }
        buf = nb;

        ULONG need = 0;
        NT_STATUS st = NtQSI(cls, buf, cap, &need);

        if (st == STATUS_INFO_LENGTH_MISMATCH) {
            cap = (need > cap ? need : cap * 2) + (1u << 16);
            continue;                      /* processes started mid-call */
        }
        if (st < 0) { free(buf); return NULL; }
        if (bytesOut) *bytesOut = need ? need : cap;
        return buf;
    }
}

static ULONGLONG li(LARGE_INTEGER x) { return (ULONGLONG)x.QuadPart; }

static void wide_to_utf8(const HOG_USTR *u, char *dst, int cap)
{
    if (!u->Buffer || u->Length == 0) { dst[0] = '\0'; return; }
    int chars = u->Length / (int)sizeof(WCHAR);
    int n = WideCharToMultiByte(CP_UTF8, 0, u->Buffer, chars, dst, cap - 1, NULL, NULL);
    dst[n > 0 ? n : 0] = '\0';
}

static int take_snapshot(SNAP *s)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s->qpc = now.QuadPart;

    /* --- per-CPU totals: this is where DPC/ISR time shows up --- */
    s->dpc100ns = s->intr100ns = s->idle100ns = s->busy100ns = 0;
    ULONG cpuBytes = 0;
    HOG_CPUPERF *cp = (HOG_CPUPERF *)query(SystemProcessorPerformanceInformation, &cpuBytes);
    if (cp) {
        size_t count = cpuBytes / sizeof(HOG_CPUPERF);
        for (size_t i = 0; i < count; i++) {
            ULONGLONG idle = li(cp[i].IdleTime);
            ULONGLONG kern = li(cp[i].KernelTime);   /* idle is inside kernel */
            ULONGLONG user = li(cp[i].UserTime);
            s->idle100ns += idle;
            s->busy100ns += (kern > idle ? kern - idle : 0) + user;
            s->dpc100ns  += li(cp[i].DpcTime);
            s->intr100ns += li(cp[i].InterruptTime);
        }
        free(cp);
    }

    /* --- every process, one call, no handles opened --- */
    void *buf = query(SystemProcessInformation, NULL);
    if (!buf) return 0;

    size_t cap = 512, n = 0;
    REC *v = (REC *)malloc(cap * sizeof(REC));
    if (!v) { free(buf); return 0; }

    HOG_PROCINFO *p = (HOG_PROCINFO *)buf;
    for (;;) {
        if (n == cap) {
            cap *= 2;
            REC *nv = (REC *)realloc(v, cap * sizeof(REC));
            if (!nv) break;
            v = nv;
        }
        REC *r = &v[n++];
        r->pid        = (DWORD)(ULONG_PTR)p->UniqueProcessId;
        r->created    = li(p->CreateTime);
        r->cpu100ns   = li(p->UserTime) + li(p->KernelTime);
        r->ioBytes    = li(p->ReadTransferCount) + li(p->WriteTransferCount);
        r->ioOps      = li(p->ReadOperationCount) + li(p->WriteOperationCount);
        r->hardFaults = p->HardFaultCount;
        r->privBytes  = (ULONGLONG)p->PrivatePageCount;
        r->threads    = p->NumberOfThreads;
        wide_to_utf8(&p->ImageName, r->name, sizeof(r->name));
        if (r->name[0] == '\0')
            strcpy(r->name, r->pid == 0 ? "Idle" : "System");

        if (p->NextEntryOffset == 0) break;
        p = (HOG_PROCINFO *)((BYTE *)p + p->NextEntryOffset);
    }

    free(buf);
    s->v = v;
    s->n = n;
    return 1;
}

/* ------------------------------------------------------------------ */

typedef struct {
    DWORD     pid;
    char      name[64];
    double    cpuPct;      /* share of total machine capacity */
    double    diskMBps;
    double    iops;
    double    faultsPerSec;
    double    privMB;
    ULONG     threads;
    double    score;
} ROW;

static int cmp_rows(const void *a, const void *b)
{
    const ROW *x = (const ROW *)a, *y = (const ROW *)b;
    double dx, dy;
    switch (g_sort) {
        case SORT_CPU:   dx = x->cpuPct;       dy = y->cpuPct;       break;
        case SORT_DISK:  dx = x->diskMBps;     dy = y->diskMBps;     break;
        case SORT_FAULT: dx = x->faultsPerSec; dy = y->faultsPerSec; break;
        case SORT_MEM:   dx = x->privMB;       dy = y->privMB;       break;
        default:         dx = x->score;        dy = y->score;        break;
    }
    if (dx < dy) return 1;
    if (dx > dy) return -1;
    return 0;
}

/*
 * The score is a deliberate, blunt heuristic: it exists to float the
 * likely culprit to row one, not to be scientifically defensible.
 *   - CPU is scored as its raw percentage of the whole machine.
 *   - Disk throughput is scored so 50 MB/s sustained is worth ~100.
 *   - Hard faults are scored so 1000/sec is worth ~100, because heavy
 *     hard faulting is what actually makes a PC feel dead.
 * Tune the divisors if your machine's normal baseline differs.
 */
static double impact_score(double cpuPct, double diskMBps, double faultsPerSec)
{
    double d = diskMBps * 2.0;
    double f = faultsPerSec / 10.0;
    if (d > 150.0) d = 150.0;
    if (f > 150.0) f = 150.0;
    return cpuPct + d + f;
}

static int find_prev(const SNAP *prev, DWORD pid, ULONGLONG created, size_t hint)
{
    /* Order is near-identical between snapshots, so start at the hint. */
    for (size_t k = 0; k < prev->n; k++) {
        size_t i = (hint + k) % prev->n;
        if (prev->v[i].pid == pid && prev->v[i].created == created) return (int)i;
    }
    return -1;
}

static void report(const SNAP *a, const SNAP *b, int topN, double freq, int nproc)
{
    double secs = (double)(b->qpc - a->qpc) / freq;
    if (secs <= 0.0) secs = 0.001;
    double machine100ns = secs * 1e7 * (double)nproc;

    ROW *rows = (ROW *)malloc(b->n * sizeof(ROW));
    if (!rows) return;
    size_t nr = 0;

    for (size_t i = 0; i < b->n; i++) {
        const REC *cur = &b->v[i];
        int j = find_prev(a, cur->pid, cur->created, i);
        if (j < 0) continue;                   /* born during the interval */
        const REC *old = &a->v[j];

        ROW *r = &rows[nr];
        r->pid = cur->pid;
        strcpy(r->name, cur->name);
        r->threads = cur->threads;
        r->privMB  = (double)cur->privBytes / (1024.0 * 1024.0);

        ULONGLONG dcpu = cur->cpu100ns > old->cpu100ns ? cur->cpu100ns - old->cpu100ns : 0;
        ULONGLONG dio  = cur->ioBytes  > old->ioBytes  ? cur->ioBytes  - old->ioBytes  : 0;
        ULONGLONG dops = cur->ioOps    > old->ioOps    ? cur->ioOps    - old->ioOps    : 0;
        ULONG     dhf  = cur->hardFaults > old->hardFaults ? cur->hardFaults - old->hardFaults : 0;

        r->cpuPct       = 100.0 * (double)dcpu / machine100ns;
        r->diskMBps     = ((double)dio / (1024.0 * 1024.0)) / secs;
        r->iops         = (double)dops / secs;
        r->faultsPerSec = (double)dhf / secs;
        r->score        = impact_score(r->cpuPct, r->diskMBps, r->faultsPerSec);

        if (r->pid == 0) continue;             /* Idle is not interesting */
        nr++;
    }

    qsort(rows, nr, sizeof(ROW), cmp_rows);

    /* Machine-level line. DPC and ISR time belongs to drivers, not to any
       process, which is exactly the case Task Manager hides from you. */
    double busy = 100.0 * (double)(b->busy100ns - a->busy100ns) / machine100ns;
    double dpc  = 100.0 * (double)(b->dpc100ns  - a->dpc100ns)  / machine100ns;
    double isr  = 100.0 * (double)(b->intr100ns - a->intr100ns) / machine100ns;

    printf("sampled %.2fs over %d logical CPUs   cpu %.1f%%   dpc %.1f%%   isr %.1f%%\n",
           secs, nproc, busy, dpc, isr);
    if (dpc + isr > 8.0)
        printf("  note: high kernel DPC/ISR time. Suspect a driver, not a process.\n");
    printf("\n%7s %6s %10s %9s %10s %9s %5s  %s\n",
           "PID", "CPU%", "DISK MB/s", "IO/s", "HARDFLT/s", "PRIV MB", "THR", "NAME");

    int shown = 0;
    for (size_t i = 0; i < nr && shown < topN; i++, shown++) {
        ROW *r = &rows[i];
        printf("%7lu %6.1f %10.2f %9.0f %10.0f %9.1f %5lu  %s\n",
               (unsigned long)r->pid, r->cpuPct, r->diskMBps, r->iops,
               r->faultsPerSec, r->privMB, (unsigned long)r->threads, r->name);
    }
    free(rows);
}

static void free_snap(SNAP *s) { free(s->v); s->v = NULL; s->n = 0; }

static void usage(void)
{
    printf(
      "hog - what is slowing this machine down, right now\n\n"
      "  hog [-n TOP] [-i MS] [-w] [-c|-d|-f|-m]\n\n"
      "  -n TOP   rows to print (default 10)\n"
      "  -i MS    sample interval in ms (default 400, minimum 50)\n"
      "  -w       watch: keep sampling until Ctrl+C\n"
      "  -c       sort by CPU\n"
      "  -d       sort by disk throughput\n"
      "  -f       sort by hard page faults\n"
      "  -m       sort by private memory\n"
      "           default sorts by a combined impact score\n");
}

int main(int argc, char **argv)
{
    int topN = 10, interval = 400, watch = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) topN = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w")) watch = 1;
        else if (!strcmp(argv[i], "-c")) g_sort = SORT_CPU;
        else if (!strcmp(argv[i], "-d")) g_sort = SORT_DISK;
        else if (!strcmp(argv[i], "-f")) g_sort = SORT_FAULT;
        else if (!strcmp(argv[i], "-m")) g_sort = SORT_MEM;
        else { usage(); return 0; }
    }
    if (topN < 1) topN = 1;
    if (interval < 50) interval = 50;

    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    NtQSI = nt ? (PFN_NTQSI)(void *)GetProcAddress(nt, "NtQuerySystemInformation") : NULL;
    if (!NtQSI) { fprintf(stderr, "hog: cannot reach NtQuerySystemInformation\n"); return 1; }

    SetConsoleOutputCP(CP_UTF8);

    /* Stay responsive even while the box is being hammered. */
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double freq = (double)f.QuadPart;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nproc = (int)si.dwNumberOfProcessors;
    if (nproc < 1) nproc = 1;

    SNAP a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);

    if (!take_snapshot(&a)) { fprintf(stderr, "hog: snapshot failed\n"); return 1; }

    do {
        Sleep((DWORD)interval);
        if (!take_snapshot(&b)) { fprintf(stderr, "hog: snapshot failed\n"); return 1; }
        report(&a, &b, topN, freq, nproc);
        if (watch) printf("\n");
        free_snap(&a);
        a = b;
        memset(&b, 0, sizeof b);
    } while (watch);

    free_snap(&a);
    return 0;
}
