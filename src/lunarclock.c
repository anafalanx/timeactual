/* lunarclock.c -- Direct2D analog-clock widget for the Tk shell.
 *
 * The rest of Lunar deliberately remains Tcl/Tk: menus, settings, tray, and
 * window lifetime all belong to Tk.  This widget owns only its child HWND's
 * pixels, so Direct2D never races Tk's canvas paint machinery.
 */
#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d2d1.h>
#include <tcl.h>
#include <tk.h>
#include <tkPlatDecls.h>

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef enum {
    CLOCK_TRUST_INOP,
    CLOCK_TRUST_OK,
    CLOCK_TRUST_HOLDOVER,
    CLOCK_TRUST_REACQUIRING
} ClockTrust;

typedef struct ClockWidget {
    Tk_Window tkwin;
    Tcl_Interp *interp;
    Tcl_Command command;
    int width, height;
    int hour, minute, second, millisecond;
    int hasTime, synced;
    int boundMs;   /* honest worst-case error bound; the second hand's width */
    int stopped;   /* bound exceeded the user's ceiling: withdraw the seconds claim */
    unsigned short armedMask;
    ClockTrust trust;
    int64_t baseQpc;            /* QPC stamp of the last `show` feed */
    Tcl_TimerToken sweepTimer;  /* ~30 fps sweep re-arm, NULL when idle */
    int redrawPending;
    ID2D1HwndRenderTarget *target;
    ID2D1SolidColorBrush *brush;
    int targetWidth, targetHeight;
    ID2D1PathGeometry *hourHand;
    ID2D1PathGeometry *minuteHand;
    float handSize;
} ClockWidget;

static ID2D1Factory *g_d2d;

/* --- Sweep animation -------------------------------------------------------
 * The Tcl side feeds authoritative disciplined time at 5 Hz; between feeds
 * the widget extrapolates the shown milliseconds from raw QPC so the second
 * hand SWEEPS instead of stepping. The extrapolation is display-only and
 * hard-capped: if the feed stalls, the hand pauses (an honest signal) rather
 * than free-running local time. The timer runs only while a dial is shown. */
#define SWEEP_FRAME_MS        33   /* ~30 fps: tip moves < 1 px per frame */
#define SWEEP_MAX_EXTRAP_MS  400   /* two feed periods, then hold */

static int64_t qpc_freq(void) {
    static int64_t f = 0;
    if (!f) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        f = li.QuadPart ? li.QuadPart : 1;
    }
    return f;
}

static int64_t qpc_now(void) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

#ifndef PI
#define PI 3.14159265358979323846f
#endif
#define DEG2RAD (PI / 180.0f)

static D2D1_COLOR_F rgb(int r, int g, int b) {
    D2D1_COLOR_F c = { r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
    return c;
}

static D2D1_POINT_2F pt(float x, float y) {
    D2D1_POINT_2F p = { x, y };
    return p;
}

static D2D1_ELLIPSE ellipse(float cx, float cy, float r) {
    D2D1_ELLIPSE e = { { cx, cy }, r, r };
    return e;
}

static D2D1_POINT_2F polar(float cx, float cy, float radius, float degrees) {
    float a = (degrees - 90.0f) * DEG2RAD;
    return pt(cx + radius * cosf(a), cy + radius * sinf(a));
}

static void set_brush(ClockWidget *clock, D2D1_COLOR_F color) {
    ID2D1SolidColorBrush_SetColor(clock->brush, &color);
}

static HRESULT ensure_factories(void) {
    HRESULT hr;
    if (!g_d2d) {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               &IID_ID2D1Factory, NULL, (void **)&g_d2d);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

static void release_hand_cache(ClockWidget *clock) {
    if (clock->hourHand) {
        ID2D1PathGeometry_Release(clock->hourHand);
        clock->hourHand = NULL;
    }
    if (clock->minuteHand) {
        ID2D1PathGeometry_Release(clock->minuteHand);
        clock->minuteHand = NULL;
    }
    clock->handSize = 0.0f;
}

static void discard_target(ClockWidget *clock) {
    release_hand_cache(clock);
    if (clock->brush) {
        ID2D1SolidColorBrush_Release(clock->brush);
        clock->brush = NULL;
    }
    if (clock->target) {
        ID2D1HwndRenderTarget_Release(clock->target);
        clock->target = NULL;
    }
    clock->targetWidth = 0;
    clock->targetHeight = 0;
}

static ID2D1PathGeometry *build_hand(float length, float baseWidth,
                                     float tipWidth) {
    ID2D1PathGeometry *geometry = NULL;
    ID2D1GeometrySink *sink = NULL;
    float back = baseWidth;
    if (FAILED(ID2D1Factory_CreatePathGeometry(g_d2d, &geometry))) return NULL;
    if (FAILED(ID2D1PathGeometry_Open(geometry, &sink))) {
        ID2D1PathGeometry_Release(geometry);
        return NULL;
    }
    D2D1_POINT_2F first = pt(+baseWidth * 0.5f, +back);
    D2D1_POINT_2F rest[3] = {
        { +tipWidth * 0.5f, -length },
        { -tipWidth * 0.5f, -length },
        { -baseWidth * 0.5f, +back }
    };
    ID2D1GeometrySink_BeginFigure(sink, first, D2D1_FIGURE_BEGIN_FILLED);
    ID2D1GeometrySink_AddLines(sink, rest, 3);
    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
    if (FAILED(ID2D1GeometrySink_Close(sink))) {
        ID2D1GeometrySink_Release(sink);
        ID2D1PathGeometry_Release(geometry);
        return NULL;
    }
    ID2D1GeometrySink_Release(sink);
    return geometry;
}

static void ensure_hand_cache(ClockWidget *clock, float size) {
    if (clock->handSize == size && clock->hourHand && clock->minuteHand) return;
    release_hand_cache(clock);
    clock->hourHand = build_hand(0.28f * size, 0.035f * size, 0.020f * size);
    clock->minuteHand = build_hand(0.40f * size, 0.022f * size, 0.012f * size);
    clock->handSize = size;
}

static void draw_hand(ClockWidget *clock, ID2D1PathGeometry *geometry,
                      float cx, float cy, float degrees, D2D1_COLOR_F color) {
    if (!geometry) return;
    float radians = degrees * DEG2RAD;
    float cosine = cosf(radians), sine = sinf(radians);
    D2D1_MATRIX_3X2_F transform = { 0 };
    transform._11 = cosine;  transform._12 = sine;
    transform._21 = -sine;   transform._22 = cosine;
    transform._31 = cx;      transform._32 = cy;
    ID2D1RenderTarget_SetTransform((ID2D1RenderTarget *)clock->target, &transform);
    set_brush(clock, color);
    ID2D1RenderTarget_FillGeometry((ID2D1RenderTarget *)clock->target,
                                   (ID2D1Geometry *)geometry,
                                   (ID2D1Brush *)clock->brush, NULL);
    D2D1_MATRIX_3X2_F identity = { 0 };
    identity._11 = 1.0f;
    identity._22 = 1.0f;
    ID2D1RenderTarget_SetTransform((ID2D1RenderTarget *)clock->target, &identity);
}

/* The second hand IS the uncertainty sector: one solid red shape, drawn as
 * wide as the error bound.  ±boundMs maps onto the seconds dial as a
 * half-angle of boundMs/1000 × 6°, centered on the best-estimate second,
 * and the needle drawn later along that center gives the shape a floor
 * width, so a tight bound reads as an ordinary thin second hand and a loose
 * one as the same hand grown wide.  The sector goes UNDER the ticks and the
 * hour/minute hands (they stay legible by order, not by transparency); the
 * needle goes over them like any second hand.  At ±30 s the sector covers
 * the whole dial; draw a disc rather than a degenerate arc.
 * The display has exactly two states -- time shown (this dial, uncertainty
 * carried entirely by the hand's width) or no time -- so the hand is always
 * the signature red; there is no third look. */
static void draw_uncertainty(ClockWidget *clock, float cx, float cy,
                             float size, float seconds) {
    if (clock->boundMs <= 0) return;
    float half = (float)clock->boundMs / 1000.0f * 6.0f;
    D2D1_COLOR_F tint = rgb(15, 71, 175);   /* the needle's own colour */
    float radius = size * 0.44f;   /* same reach as the second hand's tip */
    ID2D1RenderTarget *target = (ID2D1RenderTarget *)clock->target;

    if (half >= 180.0f) {
        set_brush(clock, tint);
        D2D1_ELLIPSE disc = ellipse(cx, cy, radius);
        ID2D1RenderTarget_FillEllipse(target, &disc, (ID2D1Brush *)clock->brush);
        return;
    }

    float center = seconds * 6.0f;
    D2D1_POINT_2F from = polar(cx, cy, radius, center - half);
    D2D1_POINT_2F to   = polar(cx, cy, radius, center + half);
    ID2D1PathGeometry *sector = NULL;
    ID2D1GeometrySink *sink = NULL;
    if (FAILED(ID2D1Factory_CreatePathGeometry(g_d2d, &sector))) return;
    if (FAILED(ID2D1PathGeometry_Open(sector, &sink))) {
        ID2D1PathGeometry_Release(sector);
        return;
    }
    ID2D1GeometrySink_BeginFigure(sink, pt(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
    ID2D1GeometrySink_AddLine(sink, from);
    {
        D2D1_ARC_SEGMENT arc = {
            to, { radius, radius }, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
            (half * 2.0f > 180.0f) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL
        };
        ID2D1GeometrySink_AddArc(sink, &arc);
    }
    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(ID2D1GeometrySink_Close(sink))) {
        set_brush(clock, tint);
        ID2D1RenderTarget_FillGeometry(target, (ID2D1Geometry *)sector,
                                       (ID2D1Brush *)clock->brush, NULL);
    }
    ID2D1GeometrySink_Release(sink);
    ID2D1PathGeometry_Release(sector);
}

static void draw_dial(ClockWidget *clock, float cx, float cy, float size,
                      int live) {
    ID2D1RenderTarget *target = (ID2D1RenderTarget *)clock->target;
    D2D1_COLOR_F ink = rgb(0, 0, 0);
    D2D1_COLOR_F soft = rgb(110, 110, 110);
    D2D1_COLOR_F ring = rgb(212, 212, 212);
    D2D1_COLOR_F face = rgb(255, 255, 255);
    /* Match the original Direct2D face: an inset dial, full-length ticks,
     * and a double marker at 12 o'clock. */
    float radius = size * 0.46f;

    /* Sweep: extrapolate the shown time by the QPC elapsed since the last
     * authoritative feed, hard-capped so a stalled feed pauses the hand.
     * Total-seconds math lets the carry ripple into minutes and hours. */
    int64_t extraMs = 0;
    if (clock->baseQpc) {
        extraMs = (qpc_now() - clock->baseQpc) * 1000 / qpc_freq();
        if (extraMs < 0) extraMs = 0;
        if (extraMs > SWEEP_MAX_EXTRAP_MS) extraMs = SWEEP_MAX_EXTRAP_MS;
    }
    double totalSec = (double)(clock->hour % 12) * 3600.0
                    + (double)clock->minute * 60.0
                    + (double)clock->second
                    + ((double)clock->millisecond + (double)extraMs) / 1000.0;
    float seconds = (float)fmod(totalSec, 60.0);
    float minutes = (float)(fmod(totalSec, 3600.0) / 60.0);
    float hours   = (float)(totalSec / 3600.0);

    set_brush(clock, ring);
    D2D1_ELLIPSE outer = ellipse(cx, cy, radius + 1.5f);
    ID2D1RenderTarget_FillEllipse(target, &outer, (ID2D1Brush *)clock->brush);
    set_brush(clock, face);
    D2D1_ELLIPSE inner = ellipse(cx, cy, radius);
    ID2D1RenderTarget_FillEllipse(target, &inner, (ID2D1Brush *)clock->brush);

    /* The uncertainty fan sits under the ticks and hands. */
    if (live) draw_uncertainty(clock, cx, cy, size, seconds);

    set_brush(clock, soft);
    for (int mark = 0; mark < 60; ++mark) {
        if (mark % 5 == 0) continue;
        D2D1_POINT_2F from = polar(cx, cy, radius - size * 0.020f, mark * 6.0f);
        D2D1_POINT_2F to = polar(cx, cy, radius, mark * 6.0f);
        ID2D1RenderTarget_DrawLine(target, from, to, (ID2D1Brush *)clock->brush,
                                   size * 0.0080f, NULL);
    }
    for (int mark = 1; mark < 12; ++mark) {
        /* Five-minute markers double as the persistent chime controls. */
        D2D1_COLOR_F marker = (clock->armedMask & (1u << mark))
                            ? rgb(15, 71, 175) : ink;
        set_brush(clock, marker);
        D2D1_POINT_2F from = polar(cx, cy, radius - size * 0.050f, mark * 30.0f);
        D2D1_POINT_2F to = polar(cx, cy, radius, mark * 30.0f);
        ID2D1RenderTarget_DrawLine(target, from, to, (ID2D1Brush *)clock->brush,
                                   size * 0.0140f, NULL);
    }

    {
        D2D1_COLOR_F marker = (clock->armedMask & 1u)
                            ? rgb(15, 71, 175) : ink;
        float offset = size * 0.020f;
        D2D1_POINT_2F from = polar(cx, cy, radius - size * 0.050f, 0.0f);
        D2D1_POINT_2F to = polar(cx, cy, radius, 0.0f);
        set_brush(clock, marker);
        ID2D1RenderTarget_DrawLine(target, pt(from.x - offset, from.y),
                                   pt(to.x - offset, to.y), (ID2D1Brush *)clock->brush,
                                   size * 0.0140f, NULL);
        ID2D1RenderTarget_DrawLine(target, pt(from.x + offset, from.y),
                                   pt(to.x + offset, to.y), (ID2D1Brush *)clock->brush,
                                   size * 0.0140f, NULL);
    }

    /* No time to show: the dial stays bare -- ring, ticks, and markers
     * only. No hands, no fan, no hub, and never a word on the face. */
    if (!live) return;

    ensure_hand_cache(clock, size);
    draw_hand(clock, clock->hourHand, cx, cy, hours * 30.0f, ink);
    draw_hand(clock, clock->minuteHand, cx, cy, minutes * 6.0f, ink);

    /* The needle: the best-estimate centerline of the same red sector, and
     * the floor width of the second hand when the bound is tight. */
    D2D1_COLOR_F accent = rgb(15, 71, 175);
    D2D1_POINT_2F tip = polar(cx, cy, size * 0.44f, seconds * 6.0f);
    D2D1_POINT_2F tail = polar(cx, cy, -size * 0.08f, seconds * 6.0f);
    set_brush(clock, accent);
    ID2D1RenderTarget_DrawLine(target, tail, tip, (ID2D1Brush *)clock->brush,
                               size * 0.0045f, NULL);
    D2D1_ELLIPSE pivot = ellipse(cx, cy, size * 0.014f);
    ID2D1RenderTarget_FillEllipse(target, &pivot, (ID2D1Brush *)clock->brush);
}

static HRESULT create_target(ClockWidget *clock, HWND hwnd, int width, int height) {
    HRESULT hr = ensure_factories();
    if (FAILED(hr)) return hr;
    D2D1_RENDER_TARGET_PROPERTIES properties = {
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        { DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED },
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT
    };
    D2D1_SIZE_U size = { (UINT32)width, (UINT32)height };
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties = {
        hwnd, size, D2D1_PRESENT_OPTIONS_NONE
    };
    hr = ID2D1Factory_CreateHwndRenderTarget(g_d2d, &properties,
                                              &hwndProperties, &clock->target);
    if (FAILED(hr)) return hr;
    D2D1_COLOR_F initial = rgb(0, 0, 0);
    hr = ID2D1HwndRenderTarget_CreateSolidColorBrush(clock->target, &initial,
                                                      NULL, &clock->brush);
    if (FAILED(hr)) discard_target(clock);
    if (SUCCEEDED(hr)) {
        clock->targetWidth = width;
        clock->targetHeight = height;
    }
    return hr;
}

static void paint_fallback(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    RECT rect;
    GetClientRect(hwnd, &rect);
    HBRUSH background = CreateSolidBrush(RGB(242, 242, 242));
    FillRect(dc, &rect, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 50, 47));
    DrawTextW(dc, L"INOP", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ReleaseDC(hwnd, dc);
}

static void sweep_manage(ClockWidget *clock);

static void clock_redraw(void *clientData) {
    ClockWidget *clock = clientData;
    clock->redrawPending = 0;
    sweep_manage(clock);
    if (!clock->tkwin || !Tk_IsMapped(clock->tkwin)) return;
    Tk_MakeWindowExist(clock->tkwin);
    HWND hwnd = Tk_GetHWND(Tk_WindowId(clock->tkwin));
    if (!IsWindow(hwnd)) return;

    RECT client;
    GetClientRect(hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width < 1 || height < 1) return;
    if (clock->target && (clock->targetWidth != width || clock->targetHeight != height)) {
        /* A Tk resize can move and resize this child HWND several times in
         * one top-level gesture. Recreating the target avoids stale swap-chain
         * pixels from an in-place HwndRenderTarget::Resize appearing beside
         * the newly laid-out dial. This is only a resize-path cost. */
        discard_target(clock);
    }
    if (!clock->target) {
        if (FAILED(create_target(clock, hwnd, width, height))) {
            paint_fallback(hwnd);
            return;
        }
    }

    ID2D1RenderTarget *target = (ID2D1RenderTarget *)clock->target;
    ID2D1RenderTarget_BeginDraw(target);
    D2D1_COLOR_F page = rgb(255, 255, 255);
    ID2D1RenderTarget_Clear(target, &page);
    ID2D1RenderTarget_SetAntialiasMode(target, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float dw = (float)width, dh = (float)height;
    float size = dw < dh ? dw : dh;
    if (size > 40.0f) {
        /* Exactly two display states, and NO words on the face: the time
         * (hands + uncertainty fan), or the bare dial with no hands at
         * all. State words live in the status bar only. */
        int live = clock->hasTime && !clock->stopped;
        draw_dial(clock, dw * 0.5f, dh * 0.5f, size, live);
    }

    HRESULT hr = ID2D1RenderTarget_EndDraw(target, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        discard_target(clock);
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static void request_redraw(ClockWidget *clock) {
    if (!clock->redrawPending) {
        clock->redrawPending = 1;
        Tcl_DoWhenIdle(clock_redraw, clock);
    }
}

static void sweep_tick(void *clientData) {
    ClockWidget *clock = clientData;
    clock->sweepTimer = NULL;
    request_redraw(clock);   /* clock_redraw re-arms while still eligible */
}

/* Keep the ~30 fps sweep timer alive exactly while a dial is being shown:
 * hasTime, not stopped, and the window is mapped. The word screens and the
 * stopped face are static, so the widget goes fully idle there. */
static void sweep_manage(ClockWidget *clock) {
    int want = clock->tkwin && Tk_IsMapped(clock->tkwin)
               && clock->hasTime && !clock->stopped;
    if (want && !clock->sweepTimer) {
        clock->sweepTimer = Tcl_CreateTimerHandler(SWEEP_FRAME_MS,
                                                   sweep_tick, clock);
    } else if (!want && clock->sweepTimer) {
        Tcl_DeleteTimerHandler(clock->sweepTimer);
        clock->sweepTimer = NULL;
    }
}

static void clock_event(void *clientData, XEvent *eventPtr) {
    ClockWidget *clock = clientData;
    if (eventPtr->type == Expose) {
        request_redraw(clock);
    } else if (eventPtr->type == ConfigureNotify) {
        request_redraw(clock);
    } else if (eventPtr->type == DestroyNotify) {
        clock->tkwin = NULL;
        if (clock->command) Tcl_DeleteCommandFromToken(clock->interp, clock->command);
    }
}

static ClockTrust parse_trust(const char *name, int *ok) {
    *ok = 1;
    if (strcmp(name, "ok") == 0) return CLOCK_TRUST_OK;
    if (strcmp(name, "holdover") == 0) return CLOCK_TRUST_HOLDOVER;
    if (strcmp(name, "reacquiring") == 0) return CLOCK_TRUST_REACQUIRING;
    if (strcmp(name, "inop") == 0) return CLOCK_TRUST_INOP;
    *ok = 0;
    return CLOCK_TRUST_INOP;
}

/* armed.dat's portable on-disk representation: 12 characters, :00 through
 * :55.  Keeping the native widget on that same representation avoids a
 * second source of truth for the alarms. */
static int parse_armed_mask(Tcl_Interp *interp, Tcl_Obj *obj, unsigned short *mask) {
    const char *text = Tcl_GetString(obj);
    if (strlen(text) != 12) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("armed marker mask must contain 12 digits", -1));
        return TCL_ERROR;
    }
    unsigned short value = 0;
    for (int i = 0; i < 12; ++i) {
        if (text[i] == '1') value |= (unsigned short)(1u << i);
        else if (text[i] != '0') {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("armed marker mask must contain only 0 or 1", -1));
            return TCL_ERROR;
        }
    }
    *mask = value;
    return TCL_OK;
}

static void clock_command_deleted(void *clientData) {
    ClockWidget *clock = clientData;
    if (clock->sweepTimer) {
        Tcl_DeleteTimerHandler(clock->sweepTimer);
        clock->sweepTimer = NULL;
    }
    if (clock->redrawPending) Tcl_CancelIdleCall(clock_redraw, clock);
    if (clock->tkwin) {
        Tk_DeleteEventHandler(clock->tkwin, ExposureMask | StructureNotifyMask,
                              clock_event, clock);
        Tk_DestroyWindow(clock->tkwin);
        clock->tkwin = NULL;
    }
    discard_target(clock);
    ckfree(clock);
}

static int clock_widget_command(void *clientData, Tcl_Interp *interp,
                                int objc, Tcl_Obj *const objv[]) {
    ClockWidget *clock = clientData;
    static const char *usage =
        "show hour minute second millisecond hasTime state synced boundMs stopped ?armedMask? | armed mask | redraw";
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, usage);
        return TCL_ERROR;
    }
    const char *subcommand = Tcl_GetString(objv[1]);
    if (strcmp(subcommand, "redraw") == 0 && objc == 2) {
        request_redraw(clock);
        return TCL_OK;
    }
    if (strcmp(subcommand, "armed") == 0 && objc == 3) {
        unsigned short mask;
        if (parse_armed_mask(interp, objv[2], &mask) != TCL_OK) return TCL_ERROR;
        if (clock->armedMask != mask) {
            clock->armedMask = mask;
            request_redraw(clock);
        }
        return TCL_OK;
    }
    if (strcmp(subcommand, "show") != 0 || (objc != 11 && objc != 12)) {
        Tcl_WrongNumArgs(interp, 1, objv, usage);
        return TCL_ERROR;
    }
    int hour, minute, second, millisecond, hasTime, synced, boundMs, stopped, trustOk;
    unsigned short armedMask = clock->armedMask;
    if (Tcl_GetIntFromObj(interp, objv[2], &hour) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[3], &minute) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[4], &second) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[5], &millisecond) != TCL_OK ||
        Tcl_GetBooleanFromObj(interp, objv[6], &hasTime) != TCL_OK ||
        Tcl_GetBooleanFromObj(interp, objv[8], &synced) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[9], &boundMs) != TCL_OK ||
        Tcl_GetBooleanFromObj(interp, objv[10], &stopped) != TCL_OK) return TCL_ERROR;
    if (objc == 12 && parse_armed_mask(interp, objv[11], &armedMask) != TCL_OK) return TCL_ERROR;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || millisecond < 0 || millisecond > 999) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("time components out of range", -1));
        return TCL_ERROR;
    }
    if (boundMs < 0) boundMs = 0;
    ClockTrust trust = parse_trust(Tcl_GetString(objv[7]), &trustOk);
    if (!trustOk) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("unknown clock trust state", -1));
        return TCL_ERROR;
    }
    /* This feed's time components are current NOW: restart the sweep
     * extrapolation from this instant (even when the fields are equal --
     * a fresh feed still re-bases the sweep). */
    if (hasTime) clock->baseQpc = qpc_now();
    if (clock->hour != hour || clock->minute != minute || clock->second != second ||
        clock->millisecond != millisecond ||
        clock->hasTime != hasTime || clock->synced != synced || clock->trust != trust ||
        clock->boundMs != boundMs || clock->stopped != stopped ||
        clock->armedMask != armedMask) {
        clock->hour = hour;
        clock->minute = minute;
        clock->second = second;
        clock->millisecond = millisecond;
        clock->hasTime = hasTime;
        clock->synced = synced;
        clock->trust = trust;
        clock->boundMs = boundMs;
        clock->stopped = stopped;
        clock->armedMask = armedMask;
        request_redraw(clock);
    }
    return TCL_OK;
}

static int clock_create_command(void *clientData, Tcl_Interp *interp,
                                int objc, Tcl_Obj *const objv[]) {
    Tk_Window mainWindow = clientData;
    if (objc < 2 || (objc - 2) % 2 != 0) {
        Tcl_WrongNumArgs(interp, 1, objv, "pathName ?-width pixels -height pixels?");
        return TCL_ERROR;
    }
    int width = 440, height = 440;
    for (int i = 2; i < objc; i += 2) {
        const char *option = Tcl_GetString(objv[i]);
        if (strcmp(option, "-width") == 0) {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &width) != TCL_OK) return TCL_ERROR;
        } else if (strcmp(option, "-height") == 0) {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &height) != TCL_OK) return TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option %s", option));
            return TCL_ERROR;
        }
    }
    if (width < 40 || height < 40) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("clock widget must be at least 40 pixels", -1));
        return TCL_ERROR;
    }
    Tk_Window window = Tk_CreateWindowFromPath(interp, mainWindow,
                                                Tcl_GetString(objv[1]), NULL);
    if (!window) return TCL_ERROR;
    ClockWidget *clock = ckalloc(sizeof *clock);
    memset(clock, 0, sizeof *clock);
    clock->tkwin = window;
    clock->interp = interp;
    clock->width = width;
    clock->height = height;
    clock->trust = CLOCK_TRUST_INOP;
    Tk_SetClass(window, "LunarClock");
    Tk_GeometryRequest(window, width, height);
    Tk_CreateEventHandler(window, ExposureMask | StructureNotifyMask,
                          clock_event, clock);
    clock->command = Tcl_CreateObjCommand(interp, Tk_PathName(window),
                                           clock_widget_command, clock,
                                           clock_command_deleted);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(Tk_PathName(window), -1));
    return TCL_OK;
}

int LunarClock_Init(Tcl_Interp *interp) {
    if (Tk_InitStubs(interp, "9.0", 0) == NULL) return TCL_ERROR;
    /* Global widget-class command, Tk style (button, canvas, ...). It must
     * NOT live inside ::lunar: a ::lunar::clock command shadows Tcl's own
     * [clock] for every proc defined in that namespace (namespace-first
     * resolution), silently breaking [clock milliseconds]/[clock format]
     * throughout the UI -- and only in the packaged exe, where this widget
     * is registered. */
    Tcl_CreateObjCommand(interp, "lunarclock", clock_create_command,
                         Tk_MainWindow(interp), NULL);
    return TCL_OK;
}
