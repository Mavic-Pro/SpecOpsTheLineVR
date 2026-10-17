"""Cattura la finestra "Headset Window" del driver null di SteamVR (cio' che
il compositor manderebbe al visore) in shots/<nome>.png.

    python tools/hmdwindow.py nome
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
u = ctypes.WinDLL("user32")
g = ctypes.WinDLL("gdi32")


def find_window(title="Headset Window"):
    return u.FindWindowW(None, title)


def capture(hwnd):
    r = wt.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    hdc = u.GetDC(hwnd)
    mdc = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(mdc, bmp)
    # PW_CLIENTONLY | PW_RENDERFULLCONTENT: funziona anche con finestre D3D
    # coperte da altre finestre.
    u.PrintWindow(hwnd, mdc, 1 | 2)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    u.ReleaseDC(hwnd, hdc)
    return Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1).convert("RGB")


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "hmd"
    hwnd = find_window()
    if not hwnd:
        sys.exit("finestra 'Headset Window' non trovata (driver null di SteamVR non attivo?)")
    img = capture(hwnd)
    path = os.path.join(ROOT, "shots", name + ".png")
    img.save(path)
    print(path, img.size)


if __name__ == "__main__":
    main()
