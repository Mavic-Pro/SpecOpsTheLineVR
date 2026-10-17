#pragma once
#include <d3d9.h>

namespace vr::realdll {

// Carica il vero d3d9.dll di sistema (System32/SysWOW64), bypassando
// la risoluzione standard del nome per non ricaricare questo stesso proxy.
bool Load();

typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT (WINAPI* Direct3DCreate9Ex_t)(UINT SDKVersion, IDirect3D9Ex** ppD3D);
typedef int (WINAPI* D3DPERF_BeginEvent_t)(D3DCOLOR col, LPCWSTR wszName);
typedef int (WINAPI* D3DPERF_EndEvent_t)(void);
typedef void (WINAPI* D3DPERF_SetMarker_t)(D3DCOLOR col, LPCWSTR wszName);
typedef void (WINAPI* D3DPERF_SetRegion_t)(D3DCOLOR col, LPCWSTR wszName);
typedef BOOL (WINAPI* D3DPERF_QueryRepeatFrame_t)(void);
typedef void (WINAPI* D3DPERF_SetOptions_t)(DWORD dwOptions);
typedef DWORD (WINAPI* D3DPERF_GetStatus_t)(void);

extern Direct3DCreate9_t Direct3DCreate9_Real;
extern Direct3DCreate9Ex_t Direct3DCreate9Ex_Real;
extern D3DPERF_BeginEvent_t D3DPERF_BeginEvent_Real;
extern D3DPERF_EndEvent_t D3DPERF_EndEvent_Real;
extern D3DPERF_SetMarker_t D3DPERF_SetMarker_Real;
extern D3DPERF_SetRegion_t D3DPERF_SetRegion_Real;
extern D3DPERF_QueryRepeatFrame_t D3DPERF_QueryRepeatFrame_Real;
extern D3DPERF_SetOptions_t D3DPERF_SetOptions_Real;
extern D3DPERF_GetStatus_t D3DPERF_GetStatus_Real;

}  // namespace vr::realdll
