// Diagnostica audio: nome del dispositivo di riproduzione predefinito di
// Windows nel log (22/10: "mancava l'audio in questa partita" e nel log non
// c'era nulla con cui capire se il gioco suonava sul visore o sul PC).
#include "audio_diag.h"
#include "logger.h"
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>

namespace vr::audio {

std::string DefaultDeviceName() {
    std::string name = "(unknown)";
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) && en) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev) {
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
                    char buf[256];
                    int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, buf, sizeof(buf), nullptr, nullptr);
                    if (n > 0) name = buf;
                }
                PropVariantClear(&v);
                props->Release();
            }
            dev->Release();
        } else {
            name = "(no default playback device)";
        }
        en->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return name;
}

void LogDefaultDevice(const char* when) {
    static std::string last;
    std::string now = DefaultDeviceName();
    if (now == last) return;
    last = now;
    vr::log::Line(std::string("AUDIO: Windows default playback device ") + when + ": " + now);
}

}  // namespace vr::audio
