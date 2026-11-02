/* ddrawlog: a stand-in ddraw.dll for Railroad Tycoon II under CrossOver on a Mac. It logs what the game asks
   DirectDraw for and passes the calls to the real DirectDraw (Wine's), changing only what the game needs to run.
   Put it in the game folder with ddraw=native,builtin; the log is ddrawlog.txt beside it. It patches the method
   tables of the objects DirectDraw hands back, so it needs no knowledge of the game's own code.

   Display layer (v1.2): the game's full-screen mode change is answered as done but never made, so the Mac's display
   keeps its own resolution. The game draws into an ordinary picture of the size it asked for (640x480, 800x600 or
   1024x768), and a separate thread shows that picture scaled up to fill the screen, keeping its 4:3 shape, through
   OpenGL (GDI if OpenGL can't start). Mouse positions are translated between the screen and the game's picture.
   CrossOver's Mac driver defers real mode changes until the window is activated and then takes over every display,
   which goes black on displays without that mode; with no mode change, that never happens.
   DDRAWLOG_NOSCALE=1 turns the display layer off (v1.1 behaviour); DDRAWLOG_GDI=1 skips OpenGL. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static FILE *lg;
static void L(const char *fmt, ...)
{
    if (!lg) return;
    va_list a; va_start(a, fmt);
    fprintf(lg, "%8lu ", GetTickCount()); vfprintf(lg, fmt, a); fputc('\n', lg); fflush(lg);
    va_end(a);
}
static int env_is(const char *name)
{
    char v[8];
    return GetEnvironmentVariableA(name, v, sizeof v) && v[0] == '1';
}

/* Original method pointers, per method table (Wine uses one table per interface version). */
typedef struct { void **vt; void *orig[48]; int kind; } table;
static table tables[16]; static int ntables;
static void *orig(void *obj, int idx)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < ntables; i++) if (tables[i].vt == vt) return tables[i].orig[idx];
    return vt[idx];
}
static int kind_of(void *obj)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < ntables; i++) if (tables[i].vt == vt) return tables[i].kind;
    return -1;
}
static table *add_table(void *obj, int kind)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < ntables; i++) if (tables[i].vt == vt) return NULL;   /* already patched */
    if (ntables == 16) return NULL;
    table *t = &tables[ntables++]; t->vt = vt; t->kind = kind;
    for (int i = 0; i < 48; i++) t->orig[i] = vt[i];
    return t;
}
static void patch(table *t, int idx, void *fn)
{
    DWORD old; VirtualProtect(&t->vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    t->vt[idx] = fn; VirtualProtect(&t->vt[idx], sizeof(void *), old, &old);
}

/* Surface descriptions (DDSURFACEDESC and DDSURFACEDESC2 share these offsets). */
static void desc_str(const BYTE *d, char *out)
{
    if (!d) { strcpy(out, "(null)"); return; }
    sprintf(out, "size=%lu flags=%#lx %lux%lu caps=%#lx bpp=%lu", *(DWORD *)(d), *(DWORD *)(d + 4),
            *(DWORD *)(d + 12), *(DWORD *)(d + 8), *(DWORD *)(d + 104), *(DWORD *)(d + 84));
}
/* The pixel format of a display mode, at offset 72 of a surface description. */
static void fill_format(BYTE *d, DWORD bpp)
{
    *(DWORD *)(d + 72) = 32;                                      /* DDPIXELFORMAT */
    *(DWORD *)(d + 76) = bpp == 8 ? 0x40 | 0x20 : 0x40;           /* RGB (| PALETTEINDEXED8) */
    *(DWORD *)(d + 84) = bpp;
    if (bpp == 16) { *(DWORD *)(d + 88) = 0xf800; *(DWORD *)(d + 92) = 0x07e0; *(DWORD *)(d + 96) = 0x001f; }
    if (bpp == 32) { *(DWORD *)(d + 88) = 0xff0000; *(DWORD *)(d + 92) = 0xff00; *(DWORD *)(d + 96) = 0xff; }
}
static void fill_mode(BYTE *d, DWORD w, DWORD h, DWORD bpp)
{
    memset(d, 0, 108);
    *(DWORD *)(d) = 108;
    *(DWORD *)(d + 4) = 0x2 | 0x4 | 0x8 | 0x1000 | 0x40000;      /* HEIGHT|WIDTH|PITCH|PIXELFORMAT|REFRESHRATE */
    *(DWORD *)(d + 8) = h; *(DWORD *)(d + 12) = w; *(LONG *)(d + 16) = w * bpp / 8;
    *(DWORD *)(d + 24) = 0;                                       /* refresh: default */
    fill_format(d, bpp);
}

/* ---- display layer state ---- */
static int scaling = -1;                  /* display layer on (decided at the first DirectDrawCreate) */
static CRITICAL_SECTION cs;
static DWORD mode_w = 640, mode_h = 480, mode_bpp = 8; static int mode_set;
static HWND game;                         /* the window the game went full screen with */
static RECT mon;                          /* the screen it fills, in screen coordinates */
static void *primary;                     /* the stand-in for the game's screen */
static DWORD prim_w, prim_h, prim_bpp;
static BYTE *shadow;                      /* a copy of its pixels, read by the presenting thread */
static DWORD pal[256];                    /* its palette (8-bit), as BGRA */
static volatile LONG dirty;
static HANDLE wake, presenter_thread;

static int active(void) { return scaling > 0 && game && mode_set; }
static volatile LONG game_in_front = 1;   /* the game is the app in front (WM_ACTIVATEAPP) */

/* Where the picture sits in a client area of cw x ch: as large as fits, 4:3 kept, centred. */
static void picture_in(int cw, int ch, DWORD w, DWORD h, RECT *r)
{
    int dw = cw, dh = MulDiv(cw, h, w);
    if (dh > ch) { dh = ch; dw = MulDiv(ch, w, h); }
    r->left = (cw - dw) / 2; r->top = (ch - dh) / 2; r->right = r->left + dw; r->bottom = r->top + dh;
}
static void picture(RECT *r) { picture_in(mon.right - mon.left, mon.bottom - mon.top, mode_w, mode_h, r); }
/* A point in the game window's client area (the whole screen) to the game's picture, and back. */
static void to_game(POINT *p)
{
    RECT r; picture(&r);
    LONG x = MulDiv(p->x - r.left, mode_w, r.right - r.left), y = MulDiv(p->y - r.top, mode_h, r.bottom - r.top);
    p->x = x < 0 ? 0 : x >= (LONG)mode_w ? (LONG)mode_w - 1 : x;
    p->y = y < 0 ? 0 : y >= (LONG)mode_h ? (LONG)mode_h - 1 : y;
}
static void from_game(POINT *p)
{
    RECT r; picture(&r);
    p->x = r.left + MulDiv(p->x, r.right - r.left, mode_w); p->y = r.top + MulDiv(p->y, r.bottom - r.top, mode_h);
}

/* ---- the game's window: kept covering the screen, mouse messages translated ---- */
static struct { HWND w; WNDPROC proc; } subclassed[4]; static int nsubclassed;
static LRESULT CALLBACK game_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    WNDPROC proc = NULL;
    for (int i = 0; i < nsubclassed; i++) if (subclassed[i].w == w) proc = subclassed[i].proc;
    if (active() && w == game)
    {
        if (m == WM_WINDOWPOSCHANGING && !IsIconic(w))
        {
            WINDOWPOS *p = (WINDOWPOS *)lp;
            p->x = mon.left; p->y = mon.top; p->cx = mon.right - mon.left; p->cy = mon.bottom - mon.top;
            p->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
        else if (m == WM_WINDOWPOSCHANGED)                          /* the game sees its window as its screen */
        {
            WINDOWPOS p = *(WINDOWPOS *)lp;
            p.x = p.y = 0; p.cx = mode_w; p.cy = mode_h;
            return CallWindowProcA(proc, w, m, wp, (LPARAM)&p);
        }
        else if (m == WM_SIZE && wp != SIZE_MINIMIZED) lp = MAKELPARAM(mode_w, mode_h);
        else if (m == WM_ACTIVATEAPP) game_in_front = wp != 0;
        else if (m == WM_MOVE) lp = 0;
        else if (m == WM_MOUSEWHEEL || m == 0x020e)                 /* screen coordinates */
        {
            POINT p = { (short)LOWORD(lp) - mon.left, (short)HIWORD(lp) - mon.top }; to_game(&p);
            lp = MAKELPARAM(p.x, p.y);
        }
        else if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST)           /* client coordinates */
        {
            POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) }; to_game(&p);
            lp = MAKELPARAM(p.x, p.y);
        }
    }
    return proc ? CallWindowProcA(proc, w, m, wp, lp) : DefWindowProcA(w, m, wp, lp);
}
static void fill_screen(void)
{
    SetWindowLongA(game, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
    SetWindowLongA(game, GWL_EXSTYLE, GetWindowLongA(game, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME));
    SetWindowPos(game, HWND_TOP, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* ---- the game's own calls about the mouse and the screen: answered in the game's picture ---- */
static int calls[8];
#define FIRST(i, ...) do { if (calls[i]++ < 3) L(__VA_ARGS__); } while (0)
/* While another app is in front, or the pointer is on another screen, the game keeps the last position it had:
   otherwise the pointer would sit on the picture's edge and the game would scroll as if pushed against it. */
static POINT last_pos;
static BOOL WINAPI h_GetCursorPos(POINT *p)
{
    BOOL ok = GetCursorPos(p);
    if (ok && active())
    {
        if (!game_in_front || !PtInRect(&mon, *p)) { *p = last_pos; return ok; }
        p->x -= mon.left; p->y -= mon.top; to_game(p); last_pos = *p;
    }
    FIRST(0, "game GetCursorPos -> %ld,%ld", p ? p->x : 0, p ? p->y : 0);
    return ok;
}
static BOOL WINAPI h_SetCursorPos(int x, int y)
{
    FIRST(1, "game SetCursorPos %d,%d", x, y);
    if (active()) { POINT p = { x, y }; from_game(&p); x = p.x + mon.left; y = p.y + mon.top; }
    return SetCursorPos(x, y);
}
static BOOL WINAPI h_ClipCursor(const RECT *r)
{
    FIRST(2, "game ClipCursor %s", r ? "rect" : "off");
    if (r && active()) { RECT c; picture(&c); OffsetRect(&c, mon.left, mon.top); return ClipCursor(&c); }
    return ClipCursor(r);
}
static int WINAPI h_GetSystemMetrics(int i)
{
    if (active() && (i == SM_CXSCREEN || i == SM_CYSCREEN)) { FIRST(3, "game GetSystemMetrics %d", i); return i == SM_CXSCREEN ? mode_w : mode_h; }
    return GetSystemMetrics(i);
}
static BOOL WINAPI h_GetClientRect(HWND w, RECT *r)
{
    if (active() && w == game) { FIRST(4, "game GetClientRect"); SetRect(r, 0, 0, mode_w, mode_h); return TRUE; }
    return GetClientRect(w, r);
}
static BOOL WINAPI h_GetWindowRect(HWND w, RECT *r)
{
    if (active() && w == game) { FIRST(5, "game GetWindowRect"); SetRect(r, 0, 0, mode_w, mode_h); return TRUE; }
    return GetWindowRect(w, r);
}
static BOOL WINAPI h_ClientToScreen(HWND w, POINT *p)
{
    if (active() && w == game) { FIRST(6, "game ClientToScreen"); return TRUE; }   /* its window is its whole screen */
    return ClientToScreen(w, p);
}
static BOOL WINAPI h_ScreenToClient(HWND w, POINT *p)
{
    if (active() && w == game) { FIRST(7, "game ScreenToClient"); return TRUE; }
    return ScreenToClient(w, p);
}
static int WINAPI h_GetDeviceCaps(HDC dc, int i)
{
    if (active() && (i == HORZRES || i == VERTRES || i == DESKTOPHORZRES || i == DESKTOPVERTRES))
    {
        FIRST(3, "game GetDeviceCaps %d", i);
        return i == HORZRES || i == DESKTOPHORZRES ? mode_w : mode_h;
    }
    return GetDeviceCaps(dc, i);
}
static BOOL WINAPI h_GetMonitorInfoA(HMONITOR m, MONITORINFO *mi)
{
    BOOL ok = GetMonitorInfoA(m, mi);
    if (ok && active()) { FIRST(3, "game GetMonitorInfo"); SetRect(&mi->rcMonitor, 0, 0, mode_w, mode_h); mi->rcWork = mi->rcMonitor; }
    return ok;
}
static BOOL WINAPI h_EnumDisplaySettingsA(LPCSTR dev, DWORD n, DEVMODEA *dm)
{
    BOOL ok = EnumDisplaySettingsA(dev, n, dm);
    if (ok && active() && n == ENUM_CURRENT_SETTINGS)
    { FIRST(3, "game EnumDisplaySettings (current)"); dm->dmPelsWidth = mode_w; dm->dmPelsHeight = mode_h; dm->dmBitsPerPel = mode_bpp; }
    return ok;
}
/* Point the game's own imports of these at the functions above (our own calls still reach the real ones). */
static void patch_imports(void)
{
    static const struct { const char *name; void *fn; } hooks[] = {
        { "GetCursorPos", h_GetCursorPos }, { "SetCursorPos", h_SetCursorPos }, { "ClipCursor", h_ClipCursor },
        { "GetSystemMetrics", h_GetSystemMetrics }, { "GetClientRect", h_GetClientRect },
        { "GetWindowRect", h_GetWindowRect }, { "ClientToScreen", h_ClientToScreen }, { "ScreenToClient", h_ScreenToClient },
        { "GetMonitorInfoA", h_GetMonitorInfoA }, { "EnumDisplaySettingsA", h_EnumDisplaySettingsA },
        { "GetDeviceCaps", h_GetDeviceCaps } };
    enum { N = sizeof hooks / sizeof hooks[0] };
    HMODULE user = GetModuleHandleA("user32.dll"), gdi = GetModuleHandleA("gdi32.dll");
    void *real[N];
    for (int i = 0; i < N; i++)
        if (!(real[i] = (void *)GetProcAddress(user, hooks[i].name)) && gdi) real[i] = (void *)GetProcAddress(gdi, hooks[i].name);
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); d->Name; d++)
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++)
            for (int i = 0; i < N; i++)
                if (real[i] && *slot == real[i])
                {
                    DWORD old; VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
                    *slot = hooks[i].fn; VirtualProtect(slot, sizeof *slot, old, &old);
                    L("translating the game's %s", hooks[i].name);
                }
}
static void take_window(HWND w)
{
    if (w != game)
    {
        static int imports_done;
        game = w;
        HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof mi }; GetMonitorInfoA(m, &mi); mon = mi.rcMonitor;
        L("display layer: window %p fills the screen at %ld,%ld %ldx%ld", w, mon.left, mon.top,
          mon.right - mon.left, mon.bottom - mon.top);
        int known = 0;
        for (int i = 0; i < nsubclassed; i++) known |= subclassed[i].w == w;
        if (!known && nsubclassed < 4)
        {
            subclassed[nsubclassed].w = w;
            subclassed[nsubclassed++].proc = (WNDPROC)SetWindowLongA(w, GWL_WNDPROC, (LONG)game_wndproc);
        }
        if (!imports_done++) patch_imports();
    }
    if (mode_set) fill_screen();     /* otherwise at the game's first SetDisplayMode, so it never sees the real size */
}

/* ---- presenting: a thread with its own child window over the game's, showing the picture ---- */
static void mark_dirty(void) { InterlockedExchange(&dirty, 1); if (wake) SetEvent(wake); }
static LRESULT CALLBACK view_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(w, &ps); EndPaint(w, &ps); mark_dirty(); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(w, m, wp, lp);
}
/* The shadow copy as BGRA, ready to show. */
static void convert(BYTE *out, DWORD w, DWORD h, DWORD bpp)
{
    DWORD *o = (DWORD *)out;
    if (bpp == 8)
        for (DWORD i = 0; i < w * h; i++) o[i] = pal[shadow[i]];
    else if (bpp == 16)
        for (DWORD i = 0; i < w * h; i++)
        {
            WORD v = ((WORD *)shadow)[i]; DWORD r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            o[i] = 0xff000000 | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    else
        for (DWORD i = 0; i < w * h; i++) o[i] = ((DWORD *)shadow)[i] | 0xff000000;
}
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#define TEXW 4096
#define TEXH 2048
/* Sharp scaling: the picture is first enlarged by a whole number with hard pixel edges, then smoothed only over
   the last, smaller step to the screen, so pixels stay crisp squares without the uneven blockiness of plain
   pixel doubling at a ratio like 3.6. */
static void enlarge(DWORD *out, const DWORD *in, DWORD w, DWORD h, DWORD k)
{
    for (DWORD y = 0; y < h; y++)
    {
        DWORD *row = out + y * k * (w * k);
        for (DWORD x = 0; x < w; x++)
            for (DWORD i = 0; i < k; i++) row[x * k + i] = in[y * w + x];
        for (DWORD i = 1; i < k; i++) memcpy(row + i * w * k, row, w * k * 4);
    }
}
static DWORD WINAPI present(void *unused)
{
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC; wc.lpfnWndProc = view_proc; wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = GetStockObject(BLACK_BRUSH); wc.lpszClassName = "ddrawlog_view";
    RegisterClassA(&wc);
    RECT c; GetClientRect(game, &c);
    /* disabled, so the mouse goes through it to the game's window */
    HWND view = CreateWindowExA(WS_EX_NOPARENTNOTIFY, "ddrawlog_view", "", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                0, 0, c.right, c.bottom, game, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(view);
    HGLRC gl = NULL; GLuint tex = 0;
    if (!env_is("DDRAWLOG_GDI"))
    {
        PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
        int pf = ChoosePixelFormat(dc, &pfd);
        if (pf && SetPixelFormat(dc, pf, &pfd) && (gl = wglCreateContext(dc)) && wglMakeCurrent(dc, gl))
        {
            glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEXW, TEXH, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
            glEnable(GL_TEXTURE_2D);
            L("display layer: showing with OpenGL (%s)", (const char *)glGetString(GL_RENDERER));
        }
        else { if (gl) wglDeleteContext(gl); gl = NULL; }
    }
    if (!gl) L("display layer: showing with GDI");
    BYTE *rgba = malloc(1024 * 768 * 4), *big = gl ? malloc(TEXW * TEXH * 4) : NULL;
    DWORD shown_k = 0;
    DWORD last = 0, frames = 0, spent = 0;
    for (;;)
    {
        MsgWaitForMultipleObjects(1, &wake, FALSE, 100, QS_ALLINPUT);
        MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (!IsWindow(game)) break;
        GetClientRect(game, &c);
        RECT v; GetClientRect(view, &v);
        if (v.right != c.right || v.bottom != c.bottom) { MoveWindow(view, 0, 0, c.right, c.bottom, FALSE); mark_dirty(); }
        if (!dirty) continue;
        DWORD now = GetTickCount();
        if (now - last < 15) { Sleep(15 - (now - last)); now = GetTickCount(); }   /* at most ~60 a second */
        InterlockedExchange(&dirty, 0);
        DWORD w, h;
        EnterCriticalSection(&cs);
        w = prim_w; h = prim_h;
        if (shadow && w <= 1024 && h <= 768) convert(rgba, w, h, prim_bpp); else w = 0;
        LeaveCriticalSection(&cs);
        if (!w) continue;
        RECT r; picture_in(c.right, c.bottom, w, h, &r);
        if (gl)
        {
            glViewport(0, 0, c.right, c.bottom); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
            DWORD k = (r.right - r.left) / w;
            if (k < 1) k = 1;
            while (k > 1 && (w * k > TEXW || h * k > TEXH)) k--;
            if (k != shown_k) { L("display layer: %lux%lu enlarged %lu times, then smoothed to %ldx%ld", w, h, k, r.right - r.left, r.bottom - r.top); shown_k = k; }
            if (k > 1) enlarge((DWORD *)big, (DWORD *)rgba, w, h, k);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w * k, h * k, GL_BGRA_EXT, GL_UNSIGNED_BYTE, k > 1 ? big : rgba);
            glViewport(r.left, c.bottom - r.bottom, r.right - r.left, r.bottom - r.top);
            float u = (float)(w * k) / TEXW, t = (float)(h * k) / TEXH;
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(-1, 1); glTexCoord2f(u, 0); glVertex2f(1, 1);
            glTexCoord2f(u, t); glVertex2f(1, -1); glTexCoord2f(0, t); glVertex2f(-1, -1);
            glEnd();
            SwapBuffers(dc);
        }
        else
        {
            BITMAPINFO bi = {{ sizeof bi.bmiHeader, w, -(LONG)h, 1, 32, BI_RGB }};
            SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
            StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, w, h, rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
            HBRUSH black = GetStockObject(BLACK_BRUSH); RECT bar;
            SetRect(&bar, 0, 0, r.left, c.bottom); FillRect(dc, &bar, black);
            SetRect(&bar, r.right, 0, c.right, c.bottom); FillRect(dc, &bar, black);
            SetRect(&bar, r.left, 0, r.right, r.top); FillRect(dc, &bar, black);
            SetRect(&bar, r.left, r.bottom, r.right, c.bottom); FillRect(dc, &bar, black);
        }
        DWORD done = GetTickCount();
        spent += done - now; last = done;
        if (++frames % 600 == 0) { L("display layer: %lu frames, %lu ms each on average", frames, spent / 600); spent = 0; }
    }
    L("display layer: game window gone after %lu frames", frames);
    return 0;
}

/* ---- the stand-in screen: its pixels copied out after every change ---- */
typedef HRESULT (WINAPI *lock_fn)(void *, RECT *, BYTE *, DWORD, HANDLE);
typedef HRESULT (WINAPI *unlock_fn)(void *, void *);
static void copy_rect(void *s, const RECT *r)
{
    BYTE d[108]; memset(d, 0, sizeof d); *(DWORD *)d = 108;
    if (FAILED(((lock_fn)orig(s, 25))(s, NULL, d, 0x1 | 0x10, NULL))) return;   /* WAIT | READONLY */
    RECT full = { 0, 0, prim_w, prim_h }, c = full;
    if (r) IntersectRect(&c, r, &full);
    LONG pitch = *(LONG *)(d + 16); BYTE *bits = *(BYTE **)(d + 36); DWORD bb = prim_bpp / 8;
    EnterCriticalSection(&cs);
    if (shadow && bits && c.right > c.left)
        for (LONG y = c.top; y < c.bottom; y++)
            memcpy(shadow + (y * prim_w + c.left) * bb, bits + y * pitch + c.left * bb, (c.right - c.left) * bb);
    LeaveCriticalSection(&cs);
    ((unlock_fn)orig(s, 32))(s, NULL);
    mark_dirty();
}
static void sync_palette(void)
{
    void *p = NULL;
    if (!primary || prim_bpp != 8) return;
    if (FAILED(((HRESULT (WINAPI *)(void *, void **))orig(primary, 20))(primary, &p)) || !p) return;
    PALETTEENTRY e[256];
    if (SUCCEEDED(((HRESULT (WINAPI *)(void *, DWORD, DWORD, DWORD, PALETTEENTRY *))orig(p, 4))(p, 0, 0, 256, e)))
    {
        EnterCriticalSection(&cs);
        for (int i = 0; i < 256; i++) pal[i] = 0xff000000 | e[i].peRed << 16 | e[i].peGreen << 8 | e[i].peBlue;
        LeaveCriticalSection(&cs);
        mark_dirty();
    }
    ((ULONG (WINAPI *)(void *))(*(void ***)p)[2])(p);
}

static int fails[48];
#define FAIL(idx, name, hr) do { if ((hr) && fails[idx]++ < 40) L("surface %p %s -> %#lx", This, name, (unsigned long)(hr)); } while (0)

/* ---- surfaces ---- */
typedef HRESULT (WINAPI *f1)(void *);
static RECT lock_rect; static int lock_whole;
static ULONG WINAPI s_Release(void *This)
{
    ULONG n = ((ULONG (WINAPI *)(void *))orig(This, 2))(This);
    if (!n && This == primary) { EnterCriticalSection(&cs); primary = NULL; LeaveCriticalSection(&cs); L("display layer: screen %p released", This); }
    return n;
}
static HRESULT WINAPI s_Restore(void *This) { HRESULT hr = ((f1)orig(This, 27))(This); L("surface %p Restore -> %#lx", This, (unsigned long)hr); return hr; }
static HRESULT WINAPI s_IsLost(void *This) { HRESULT hr = ((f1)orig(This, 24))(This); FAIL(24, "IsLost", hr); return hr; }
static HRESULT WINAPI s_Lock(void *This, RECT *r, BYTE *d, DWORD f, HANDLE e)
{
    HRESULT hr = ((lock_fn)orig(This, 25))(This, r, d, f, e); FAIL(25, "Lock", hr);
    if (SUCCEEDED(hr) && This == primary) { lock_whole = !r; if (r) lock_rect = *r; }
    return hr;
}
static HRESULT WINAPI s_Unlock(void *This, void *p)
{
    HRESULT hr = ((unlock_fn)orig(This, 32))(This, p); FAIL(32, "Unlock", hr);
    if (SUCCEEDED(hr) && This == primary) copy_rect(This, lock_whole ? NULL : &lock_rect);
    return hr;
}
static HRESULT WINAPI s_ReleaseDC(void *This, HDC dc)
{
    HRESULT hr = ((HRESULT (WINAPI *)(void *, HDC))orig(This, 26))(This, dc); FAIL(26, "ReleaseDC", hr);
    if (SUCCEEDED(hr) && This == primary) copy_rect(This, NULL);
    return hr;
}
static HRESULT WINAPI s_Flip(void *This, void *o, DWORD f)
{ HRESULT hr = ((HRESULT (WINAPI *)(void *, void *, DWORD))orig(This, 11))(This, o, f); FAIL(11, "Flip", hr); return hr; }
static HRESULT WINAPI s_Blt(void *This, RECT *r, void *s, RECT *sr, DWORD f, void *fx)
{
    RECT mapped;
    if (This == primary && r && (r->right > (LONG)prim_w || r->bottom > (LONG)prim_h))
    {   /* aimed at the real screen: the same place on the picture */
        int sw = mon.right - mon.left, sh = mon.bottom - mon.top;
        if (fails[40]++ < 3) L("  Blt to the screen at %ld,%ld-%ld,%ld (real screen size): put on the picture", r->left, r->top, r->right, r->bottom);
        SetRect(&mapped, MulDiv(r->left, prim_w, sw), MulDiv(r->top, prim_h, sh), MulDiv(r->right, prim_w, sw), MulDiv(r->bottom, prim_h, sh));
        r = &mapped;
    }
    HRESULT hr = ((HRESULT (WINAPI *)(void *, RECT *, void *, RECT *, DWORD, void *))orig(This, 5))(This, r, s, sr, f, fx); FAIL(5, "Blt", hr);
    if (FAILED(hr) && fails[5] <= 40)
        L("  Blt to %p%s flags=%#lx dest %ld,%ld-%ld,%ld from %p %ld,%ld-%ld,%ld", This, This == primary ? " (screen)" : "", f,
          r ? r->left : -1, r ? r->top : -1, r ? r->right : -1, r ? r->bottom : -1, s,
          sr ? sr->left : -1, sr ? sr->top : -1, sr ? sr->right : -1, sr ? sr->bottom : -1);
    if (SUCCEEDED(hr) && This == primary) copy_rect(This, r);
    return hr;
}
static HRESULT WINAPI s_BltFast(void *This, DWORD x, DWORD y, void *s, RECT *sr, DWORD f)
{
    HRESULT hr = ((HRESULT (WINAPI *)(void *, DWORD, DWORD, void *, RECT *, DWORD))orig(This, 7))(This, x, y, s, sr, f); FAIL(7, "BltFast", hr);
    if (SUCCEEDED(hr) && This == primary)
    {
        RECT r = { x, y, x + (sr ? sr->right - sr->left : 0), y + (sr ? sr->bottom - sr->top : 0) };
        copy_rect(This, sr ? &r : NULL);
    }
    return hr;
}
static HRESULT WINAPI s_SetPalette(void *This, void *p)
{
    HRESULT hr = ((HRESULT (WINAPI *)(void *, void *))orig(This, 31))(This, p); FAIL(31, "SetPalette", hr);
    if (SUCCEEDED(hr) && This == primary) sync_palette();
    return hr;
}
static void hook_surface(void *s)
{
    table *t = add_table(s, 100); if (!t) return;
    L("hooking surface table %p", t->vt);
    patch(t, 5, s_Blt); patch(t, 7, s_BltFast); patch(t, 11, s_Flip); patch(t, 24, s_IsLost);
    patch(t, 25, s_Lock); patch(t, 27, s_Restore); patch(t, 31, s_SetPalette);
    if (scaling > 0) { patch(t, 2, s_Release); patch(t, 26, s_ReleaseDC); patch(t, 32, s_Unlock); }
}

/* ---- palettes: a change to the screen's palette is shown at once ---- */
static HRESULT WINAPI p_SetEntries(void *This, DWORD f, DWORD start, DWORD n, PALETTEENTRY *e)
{
    HRESULT hr = ((HRESULT (WINAPI *)(void *, DWORD, DWORD, DWORD, PALETTEENTRY *))orig(This, 6))(This, f, start, n, e);
    if (SUCCEEDED(hr)) sync_palette();
    return hr;
}
static HRESULT WINAPI d_CreatePalette(void *This, DWORD f, PALETTEENTRY *e, void **p, IUnknown *o)
{
    HRESULT hr = ((HRESULT (WINAPI *)(void *, DWORD, PALETTEENTRY *, void **, IUnknown *))orig(This, 5))(This, f, e, p, o);
    if (SUCCEEDED(hr) && *p) { table *t = add_table(*p, 200); if (t) patch(t, 6, p_SetEntries); }
    return hr;
}

/* ---- DirectDraw objects ---- */
static void hook_ddraw(void *dd, int version);
static const GUID IID_DD2 = {0xB3A6F3E0,0x2B43,0x11CF,{0xA2,0xDE,0x00,0xAA,0x00,0xB9,0x33,0x56}};
static const GUID IID_DD4 = {0x9c59509a,0x39bd,0x11d1,{0x8c,0x4a,0x00,0xc0,0x4f,0xd9,0x30,0xc5}};
static const GUID IID_DD7 = {0x15e65ec0,0x3b9c,0x11d2,{0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b}};
static HRESULT WINAPI d_QI(void *This, REFIID iid, void **out)
{
    HRESULT hr = ((HRESULT (WINAPI *)(void *, REFIID, void **))orig(This, 0))(This, iid, out);
    L("ddraw %p QueryInterface {%08lx-...} -> %#lx", This, iid->Data1, (unsigned long)hr);
    if (SUCCEEDED(hr) && *out)
    {
        if (IsEqualGUID(iid, &IID_DD2)) hook_ddraw(*out, 2);
        else if (IsEqualGUID(iid, &IID_DD4)) hook_ddraw(*out, 4);
        else if (IsEqualGUID(iid, &IID_DD7)) hook_ddraw(*out, 7);
    }
    return hr;
}
static HRESULT WINAPI d_CreateSurface(void *This, BYTE *d, void **s, IUnknown *o)
{
    char ds[160]; desc_str(d, ds);
    HRESULT (WINAPI *create)(void *, BYTE *, void **, IUnknown *) = orig(This, 6);
    /* The game's screen (a primary surface): an ordinary picture in memory, of the mode the game asked for. */
    if (scaling > 0 && game && kind_of(This) == 1 && d && (*(DWORD *)(d + 104) & 0x200))
    {
        BYTE nd[108]; memset(nd, 0, sizeof nd);
        *(DWORD *)nd = 108; *(DWORD *)(nd + 4) = 0x1 | 0x2 | 0x4 | 0x1000;      /* CAPS|HEIGHT|WIDTH|PIXELFORMAT */
        *(DWORD *)(nd + 8) = mode_h; *(DWORD *)(nd + 12) = mode_w;
        fill_format(nd, mode_bpp);
        *(DWORD *)(nd + 104) = 0x40 | 0x800;                                   /* OFFSCREENPLAIN|SYSTEMMEMORY */
        HRESULT hr = create(This, nd, s, o);
        L("ddraw v1 %p CreateSurface %s -> stand-in %lux%lux%lu -> %#lx surface %p", This, ds, mode_w, mode_h, mode_bpp,
          (unsigned long)hr, SUCCEEDED(hr) ? *s : NULL);
        if (FAILED(hr) || !*s) return hr;
        hook_surface(*s);
        EnterCriticalSection(&cs);
        primary = *s; prim_w = mode_w; prim_h = mode_h; prim_bpp = mode_bpp;
        free(shadow); shadow = calloc(prim_w * prim_h, prim_bpp / 8);
        LeaveCriticalSection(&cs);
        if (!presenter_thread) presenter_thread = CreateThread(NULL, 0, present, NULL, 0, NULL);
        mark_dirty();
        return hr;
    }
    /* A picture without its own pixel format takes the display's. The real display was never changed, so give it
       the format of the game's mode, as it would have had on a display that switched. */
    BYTE nd[108];
    if (scaling > 0 && mode_set && d && *(DWORD *)d == 108 && !(*(DWORD *)(d + 4) & 0x1000) && !(*(DWORD *)(d + 104) & 0x200))
    {
        memcpy(nd, d, 108); *(DWORD *)(nd + 4) |= 0x1000; fill_format(nd, mode_bpp); d = nd;
    }
    HRESULT hr = create(This, d, s, o);
    L("ddraw v%d %p CreateSurface %s%s -> %#lx surface %p", kind_of(This), This, ds, d == nd ? " (in the game's format)" : "",
      (unsigned long)hr, SUCCEEDED(hr) ? *s : NULL);
    if (SUCCEEDED(hr) && *s) hook_surface(*s);
    return hr;
}
static HRESULT WINAPI d_RestoreDisplayMode(void *This)
{
    if (scaling > 0) { L("ddraw %p RestoreDisplayMode (display never changed)", This); return 0; }
    HRESULT hr = ((f1)orig(This, 19))(This); L("ddraw %p RestoreDisplayMode -> %#lx", This, (unsigned long)hr); return hr;
}
static HRESULT WINAPI d_SetCooperativeLevel(void *This, HWND w, DWORD f)
{
    HRESULT (WINAPI *set)(void *, HWND, DWORD) = orig(This, 20);
    if (scaling > 0 && w && (f & 0x10))                     /* EXCLUSIVE: full screen without taking the display */
    {
        HRESULT hr = set(This, w, 0x8);                     /* NORMAL */
        L("ddraw %p SetCooperativeLevel hwnd=%p flags=%#lx -> as normal -> %#lx", This, w, f, (unsigned long)hr);
        if (SUCCEEDED(hr)) take_window(w);
        return hr;
    }
    HRESULT hr = set(This, w, f);
    L("ddraw %p SetCooperativeLevel hwnd=%p flags=%#lx -> %#lx", This, w, f, (unsigned long)hr); return hr;
}
static void set_mode(DWORD w, DWORD h, DWORD bpp)
{
    mode_w = w; mode_h = h; mode_bpp = bpp; mode_set = 1;
    if (game) fill_screen();
    mark_dirty();
}
/* Some display sizes exist on a Mac display but Wine won't set them (Railroad Tycoon II's campaign intro asks for
   640x480 at 8 bits; the Studio Display refuses it with DDERR_UNSUPPORTED). When a size is refused, ask again at the
   size that last worked, in the colour depth the game asked for, so it still gets the palette and depth it expects;
   the picture then sits in the top-left of the larger screen. DDRAWLOG_NOFALLBACK=1 turns this off. */
static DWORD last_w, last_h;
static HRESULT WINAPI d_SetDisplayMode1(void *This, DWORD w, DWORD h, DWORD bpp)
{
    if (scaling > 0 && (bpp == 8 || bpp == 16 || bpp == 32))
    {
        set_mode(w, h, bpp); L("ddraw v1 %p SetDisplayMode %lux%lux%lu (shown scaled, display unchanged)", This, w, h, bpp);
        return 0;
    }
    HRESULT (WINAPI *set)(void *, DWORD, DWORD, DWORD) = orig(This, 21);
    HRESULT hr = set(This, w, h, bpp);
    L("ddraw v1 %p SetDisplayMode %lux%lux%lu -> %#lx", This, w, h, bpp, (unsigned long)hr);
    if (hr == (HRESULT)0x80004001 && last_w && (w != last_w || h != last_h) && !env_is("DDRAWLOG_NOFALLBACK"))
    {
        hr = set(This, last_w, last_h, bpp);
        L("ddraw v1 %p   refused; asked for %lux%lux%lu instead -> %#lx", This, last_w, last_h, bpp, (unsigned long)hr);
        if (SUCCEEDED(hr)) return hr;
    }
    if (SUCCEEDED(hr)) { last_w = w; last_h = h; }
    return hr;
}
static HRESULT WINAPI d_SetDisplayMode2(void *This, DWORD w, DWORD h, DWORD bpp, DWORD rr, DWORD f)
{
    HRESULT (WINAPI *set)(void *, DWORD, DWORD, DWORD, DWORD, DWORD) = orig(This, 21);
    HRESULT hr = set(This, w, h, bpp, rr, f);
    L("ddraw v%d %p SetDisplayMode %lux%lux%lu @%luHz flags=%#lx -> %#lx", kind_of(This), This, w, h, bpp, rr, f, (unsigned long)hr);
    if (hr == (HRESULT)0x80004001 && last_w && (w != last_w || h != last_h) && !env_is("DDRAWLOG_NOFALLBACK"))
    {
        hr = set(This, last_w, last_h, bpp, 0, f);
        L("ddraw v%d %p   refused; asked for %lux%lux%lu instead -> %#lx", kind_of(This), This, last_w, last_h, bpp, (unsigned long)hr);
        if (SUCCEEDED(hr)) return hr;
    }
    if (SUCCEEDED(hr)) { last_w = w; last_h = h; }
    return hr;
}
static HRESULT WINAPI d_GetDisplayMode(void *This, BYTE *d)
{
    if (scaling > 0 && mode_set && d)
    {
        fill_mode(d, mode_w, mode_h, mode_bpp);
        L("ddraw %p GetDisplayMode -> %lux%lux%lu (the game's mode)", This, mode_w, mode_h, mode_bpp); return 0;
    }
    HRESULT hr = ((HRESULT (WINAPI *)(void *, BYTE *))orig(This, 12))(This, d);
    char ds[160]; desc_str(d, ds); L("ddraw %p GetDisplayMode %s -> %#lx", This, ds, (unsigned long)hr); return hr;
}
/* EnumDisplayModes: the game asks for every mode the display supports and then uses only a few. Wine builds that
   list from macOS, and a MacBook screen reports over 1,600 modes; Wine's per-mode work makes the list take minutes
   (the game sits at full CPU with sound and no picture). Answer instead with the classic modes such games use. With
   the display layer every one of them is offered (the display never changes); without it, only those the display
   accepts (a quick CDS_TEST each). DDRAWLOG_ENUMALL=1 passes the call through. */
typedef HRESULT (WINAPI *enum_cb)(BYTE *desc, void *ctx);
static int display_accepts(DWORD w, DWORD h)
{
    DEVMODEA dm = {0}; dm.dmSize = sizeof dm;
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    return ChangeDisplaySettingsExA(NULL, &dm, NULL, CDS_TEST, NULL) == DISP_CHANGE_SUCCESSFUL;
}
static HRESULT WINAPI d_EnumDisplayModes(void *This, DWORD flags, BYTE *filter, void *ctx, enum_cb cb)
{
    if (env_is("DDRAWLOG_ENUMALL") || !cb)
    {
        HRESULT hr = ((HRESULT (WINAPI *)(void *, DWORD, BYTE *, void *, enum_cb))orig(This, 8))(This, flags, filter, ctx, cb);
        L("ddraw %p EnumDisplayModes (passed through) -> %#lx", This, (unsigned long)hr); return hr;
    }
    static const DWORD sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}}, depths[] = {8, 16, 32};
    BYTE d[108]; int n = 0;
    for (int s = 0; s < 3; s++)
    {
        DWORD w = sizes[s][0], h = sizes[s][1];
        if (filter && (*(DWORD *)(filter + 4) & 0x4) && *(DWORD *)(filter + 12) != w) continue;
        if (filter && (*(DWORD *)(filter + 4) & 0x2) && *(DWORD *)(filter + 8) != h) continue;
        int ok = scaling > 0 || display_accepts(w, h);
        L("ddraw %p EnumDisplayModes: %lux%lu %s", This, w, h, ok ? "offered" : "not available on this display");
        if (!ok) continue;
        for (int b = 0; b < 3; b++)
        {
            if (filter && (*(DWORD *)(filter + 4) & 0x1000) && *(DWORD *)(filter + 84) != depths[b]) continue;
            fill_mode(d, w, h, depths[b]); n++;
            if (cb(d, ctx) == 0) { L("ddraw %p EnumDisplayModes: stopped by the game after %d modes", This, n); return 0; }
        }
    }
    L("ddraw %p EnumDisplayModes: offered %d modes", This, n);
    return 0;
}

static void hook_ddraw(void *dd, int version)
{
    table *t = add_table(dd, version); if (!t) return;
    if (version <= 2) patch(t, 8, d_EnumDisplayModes);   /* v4/v7 use the larger DDSURFACEDESC2 */
    L("hooking ddraw v%d table %p", version, t->vt);
    patch(t, 0, d_QI); patch(t, 6, d_CreateSurface); patch(t, 12, d_GetDisplayMode); patch(t, 19, d_RestoreDisplayMode);
    patch(t, 20, d_SetCooperativeLevel); patch(t, 21, version == 1 ? (void *)d_SetDisplayMode1 : (void *)d_SetDisplayMode2);
    if (version == 1 && scaling > 0) patch(t, 5, d_CreatePalette);
}

/* ---- the export ---- */
HRESULT WINAPI DirectDrawCreate(GUID *g, void **dd, IUnknown *o)
{
    if (!lg)
    {
        char p[MAX_PATH]; HMODULE me;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)DirectDrawCreate, &me);
        GetModuleFileNameA(me, p, MAX_PATH); strcpy(strrchr(p, '\\') + 1, "ddrawlog.txt");
        lg = fopen(p, "a");
    }
    if (scaling < 0)
    {
        scaling = !env_is("DDRAWLOG_NOSCALE");
        InitializeCriticalSection(&cs); wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        L("ddrawlog v1.2: display layer %s", scaling ? "on" : "off (DDRAWLOG_NOSCALE=1)");
    }
    char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH); strcat(sys, "\\ddraw.dll");
    HMODULE real = LoadLibraryA(sys);
    L("DirectDrawCreate: real ddraw %s -> %p", sys, real);
    HRESULT (WINAPI *create)(GUID *, void **, IUnknown *) = (void *)GetProcAddress(real, "DirectDrawCreate");
    if (!create) return E_FAIL;
    HRESULT hr = create(g, dd, o);
    L("DirectDrawCreate -> %#lx object %p", (unsigned long)hr, SUCCEEDED(hr) ? *dd : NULL);
    if (SUCCEEDED(hr) && *dd) hook_ddraw(*dd, 1);
    return hr;
}
