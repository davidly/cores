/* cores.c : tiny per-physical-core clock/throttle meter for MSVC

   Build:
       cl /O2 /W4 cores.c user32.lib gdi32.lib pdh.lib

   Shows one square per physical core.
     blue  = reduced clock / likely idle
     green = high current clock
     red   = the core is clearly active but sitting well below either
             its OS-reported performance limit or its own best-ever
             clock this run (thermal/power throttling)

   The popup window is borderless, topmost, and draggable by clicking anywhere.

   This used to read CallNtPowerInformation(ProcessorInformation, ...) /
   PROCESSOR_POWER_INFORMATION. On modern CPUs with hardware-autonomous
   P-states (Intel Speed Shift/HWP, AMD CPPC - which is any current
   Intel/AMD desktop chip), that API's MaxMhz/CurrentMhz/MhzLimit fields
   are frozen at the nominal base clock and never move, so every core
   always looked "green" regardless of real load. The performance-counter
   based "Processor Information" object (backed by the APERF/MPERF MSRs
   the hardware actually updates) still tracks real frequency scaling, so
   this reads that instead via PDH.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

/* Remembers window position across runs, the same way
   \users\david\onedrive\cpu\cpp\cpu.cxx does: HKEY_CURRENT_USER registry
   value, read on startup (unless a command-line argument is given, which
   overrides with the default position) and written back on exit. */
#define REGISTRY_APP_NAME "SOFTWARE\\davidlycores"
#define REGISTRY_WINDOW_POSITION "WindowPosition"

typedef struct CoreMap {
    DWORD mask;      /* group 0 only; sufficient for normal desktop machines */
    BYTE effClass;   /* PROCESSOR_RELATIONSHIP.EfficiencyClass: higher = more
                        performant (e.g. P-core), lower = more power-efficient
                        (e.g. E-core). Same value on non-hybrid CPUs. The API
                        allows any number of distinct classes, not just two,
                        for future CPUs with more than P/E core tiers. */
} CoreMap;

typedef struct LogicalCounters {
    PDH_HCOUNTER freqPct;   /* \Processor Information(0,N)\% of Maximum Frequency */
    PDH_HCOUNTER limitPct;  /* \Processor Information(0,N)\% Performance Limit */
    int valid;
} LogicalCounters;

static CoreMap *g_core = NULL;
static int g_cores = 0;
static int g_logical = 0;
static int g_minEff = 255;  /* lowest EfficiencyClass seen among physical cores */
static int g_maxEff = 0;   /* highest EfficiencyClass seen among physical cores */
static PDH_HQUERY g_query = NULL;
static LogicalCounters *g_lc = NULL;
static double *g_freq = NULL;   /* per logical processor, latest % of Maximum Frequency */
static double *g_limit = NULL;  /* per logical processor, latest % Performance Limit */
static double *g_peak = NULL;   /* per logical processor, highest % of Maximum Frequency seen this run */
static int *g_redstreak = NULL; /* per logical processor, consecutive polls the throttle condition held */

/* Consecutive 200ms polls the throttle condition must hold before a core
   is shown red - filters out brief one-tick frequency blips (background
   task scheduling, a stray interrupt) at idle that would otherwise trip
   the peak-drop fallback for a single sample without any sustained load
   behind it. */
#define REDSTREAK_MIN 5

/* Saturation applied to the least performant (most power-efficient) core
   type's fill color; the most performant core type always gets full (1.0)
   saturation, and any core types in between are scaled linearly by their
   EfficiencyClass. Not 0.0, so even the slowest cores stay recognizably
   colored rather than going fully gray. */
#define MIN_SATURATION 0.35

static int popcount32(DWORD x)
{
    int n = 0;
    while (x) {
        x &= x - 1;
        n++;
    }
    return n;
}

static void fatal_exit(void)
{
    ExitProcess(1);
}

static void load_window_position(int *posLeft, int *posTop)
{
    HKEY hkey;
    char buf[64];
    DWORD size = sizeof(buf);
    DWORD type = 0;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, REGISTRY_APP_NAME, 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hkey, REGISTRY_WINDOW_POSITION, NULL, &type, (BYTE *) buf, &size) == ERROR_SUCCESS &&
            type == REG_SZ) {
            int l, t;

            buf[(size < sizeof(buf)) ? size : sizeof(buf) - 1] = 0;
            if (sscanf_s(buf, "%d %d", &l, &t) == 2) {
                *posLeft = l;
                *posTop = t;
            }
        }
        RegCloseKey(hkey);
    }
}

static void save_window_position(HWND hwnd)
{
    HKEY hkey;
    RECT r;
    char buf[64];
    int len;

    GetWindowRect(hwnd, &r);
    len = wsprintfA(buf, "%d %d", r.left, r.top);

    if (RegCreateKeyExA(HKEY_CURRENT_USER, REGISTRY_APP_NAME, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hkey, REGISTRY_WINDOW_POSITION, 0, REG_SZ, (const BYTE *) buf, (DWORD) len + 1);
        RegCloseKey(hkey);
    }
}

static void build_core_map(void)
{
    DWORD len = 0;
    DWORD off;
    BYTE *buf;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX p;

    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);

    buf = (BYTE *) malloc(len);
    if (!buf)
        fatal_exit();

    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) buf,
                                          &len)) {
        free(buf);
        fatal_exit();
    }

    for (off = 0; off < len; off += p->Size) {
        p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) (buf + off);
        if (p->Relationship == RelationProcessorCore &&
            p->Processor.GroupCount > 0 &&
            p->Processor.GroupMask[0].Group == 0) {
            g_cores++;
        }
    }

    g_core = (CoreMap *) calloc((size_t) g_cores, sizeof(CoreMap));
    if (!g_core) {
        free(buf);
        fatal_exit();
    }

    g_cores = 0;
    g_logical = 0;

    for (off = 0; off < len; off += p->Size) {
        p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) (buf + off);
        if (p->Relationship == RelationProcessorCore &&
            p->Processor.GroupCount > 0 &&
            p->Processor.GroupMask[0].Group == 0) {
            g_core[g_cores].mask = (DWORD) p->Processor.GroupMask[0].Mask;
            g_core[g_cores].effClass = p->Processor.EfficiencyClass;
            g_logical += popcount32(g_core[g_cores].mask);
            g_cores++;

            if (p->Processor.EfficiencyClass < g_minEff)
                g_minEff = p->Processor.EfficiencyClass;
            if (p->Processor.EfficiencyClass > g_maxEff)
                g_maxEff = p->Processor.EfficiencyClass;
        }
    }

    free(buf);

    if (g_cores == 0 || g_logical == 0)
        fatal_exit();
}

static void build_counters(void)
{
    int i;
    char path[128];

    if (PdhOpenQuery(NULL, 0, &g_query) != ERROR_SUCCESS)
        fatal_exit();

    g_lc = (LogicalCounters *) calloc((size_t) g_logical, sizeof(*g_lc));
    g_freq = (double *) calloc((size_t) g_logical, sizeof(double));
    g_limit = (double *) calloc((size_t) g_logical, sizeof(double));
    g_peak = (double *) calloc((size_t) g_logical, sizeof(double));
    g_redstreak = (int *) calloc((size_t) g_logical, sizeof(int));
    if (!g_lc || !g_freq || !g_limit || !g_peak || !g_redstreak)
        fatal_exit();

    for (i = 0; i < g_logical; i++) {
        wsprintfA(path, "\\Processor Information(0,%d)\\%% of Maximum Frequency", i);
        g_lc[i].valid = (PdhAddEnglishCounterA(g_query, path, 0, &g_lc[i].freqPct) == ERROR_SUCCESS);

        wsprintfA(path, "\\Processor Information(0,%d)\\%% Performance Limit", i);
        if (g_lc[i].valid)
            g_lc[i].valid = (PdhAddEnglishCounterA(g_query, path, 0, &g_lc[i].limitPct) == ERROR_SUCCESS);

        g_limit[i] = 100.0; /* no data yet: assume unrestricted, not throttled */
    }

    /* PDH counters need one collection before they hold real (not
       PDH_CSTATUS_INVALID_DATA) values - prime it now so the first
       WM_PAINT already has something sensible to draw. */
    PdhCollectQueryData(g_query);
}

/* Blends c toward its own gray (luma-preserving) equivalent by sat (1.0 =
   unchanged, 0.0 = fully gray) - a simple RGB-space saturation reduction
   that avoids a full HSV round-trip. */
static COLORREF desaturate(COLORREF c, double sat)
{
    int r = GetRValue(c);
    int g = GetGValue(c);
    int b = GetBValue(c);
    double gray = 0.299 * r + 0.587 * g + 0.114 * b;
    int nr = (int) (gray + (r - gray) * sat);
    int ng = (int) (gray + (g - gray) * sat);
    int nb = (int) (gray + (b - gray) * sat);

    if (nr < 0) nr = 0; else if (nr > 255) nr = 255;
    if (ng < 0) ng = 0; else if (ng > 255) ng = 255;
    if (nb < 0) nb = 0; else if (nb > 255) nb = 255;

    return RGB(nr, ng, nb);
}

static COLORREF core_color(int c)
{
    double freq = -1.0;    /* highest % of Maximum Frequency among sibling threads */
    int bit;
    int any = 0;
    int throttled = 0;
    COLORREF base;

    for (bit = 0; bit < g_logical && bit < 32; bit++) {
        if (g_core[c].mask & ((DWORD) 1u << bit)) {
            if (!g_lc[bit].valid)
                continue;

            any = 1;

            if (g_freq[bit] > freq)
                freq = g_freq[bit];

            if (g_redstreak[bit] >= REDSTREAK_MIN)
                throttled = 1;
        }
    }

    if (!any)
        base = RGB(40, 40, 40);
    /* Red: a sibling thread has held the throttle condition (see
       poll_power) for REDSTREAK_MIN consecutive polls - sustained, not a
       one-tick blip. */
    else if (throttled)
        base = RGB(220, 0, 0);
    /* Green: running near normal/high clock. */
    else if (freq >= 65.0)
        base = RGB(0, 180, 0);
    /* Blue: reduced clock / likely idle. */
    else
        base = RGB(0, 80, 220);

    /* Distinguish core types (P/E/etc.) by saturation: the most performant
       type stays full saturation, the least performant gets MIN_SATURATION,
       and anything in between (systems with more than two tiers) is scaled
       linearly by EfficiencyClass. g_maxEff == g_minEff on non-hybrid CPUs
       (all cores report the same class), so this is a no-op there. */
    if (g_maxEff > g_minEff) {
        double t = (double) (g_core[c].effClass - g_minEff) / (double) (g_maxEff - g_minEff);
        double sat = MIN_SATURATION + t * (1.0 - MIN_SATURATION);
        base = desaturate(base, sat);
    }

    return base;
}

static void poll_power(HWND hwnd)
{
    int i;
    PDH_FMT_COUNTERVALUE val;

    if (PdhCollectQueryData(g_query) != ERROR_SUCCESS)
        return;

    for (i = 0; i < g_logical; i++) {
        if (!g_lc[i].valid)
            continue;

        if (PdhGetFormattedCounterValue(g_lc[i].freqPct, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS) {
            g_freq[i] = val.doubleValue;
            if (g_freq[i] > g_peak[i])
                g_peak[i] = g_freq[i];
        }

        if (PdhGetFormattedCounterValue(g_lc[i].limitPct, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS)
            g_limit[i] = val.doubleValue;

        /* This tick's raw throttle condition: clearly active (freq >= 50)
           while either -
             (a) the OS reports an explicit thermal/power cap (% Performance
                 Limit below 100), or
             (b) it's sitting well below its OWN best-ever reading this
                 run - a self-calibrating fallback for systems whose
                 driver/BIOS never populates % Performance Limit at all
                 (it stays pinned at 100 even under a genuine sustained
                 all-core load, so (a) alone can never fire there).
           Only counts toward g_redstreak here; core_color() requires
           REDSTREAK_MIN consecutive ticks before showing red, so a
           single-tick blip (a brief background task waking a supposedly
           idle core) never turns a box red by itself. */
        if (g_freq[i] >= 50.0 &&
            (g_limit[i] < 90.0 || (g_peak[i] >= 65.0 && g_freq[i] < g_peak[i] * 0.80)))
            g_redstreak[i]++;
        else
            g_redstreak[i] = 0;
    }

    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 200, NULL); /* 5 Hz */
        return 0;

    case WM_TIMER:
        poll_power(hwnd);
        return 0;

    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProc(hwnd, msg, wp, lp);
        if (hit == HTCLIENT)
            return HTCAPTION;   /* click/drag anywhere in the client area */
        return hit;
    }

    case WM_LBUTTONDBLCLK:
        DestroyWindow(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        int h;
        int gap;
        int i;
        int x;
        int y;
        int cell;
        HBRUSH br;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        h = rc.bottom - rc.top;
        gap = 2;
        cell = h - 2 * gap;
        if (cell < 2)
            cell = 2;

        x = gap;
        y = gap;

        br = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        for (i = 0; i < g_cores; i++) {
            RECT r;

            br = CreateSolidBrush(core_color(i));

            r.left = x;
            r.top = y;
            r.right = x + cell;
            r.bottom = y + cell;

            FillRect(hdc, &r, br);
            DeleteObject(br);

            x += cell + gap;
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        save_window_position(hwnd);
        if (g_query)
            PdhCloseQuery(g_query);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    UNREFERENCED_PARAMETER( show );
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    int deskh;
    int cell;
    int gap;
    int width;
    int height;
    int posLeft;
    int posTop;

    (void) hp;

    build_core_map();
    build_counters();

    deskh = GetSystemMetrics(SM_CYSCREEN);
    cell = (deskh * 6) / 1000;  /* 0.4% of desktop height - 1/4 the original 1.6% */
    if (cell < 4)
        cell = 4;

    gap = 2;
    height = cell + 2 * gap;
    width = g_cores * (cell + gap) + gap;  /* same gap margin on both sides */

    posLeft = 40;
    posTop = 40;

    /* Any command-line argument overrides the registry setting with this
       default position, matching cpu.cxx's convention. */
    if (0 == cmd[0])
        load_window_position(&posLeft, &posTop);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = "CoreMeterWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;

    if (!RegisterClassA(&wc))
        fatal_exit();

    hwnd = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, "Core Meter", WS_POPUP, posLeft, posTop, width, height, NULL, NULL, hi, NULL);

    if (!hwnd)
        fatal_exit();

    // show the window, but don't steal the focus, since nobody wants to type in this app

    
    ShowWindow( hwnd, /*show |*/ SW_SHOWNOACTIVATE );
    UpdateWindow(hwnd);
    poll_power(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
