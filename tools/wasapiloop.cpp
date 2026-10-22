// wasapiloop <out.wav> [device substring]: records what a playback device
// plays (WASAPI loopback of the Windows default render device, or of the
// first active one whose name contains the substring) into a 16-bit PCM WAV
// until a file named <out.wav>.stop appears. Used by tools/mirrorrec.py to
// record the game's audio (the headset device) next to the mirror video.
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

static std::wstring Name(IMMDevice* dev) {
    std::wstring n;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) n = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return n;
}

static void WriteWavHeader(FILE* f, uint32_t dataBytes, uint32_t rate, uint16_t ch) {
    uint16_t bits = 16, align = ch * 2;
    uint32_t bytesPerSec = rate * align, fmtLen = 16, riffLen = 36 + dataBytes, tag = 1;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffLen, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f);
    fwrite(&tag, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&bytesPerSec, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: wasapiloop <out.wav> [device substring]\n");
        return 2;
    }
    std::wstring out = argv[1], want = argc > 2 ? argv[2] : L"", stopFile = out + L".stop";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) return 3;
    IMMDevice* dev = nullptr;
    if (want.empty()) {
        en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    } else {
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            UINT n = 0;
            coll->GetCount(&n);
            for (UINT i = 0; i < n && !dev; ++i) {
                IMMDevice* d = nullptr;
                if (FAILED(coll->Item(i, &d)) || !d) continue;
                if (Name(d).find(want) != std::wstring::npos) dev = d;
                else d->Release();
            }
            coll->Release();
        }
    }
    if (!dev) {
        fwprintf(stderr, L"playback device not found\n");
        return 4;
    }
    IAudioClient* client = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) || !client) return 5;
    WAVEFORMATEX* mix = nullptr;
    client->GetMixFormat(&mix);
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, mix, nullptr);
    if (FAILED(hr)) {
        fwprintf(stderr, L"Initialize failed 0x%08lX\n", hr);
        return 6;
    }
    IAudioCaptureClient* cap = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)) || !cap) return 7;
    bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    IsEqualGUID(((WAVEFORMATEXTENSIBLE*)mix)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    if (!isFloat || mix->wBitsPerSample != 32) {
        fwprintf(stderr, L"unexpected mix format (not 32-bit float)\n");
        return 8;
    }
    uint16_t ch = mix->nChannels, outCh = ch > 2 ? 2 : ch;
    FILE* f = _wfopen(out.c_str(), L"wb");
    if (!f) return 9;
    WriteWavHeader(f, 0, mix->nSamplesPerSec, outCh);
    fwprintf(stderr, L"recording \"%s\" %u Hz %u ch -> %s\n", Name(dev).c_str(), mix->nSamplesPerSec, ch, out.c_str());
    client->Start();
    uint32_t dataBytes = 0;
    std::vector<int16_t> buf;
    for (;;) {
        if (GetFileAttributesW(stopFile.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        Sleep(20);
        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            buf.resize(frames * outCh);
            const float* in = (const float*)data;
            for (UINT32 i = 0; i < frames; ++i)
                for (uint16_t c = 0; c < outCh; ++c) {
                    float v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : in[i * ch + c];
                    v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                    buf[i * outCh + c] = (int16_t)(v * 32767.0f);
                }
            fwrite(buf.data(), 2, buf.size(), f);
            dataBytes += (uint32_t)(buf.size() * 2);
            cap->ReleaseBuffer(frames);
        }
    }
    client->Stop();
    WriteWavHeader(f, dataBytes, mix->nSamplesPerSec, outCh);
    fclose(f);
    DeleteFileW(stopFile.c_str());
    fwprintf(stderr, L"stopped, %u bytes of audio\n", dataBytes);
    return 0;
}
