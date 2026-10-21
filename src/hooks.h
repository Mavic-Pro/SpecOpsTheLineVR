#pragma once
#include <d3d9.h>

namespace vr::hooks {

// Modalita' sperimentale D3D9Ex (file "specopsvr_gpushare" accanto alla dll):
// device D3D9Ex, risorse MANAGED convertite, ritaglio degli occhi condiviso
// con D3D11 senza passare dalla CPU.
bool GpuShareRequested();
// PlayerController del giocatore verificato vivo (GObjects), oppure null.
void* PlayerControllerAlive();
extern bool g_d3d9ex;

// Aggancia IDirect3D9::CreateDevice sull'oggetto appena creato dal vero d3d9.dll.
// Da qui, al primo CreateDevice riuscito, agganciamo anche i metodi del device
// (EndScene, Present, SetTransform, SetVertexShaderConstantF) che serviranno
// nelle fasi successive per individuare le matrici di camera e iniettare lo stereo.
bool InstallIDirect3D9Hook(IDirect3D9* realD3D9);

}  // namespace vr::hooks
