/* cap.c -- occlusion-proof window capture for the screenshot tool.
 *
 * `lunarcap::window <hwnd>` renders a specific window with PrintWindow
 * (PW_RENDERFULLCONTENT) into an off-screen DC -- works even when the window
 * is occluded or in the background, with no foreground/clipboard/Snipping-Tool
 * dance -- and returns a DIB (BITMAPINFOHEADER + 32-bpp top-down BGRA pixels)
 * as a Tcl byte array, which tools/shot.tcl turns into a PNG via dib_to_photo.
 *
 * Ported verbatim from els/src/cap.c (namespace elscap -> lunarcap).
 * C23 + Tcl stubs; links user32 + gdi32. Built by `kuu.exe run build-ext`.
 */
#include <tcl.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

static int fail(Tcl_Interp *ip, const char *msg) {
    Tcl_SetObjResult(ip, Tcl_NewStringObj(msg, -1));
    return TCL_ERROR;
}

static int Window_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                      int objc, Tcl_Obj *const objv[]) {
    if (objc != 2) { Tcl_WrongNumArgs(ip, 1, objv, "hwnd"); return TCL_ERROR; }
    Tcl_WideInt hv;
    if (Tcl_GetWideIntFromObj(ip, objv[1], &hv) != TCL_OK) return TCL_ERROR;
    HWND hwnd = (HWND)(intptr_t)hv;
    if (!IsWindow(hwnd)) return fail(ip, "no such window");

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return fail(ip, "GetWindowRect failed");
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return fail(ip, "window has zero size");

    HDC screen = GetDC(NULL);
    if (!screen) { return fail(ip, "GetDC failed"); }
    HDC mem     = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!mem || !bmp) {
        if (bmp) { DeleteObject(bmp); }
        if (mem) { DeleteDC(mem); }
        ReleaseDC(NULL, screen);
        return fail(ip, "GDI bitmap/DC allocation failed");
    }
    HGDIOBJ old = SelectObject(mem, bmp);

    BOOL ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    SelectObject(mem, old);   /* restore old; bmp must not be selected for GetDIBits */

    BITMAPINFOHEADER bih = {0};
    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = w;
    bih.biHeight      = -h;          /* negative => top-down rows */
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    Tcl_Size npix = (Tcl_Size)w * h * 4;
    unsigned char *buf = (unsigned char *)Tcl_Alloc(40 + npix);  /* Tcl_Alloc panics on OOM */
    memcpy(buf, &bih, 40);
    BITMAPINFOHEADER qih = bih;
    int lines = GetDIBits(screen, bmp, 0, (UINT)h, buf + 40,
                          (BITMAPINFO *)&qih, DIB_RGB_COLORS);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    if (!ok || lines == 0) { Tcl_Free((char *)buf); return fail(ip, "PrintWindow/GetDIBits failed"); }

    Tcl_SetObjResult(ip, Tcl_NewByteArrayObj(buf, 40 + npix));
    Tcl_Free((char *)buf);
    return TCL_OK;
}

int Cap_Init(Tcl_Interp *ip) {
    if (Tcl_InitStubs(ip, "9.0", 0) == nullptr) return TCL_ERROR;
    Tcl_CreateNamespace(ip, "lunarcap", nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "lunarcap::window", Window_Cmd, nullptr, nullptr);
    Tcl_PkgProvide(ip, "lunarcap", "0.1");
    return TCL_OK;
}
