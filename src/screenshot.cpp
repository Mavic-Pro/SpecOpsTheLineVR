#include "screenshot.h"
#include "logger.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace vr::screenshot {

namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool WriteBmp24(const std::string& path, int w, int h, const std::vector<uint8_t>& rgbTopDown) {
    int rowBytes = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + rowBytes * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;  // positivo = bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> pad(rowBytes - w * 3, 0);
    for (int y = h - 1; y >= 0; --y) {
        fwrite(&rgbTopDown[static_cast<size_t>(y) * w * 3], 1, w * 3, f);
        if (!pad.empty()) fwrite(pad.data(), 1, pad.size(), f);
    }
    fclose(f);
    return true;
}

}  // namespace

bool SaveBackBuffer(IDirect3DDevice9* device, const std::string& path) {
    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DSurface9* resolved = nullptr;
    IDirect3DSurface9* sysmem = nullptr;
    bool ok = false;

    do {
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
            vr::log::Line("SCREENSHOT: GetBackBuffer fallita.");
            break;
        }
        D3DSURFACE_DESC desc;
        backBuffer->GetDesc(&desc);
        if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
            vr::log::Line("SCREENSHOT: formato back buffer non supportato (" +
                          std::to_string(static_cast<int>(desc.Format)) + ").");
            break;
        }

        // Copia SEMPRE in un render target intermedio non multisample:
        // GetRenderTargetData direttamente sul back buffer funzionava sulla
        // schermata del titolo ma falliva gia' nel menu.
        HRESULT hr = device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8,
                                                D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
        if (SUCCEEDED(hr)) hr = device->StretchRect(backBuffer, nullptr, resolved, nullptr, D3DTEXF_NONE);
        if (FAILED(hr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "SCREENSHOT: copia in render target fallita hr=0x%08lX", hr);
            vr::log::Line(buf);
            break;
        }
        hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
        if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(resolved, sysmem);
        if (FAILED(hr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "SCREENSHOT: copia in memoria di sistema fallita hr=0x%08lX", hr);
            vr::log::Line(buf);
            break;
        }

        D3DLOCKED_RECT lr;
        if (FAILED(sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            vr::log::Line("SCREENSHOT: LockRect fallita.");
            break;
        }
        int w = static_cast<int>(desc.Width) / 2;
        int h = static_cast<int>(desc.Height) / 2;
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y * 2) * lr.Pitch;
            for (int x = 0; x < w; ++x) {
                const uint8_t* px = row + x * 2 * 4;  // BGRA
                uint8_t* out = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                out[0] = px[0];
                out[1] = px[1];
                out[2] = px[2];
            }
        }
        sysmem->UnlockRect();

        ok = WriteBmp24(path, w, h, rgb);
        vr::log::Line(std::string("SCREENSHOT ") + (ok ? "salvato: " : "scrittura fallita: ") + path);
    } while (false);

    SafeRelease(sysmem);
    SafeRelease(resolved);
    SafeRelease(backBuffer);
    return ok;
}

}  // namespace vr::screenshot
