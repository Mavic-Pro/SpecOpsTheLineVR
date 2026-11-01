#include "screenshot.h"
#include "logger.h"

#include <atomic>
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
            vr::log::Line("SCREENSHOT: GetBackBuffer failed.");
            break;
        }
        D3DSURFACE_DESC desc;
        backBuffer->GetDesc(&desc);
        if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
            vr::log::Line("SCREENSHOT: unsupported back buffer format (" +
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
            std::snprintf(buf, sizeof(buf), "SCREENSHOT: copy to render target failed hr=0x%08lX", hr);
            vr::log::Line(buf);
            break;
        }
        hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
        if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(resolved, sysmem);
        if (FAILED(hr)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "SCREENSHOT: copy to system memory failed hr=0x%08lX", hr);
            vr::log::Line(buf);
            break;
        }

        D3DLOCKED_RECT lr;
        if (FAILED(sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            vr::log::Line("SCREENSHOT: LockRect failed.");
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
        vr::log::Line(std::string("SCREENSHOT ") + (ok ? "saved: " : "write failed: ") + path);
    } while (false);

    SafeRelease(sysmem);
    SafeRelease(resolved);
    SafeRelease(backBuffer);
    return ok;
}

// "thumbs N": miniatura di ogni occhio a ogni frame, quello che va al visore (1/11). Un frame sbagliato per una sola
// volta (occhio zoomato a un cambio di ScreenPercentage) non si vede da nessun'altra misura ne' da uno "shot".
// In memoria durante la cattura, su file alla fine: [eye, pct, n, 0] int32 + kThumbW*kThumbH*3 byte BGR, per frame.
namespace {
constexpr int kThumbW = 64, kThumbH = 64;
std::atomic<int> g_thumbsLeft{0};
std::string g_thumbsPath;
std::vector<uint8_t> g_thumbs;
int g_thumbCount = 0;
IDirect3DSurface9* g_thumbRt = nullptr;
IDirect3DSurface9* g_thumbSys = nullptr;
}  // namespace

void StartThumbs(int frames, const std::string& path) {
    if (g_thumbsLeft.load() > 0 || frames <= 0) return;
    g_thumbsPath = path;
    g_thumbsLeft = frames;
}

bool ThumbsActive() { return g_thumbsLeft.load(std::memory_order_relaxed) > 0; }

void ThumbFrame(IDirect3DDevice9* device, int eye, int pct) {
    if (!g_thumbRt) {
        if (FAILED(device->CreateRenderTarget(kThumbW, kThumbH, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_thumbRt, nullptr)) ||
            FAILED(device->CreateOffscreenPlainSurface(kThumbW, kThumbH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &g_thumbSys, nullptr))) {
            vr::log::Line("THUMBS: could not create the surfaces.");
            SafeRelease(g_thumbSys);
            SafeRelease(g_thumbRt);
            g_thumbsLeft = 0;
            return;
        }
        g_thumbs.clear();
        g_thumbCount = 0;
    }
    IDirect3DSurface9* backBuffer = nullptr;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
        SUCCEEDED(device->StretchRect(backBuffer, nullptr, g_thumbRt, nullptr, D3DTEXF_LINEAR)) &&
        SUCCEEDED(device->GetRenderTargetData(g_thumbRt, g_thumbSys)) &&
        SUCCEEDED(g_thumbSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        const int32_t head[4] = {eye, pct, g_thumbCount, 0};
        const auto* hb = reinterpret_cast<const uint8_t*>(head);
        g_thumbs.insert(g_thumbs.end(), hb, hb + sizeof(head));
        for (int y = 0; y < kThumbH; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
            for (int x = 0; x < kThumbW; ++x) {
                g_thumbs.push_back(row[x * 4]);
                g_thumbs.push_back(row[x * 4 + 1]);
                g_thumbs.push_back(row[x * 4 + 2]);
            }
        }
        g_thumbSys->UnlockRect();
        ++g_thumbCount;
    }
    SafeRelease(backBuffer);
    if (g_thumbsLeft.fetch_sub(1) == 1) {
        FILE* f = nullptr;
        if (fopen_s(&f, g_thumbsPath.c_str(), "wb") == 0 && f) {
            fwrite(g_thumbs.data(), 1, g_thumbs.size(), f);
            fclose(f);
        }
        vr::log::Line("THUMBS: " + std::to_string(g_thumbCount) + " frames saved: " + g_thumbsPath);
        std::vector<uint8_t>().swap(g_thumbs);
        SafeRelease(g_thumbSys);
        SafeRelease(g_thumbRt);
    }
}

}  // namespace vr::screenshot
