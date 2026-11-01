#pragma once
#include <d3d9.h>
#include <string>

namespace vr::screenshot {

// Salva il back buffer corrente come BMP a 24 bit, ridotto a meta'
// risoluzione. Da chiamare in Present, prima dell'originale. Funziona anche
// a schermo intero esclusivo (a differenza di una cattura del desktop).
bool SaveBackBuffer(IDirect3DDevice9* device, const std::string& path);

// Collaudo: miniatura di ogni occhio a ogni frame per N frame ("thumbs N"), su file alla fine.
void StartThumbs(int frames, const std::string& path);
bool ThumbsActive();
void ThumbFrame(IDirect3DDevice9* device, int eye, int pct);  // in Present, thread di rendering

}  // namespace vr::screenshot
