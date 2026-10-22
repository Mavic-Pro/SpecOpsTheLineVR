// Backend OpenXR: vedi vr_openxr.h. Struttura del file:
//  1. matematica (matrici 3x4 <-> XrPosef)
//  2. caricamento diretto del runtime dal json (niente openxr_loader.dll)
//  3. stato: istanza, sessione, spazi, azioni, swapchain, overlay
//  4. frame loop (xrWaitFrame/Begin/End), layer, keepalive, puntatore laser
//  5. eventi e stato della sessione
//  6. le classi *Shim (interfacce OpenVR) e l'API pubblica
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <unknwn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "vr_openxr.h"
#include "xr_shim_ifaces.h"
#include "control.h"
#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vr::xr {

namespace {

// --- 1. matematica ------------------------------------------------------------

using Mat34 = vr::HmdMatrix34_t;

Mat34 Identity() {
    Mat34 m = {};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
    return m;
}

Mat34 Mul(const Mat34& a, const Mat34& b) {
    Mat34 r = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            float v = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) v += a.m[i][3];
            r.m[i][j] = v;
        }
    return r;
}

Mat34 Inverse(const Mat34& a) {  // rotazione + traslazione
    Mat34 r = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}

Mat34 FromPose(const XrPosef& p) {
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    Mat34 m;
    m.m[0][0] = 1 - 2 * (y * y + z * z); m.m[0][1] = 2 * (x * y - w * z);     m.m[0][2] = 2 * (x * z + w * y);     m.m[0][3] = p.position.x;
    m.m[1][0] = 2 * (x * y + w * z);     m.m[1][1] = 1 - 2 * (x * x + z * z); m.m[1][2] = 2 * (y * z - w * x);     m.m[1][3] = p.position.y;
    m.m[2][0] = 2 * (x * z - w * y);     m.m[2][1] = 2 * (y * z + w * x);     m.m[2][2] = 1 - 2 * (x * x + y * y); m.m[2][3] = p.position.z;
    return m;
}

XrPosef ToPose(const Mat34& m) {
    XrPosef p;
    p.position = {m.m[0][3], m.m[1][3], m.m[2][3]};
    float t = m.m[0][0] + m.m[1][1] + m.m[2][2];
    float x, y, z, w;
    if (t > 0.0f) {
        float s = std::sqrt(t + 1.0f) * 2.0f;
        w = 0.25f * s; x = (m.m[2][1] - m.m[1][2]) / s; y = (m.m[0][2] - m.m[2][0]) / s; z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
        w = (m.m[2][1] - m.m[1][2]) / s; x = 0.25f * s; y = (m.m[0][1] + m.m[1][0]) / s; z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
        w = (m.m[0][2] - m.m[2][0]) / s; x = (m.m[0][1] + m.m[1][0]) / s; y = 0.25f * s; z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
        w = (m.m[1][0] - m.m[0][1]) / s; x = (m.m[0][2] + m.m[2][0]) / s; y = (m.m[1][2] + m.m[2][1]) / s; z = 0.25f * s;
    }
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n < 1e-6f) { x = y = z = 0.0f; w = 1.0f; n = 1.0f; }
    p.orientation = {x / n, y / n, z / n, w / n};
    return p;
}

Mat34 RotX(float deg) {
    Mat34 m = Identity();
    float c = std::cos(deg * 0.01745329252f), s = std::sin(deg * 0.01745329252f);
    m.m[1][1] = c; m.m[1][2] = -s;
    m.m[2][1] = s; m.m[2][2] = c;
    return m;
}

double QpcMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * t.QuadPart / f.QuadPart;
}

// --- 2. caricamento del runtime -------------------------------------------------

#define XR_FUNCS(X)                                                                                                    \
    X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) X(xrGetInstanceProperties)      \
    X(xrPollEvent) X(xrResultToString) X(xrGetSystem) X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews)    \
    X(xrCreateSession) X(xrDestroySession) X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace) X(xrDestroySpace)   \
    X(xrLocateSpace) X(xrBeginSession) X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews)  \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages)           \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrStringToPath) X(xrPathToString) \
    X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings)                \
    X(xrAttachSessionActionSets) X(xrGetCurrentInteractionProfile) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) \
    X(xrGetActionStateVector2f) X(xrGetActionStatePose) X(xrSyncActions) X(xrCreateActionSpace) X(xrApplyHapticFeedback)

#define XR_EXT_FUNCS(X)                                                                                              \
    X(xrGetD3D11GraphicsRequirementsKHR) X(xrConvertWin32PerformanceCounterToTimeKHR) X(xrGetVisibilityMaskKHR)      \
    X(xrGetDisplayRefreshRateFB)

struct Fn {
#define X(name) PFN_##name name = nullptr;
    XR_FUNCS(X)
    XR_EXT_FUNCS(X)
#undef X
};
Fn fn;
PFN_xrGetInstanceProcAddr g_gipa = nullptr;
HMODULE g_runtimeDll = nullptr;
std::string g_runtimeJsonName;

std::string JsonString(const std::string& text, const char* key) {
    size_t k = text.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return "";
    size_t c = text.find(':', k);
    size_t q1 = text.find('"', c == std::string::npos ? k : c);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (size_t i = q1 + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            ++i;
            out += text[i] == 'n' ? '\n' : text[i];
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string ReadFileText(const std::string& path) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string s(static_cast<size_t>(GetFileSize(f, nullptr)), '\0');
    DWORD got = 0;
    if (!s.empty()) ReadFile(f, &s[0], static_cast<DWORD>(s.size()), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return s;
}

// Percorso del json del runtime attivo: override, XR_RUNTIME_JSON, registro
// (HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime; processo a 32 bit = vista WOW6432Node).
std::string RuntimeJsonPath(const std::string& overrideJson) {
    if (!overrideJson.empty()) return overrideJson;
    char env[MAX_PATH] = {};
    if (GetEnvironmentVariableA("XR_RUNTIME_JSON", env, MAX_PATH) && env[0]) return env;
    HKEY k = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &k) != ERROR_SUCCESS) return "";
    char buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - 1, type = 0;
    LONG r = RegQueryValueExA(k, "ActiveRuntime", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_SZ ? std::string(buf) : "";
}

std::string FileName(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    for (auto& c : n) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return n;
}

bool LoadRuntime(const std::string& overrideJson, std::string* error) {
    if (g_gipa) return true;
    std::string json = RuntimeJsonPath(overrideJson);
    if (json.empty()) {
        if (error) *error = "no OpenXR runtime set (Khronos\\OpenXR\\1\\ActiveRuntime)";
        return false;
    }
    std::string text = ReadFileText(json);
    std::string lib = JsonString(text, "library_path");
    if (lib.empty()) {
        if (error) *error = "cannot read library_path from " + json;
        return false;
    }
    if (lib.find(':') == std::string::npos && lib.rfind("\\\\", 0) != 0) {
        size_t s = json.find_last_of("\\/");
        if (s != std::string::npos) lib = json.substr(0, s + 1) + lib;
    }
    HMODULE h = LoadLibraryExA(lib.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        if (error) *error = "cannot load " + lib + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(h, "xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) {
        FreeLibrary(h);
        if (error) *error = lib + " has no xrNegotiateLoaderRuntimeInterface";
        return false;
    }
    XrNegotiateLoaderInfo li = {};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr = {};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof(rr);
    XrResult r = negotiate(&li, &rr);
    if (XR_FAILED(r) || !rr.getInstanceProcAddr) {
        FreeLibrary(h);
        if (error) *error = "runtime negotiation failed (" + std::to_string(r) + ") for " + lib;
        return false;
    }
    g_runtimeDll = h;
    g_gipa = rr.getInstanceProcAddr;
    g_runtimeJsonName = FileName(json);
    vr::log::Line("OPENXR: runtime " + lib + " (from " + json + ").");
    return true;
}

template <typename T>
void Load(XrInstance inst, const char* name, T* out) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(g_gipa(inst, name, &f))) f = nullptr;
    *out = reinterpret_cast<T>(f);
}

// --- 3. stato -------------------------------------------------------------------

constexpr vr::TrackedDeviceIndex_t kHmd = 0, kLeft = 1, kRight = 2;
constexpr int kActStick = 0, kActTrigger = 1, kActGrip = 2, kActPrimary = 3, kActSecondary = 4, kActStickClick = 5,
              kActMenu = 6, kActDpadL = 7, kActDpadR = 8, kActHaptic = 9, kActPose = 10, kActCount = 11;
const char* const kActNames[kActCount] = {"stick", "trigger", "grip", "primary", "secondary", "stickclick", "menu",
                                          "dpadleft", "dpadright", "haptic", "pose"};

struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    int64_t fmt = 0;
    std::vector<ID3D11Texture2D*> images;
    bool released = false;  // almeno un'immagine rilasciata: il layer puo' usarla
};

enum class Attach { Absolute, Hmd, Hand };

struct OverlayItem {
    vr::VROverlayHandle_t handle = 0;
    std::string key, name;
    bool visible = false;
    float width = 1.0f;
    uint32_t sortOrder = 0;
    uint32_t flags = 0;
    float alpha = 1.0f;
    vr::VROverlayInputMethod input = vr::VROverlayInputMethod_None;
    vr::HmdVector2_t mouseScale = {1.0f, 1.0f};
    vr::VRTextureBounds_t bounds = {0, 0, 1, 1};
    Attach attach = Attach::Absolute;
    int hand = 1;
    Mat34 xform = Identity();
    // Contenuto: pixel RGBA dalla CPU oppure una texture D3D11 del device condiviso.
    std::vector<uint8_t> pixels;
    uint32_t pixW = 0, pixH = 0;
    ID3D11Texture2D* tex = nullptr;
    bool dirty = false;
    Swap swap;
    bool internal = false;  // puntino del laser
    // Puntatore.
    bool hovered = false;
    float lastU = -1.0f, lastV = -1.0f;
    std::deque<vr::VREvent_t> events;
};

struct HandState {
    bool active = false;      // azioni sincronizzate e mano presente
    bool poseActive = false;
    bool connected = false;   // profilo di interazione assegnato
    XrVector2f stick = {};
    float trigger = 0.0f, grip = 0.0f;
    bool primary = false, secondary = false, stickClick = false, menu = false;
    std::string profile;      // es. /interaction_profiles/oculus/touch_controller
    std::string controllerType;
};

struct State {
    std::recursive_mutex mutex;  // tutto lo stato sotto; mai tenuto durante xrWaitFrame
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    std::atomic<bool> running{false};
    std::atomic<bool> dead{false};
    std::atomic<bool> focused{false};
    bool presenceKnown = false, present = true;
    std::string runtimeName, systemName;
    char runtimeVersion[64] = {};
    bool extPerfCounter = false, extVisMask = false, extRefreshRate = false, extPresence = false;
    XrSpace stage = XR_NULL_HANDLE, view = XR_NULL_HANDLE, handSpace[2] = {};
    XrReferenceSpaceType stageType = XR_REFERENCE_SPACE_TYPE_STAGE;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    int32_t adapterIndex = 0;
    XrViewConfigurationView viewCfg[2] = {};
    int64_t eyeFormat = 0, overlayFormat = 0;
    // Viste del frame in corso (spazio stage) e occhio->testa (spazio view).
    XrView frameViews[2] = {};
    bool frameViewsValid = false;
    XrView eyeInHead[2] = {};
    bool eyeInHeadValid = false;
    std::atomic<float> hz{0.0f};
    // Frame.
    std::mutex frameMutex;
    bool frameBegun = false;
    DWORD frameOwner = 0;
    double frameBeginMs = 0.0, lastEndMs = 0.0;
    XrFrameState frameState = {};
    XrTime lastPredicted = 0;
    XrDuration lastPeriod = 0;
    unsigned frames = 0, eyeFrames = 0, keepalives = 0;
    // Occhi.
    Swap eye[2];
    struct EyeSubmit {
        ID3D11Texture2D* tex = nullptr;
        vr::VRTextureBounds_t bounds = {0, 0, 1, 1};
        bool hasPose = false;
        Mat34 pose = {};
        bool set = false;
    } submit[2];
    XrCompositionLayerProjectionView projViews[2] = {};
    bool projValid = false;
    double projMs = 0.0;
    // OverlayItem.
    std::map<vr::VROverlayHandle_t, OverlayItem> overlays;
    vr::VROverlayHandle_t nextOverlay = 1;
    vr::VROverlayHandle_t cursor = 0;  // puntino del laser (overlay interno)
    // Dissolvenze.
    float fadeFrom = 0.0f, fadeTo = 0.0f, fadeAlpha = 0.0f;
    double fadeStartMs = 0.0, fadeDurMs = 0.0;
    Swap fadeSwap;
    float fadeUploaded = -1.0f;
    // Azioni.
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction action[2][kActCount] = {};
    XrPath handPath[2] = {};
    HandState hand[2];
    double lastSyncMs = 0.0;
    uint32_t packet = 1;
    double lastProfileMs = 0.0;
    bool gripFix = true;
    float gripPitch = 20.6f;
    bool gripFixDirty = false;
    // Puntatore.
    bool triggerWas[2] = {};
    int scrollLatch[2] = {};
    double scrollRepeatMs[2] = {};
    // Eventi di sistema (IVRSystem::PollNextEvent).
    std::deque<vr::VREvent_t> events;
    // Maschere nascoste per occhio (triangoli, UV 0..1 con v=0 in alto).
    std::vector<vr::HmdVector2_t> ham[2];
    bool hamTried[2] = {};
    std::thread keepalive;
    std::atomic<bool> stop{false};
};
State g;

std::string ResultName(XrResult r) {
    char b[XR_MAX_RESULT_STRING_SIZE] = {};
    if (g.instance && fn.xrResultToString && XR_SUCCEEDED(fn.xrResultToString(g.instance, r, b))) return b;
    return std::to_string(static_cast<int>(r));
}

bool Check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    vr::log::Line(std::string("OPENXR: ") + what + " failed: " + ResultName(r));
    if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST || r == XR_ERROR_RUNTIME_FAILURE) g.dead = true;
    return false;
}

XrTime Now() {
    if (g.extPerfCounter && fn.xrConvertWin32PerformanceCounterToTimeKHR) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        XrTime out = 0;
        if (XR_SUCCEEDED(fn.xrConvertWin32PerformanceCounterToTimeKHR(g.instance, &t, &out))) return out;
    }
    // Senza l'estensione: tempo del frame piu' il tempo passato da allora.
    return g.lastPredicted + static_cast<XrTime>((QpcMs() - g.frameBeginMs) * 1e6);
}

// Trasformazione grip -> raw dei Touch (componente openxr_grip del modello di SteamVR).
Mat34 GripToRaw(int hand) {
    Mat34 c = RotX(g.gripPitch);
    c.m[0][3] = hand == 0 ? 0.007f : -0.007f;
    c.m[1][3] = -0.00182941f;
    c.m[2][3] = 0.1019482f;
    return Inverse(c);
}

bool CreateHandSpaces() {
    for (int h = 0; h < 2; ++h) {
        if (g.handSpace[h]) fn.xrDestroySpace(g.handSpace[h]);
        g.handSpace[h] = XR_NULL_HANDLE;
        XrActionSpaceCreateInfo ci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = g.action[h][kActPose];
        ci.subactionPath = g.handPath[h];
        ci.poseInActionSpace = g.gripFix ? ToPose(GripToRaw(h)) : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
        if (!Check(fn.xrCreateActionSpace(g.session, &ci, &g.handSpace[h]), "xrCreateActionSpace")) return false;
    }
    g.gripFixDirty = false;
    return true;
}

// Binding suggeriti per profilo: percorsi relativi a /user/hand/<side>/.
struct Profile {
    const char* path;
    const char* ext;  // estensione necessaria (nullptr = core)
    const char* stick, *trigger, *grip, *primaryL, *primaryR, *secondaryL, *secondaryR, *stickClick, *menuL, *menuR;
    const char* type;  // Prop_ControllerType_String di SteamVR
};
const Profile kProfiles[] = {
    {"/interaction_profiles/oculus/touch_controller", nullptr, "input/thumbstick", "input/trigger/value",
     "input/squeeze/value", "input/x/click", "input/a/click", "input/y/click", "input/b/click", "input/thumbstick/click",
     "input/menu/click", nullptr, "oculus_touch"},
    {"/interaction_profiles/facebook/touch_controller_pro", XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME, "input/thumbstick",
     "input/trigger/value", "input/squeeze/value", "input/x/click", "input/a/click", "input/y/click", "input/b/click",
     "input/thumbstick/click", "input/menu/click", nullptr, "oculus_touch"},
    {"/interaction_profiles/meta/touch_controller_plus", XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME, "input/thumbstick",
     "input/trigger/value", "input/squeeze/value", "input/x/click", "input/a/click", "input/y/click", "input/b/click",
     "input/thumbstick/click", "input/menu/click", nullptr, "oculus_touch"},
    {"/interaction_profiles/valve/index_controller", nullptr, "input/thumbstick", "input/trigger/value",
     "input/squeeze/value", "input/a/click", "input/a/click", "input/b/click", "input/b/click", "input/thumbstick/click",
     "input/trackpad/force", nullptr, "knuckles"},
    {"/interaction_profiles/htc/vive_controller", nullptr, "input/trackpad", "input/trigger/value", "input/squeeze/click",
     nullptr, "input/menu/click", nullptr, nullptr, "input/trackpad/click", "input/menu/click", nullptr, "vive_controller"},
    {"/interaction_profiles/microsoft/motion_controller", nullptr, "input/thumbstick", "input/trigger/value",
     "input/squeeze/click", "input/trackpad/click", "input/trackpad/click", nullptr, "input/menu/click",
     "input/thumbstick/click", "input/menu/click", nullptr, "holographic_controller"},
    {"/interaction_profiles/hp/mixed_reality_controller", XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME,
     "input/thumbstick", "input/trigger/value", "input/squeeze/value", "input/x/click", "input/a/click", "input/y/click",
     "input/b/click", "input/thumbstick/click", "input/menu/click", nullptr, "hpmotioncontroller"},
    {"/interaction_profiles/htc/vive_cosmos_controller", XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME,
     "input/thumbstick", "input/trigger/value", "input/squeeze/click", "input/x/click", "input/a/click", "input/y/click",
     "input/b/click", "input/thumbstick/click", "input/menu/click", nullptr, "vive_cosmos_controller"},
    {"/interaction_profiles/htc/vive_focus3_controller", XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME,
     "input/thumbstick", "input/trigger/value", "input/squeeze/value", "input/x/click", "input/a/click", "input/y/click",
     "input/b/click", "input/thumbstick/click", "input/menu/click", nullptr, "vive_focus3"},
    {"/interaction_profiles/bytedance/pico_neo3_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME,
     "input/thumbstick", "input/trigger/value", "input/squeeze/value", "input/x/click", "input/a/click", "input/y/click",
     "input/b/click", "input/thumbstick/click", "input/menu/click", nullptr, "pico_controller"},
    {"/interaction_profiles/bytedance/pico4_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, "input/thumbstick",
     "input/trigger/value", "input/squeeze/value", "input/x/click", "input/a/click", "input/y/click", "input/b/click",
     "input/thumbstick/click", "input/menu/click", nullptr, "pico_controller"},
    {"/interaction_profiles/khr/simple_controller", nullptr, nullptr, "input/select/click", nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, "input/menu/click", nullptr, "generic"},
};

std::vector<std::string> g_enabledExts;
bool ExtEnabled(const char* name) {
    for (const auto& e : g_enabledExts)
        if (e == name) return true;
    return false;
}

XrPath Path(const std::string& s) {
    XrPath p = XR_NULL_PATH;
    fn.xrStringToPath(g.instance, s.c_str(), &p);
    return p;
}

bool CreateActions() {
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "specopsvr");
    strcpy_s(si.localizedActionSetName, "Spec Ops: The Line VR");
    if (!Check(fn.xrCreateActionSet(g.instance, &si, &g.actionSet), "xrCreateActionSet")) return false;
    g.handPath[0] = Path("/user/hand/left");
    g.handPath[1] = Path("/user/hand/right");
    static const XrActionType kTypes[kActCount] = {
        XR_ACTION_TYPE_VECTOR2F_INPUT, XR_ACTION_TYPE_FLOAT_INPUT,   XR_ACTION_TYPE_FLOAT_INPUT,
        XR_ACTION_TYPE_BOOLEAN_INPUT,  XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT,
        XR_ACTION_TYPE_BOOLEAN_INPUT,  XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT,
        XR_ACTION_TYPE_VIBRATION_OUTPUT, XR_ACTION_TYPE_POSE_INPUT};
    for (int h = 0; h < 2; ++h)
        for (int a = 0; a < kActCount; ++a) {
            XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
            ci.actionType = kTypes[a];
            std::string n = std::string(h ? "right_" : "left_") + kActNames[a];
            strcpy_s(ci.actionName, n.c_str());
            std::string loc = std::string(h ? "Right " : "Left ") + kActNames[a];
            strcpy_s(ci.localizedActionName, loc.c_str());
            ci.countSubactionPaths = 1;
            ci.subactionPaths = &g.handPath[h];
            if (!Check(fn.xrCreateAction(g.actionSet, &ci, &g.action[h][a]), "xrCreateAction")) return false;
        }
    int profiles = 0;
    for (const Profile& p : kProfiles) {
        if (p.ext && !ExtEnabled(p.ext)) continue;
        std::vector<XrActionSuggestedBinding> b;
        auto add = [&](int h, int act, const char* rel) {
            if (!rel) return;
            b.push_back({g.action[h][act], Path(std::string(h ? "/user/hand/right/" : "/user/hand/left/") + rel)});
        };
        for (int h = 0; h < 2; ++h) {
            add(h, kActStick, p.stick);
            add(h, kActTrigger, p.trigger);
            add(h, kActGrip, p.grip);
            add(h, kActPrimary, h ? p.primaryR : p.primaryL);
            add(h, kActSecondary, h ? p.secondaryR : p.secondaryL);
            add(h, kActStickClick, p.stickClick);
            add(h, kActMenu, h ? p.menuR : p.menuL);
            add(h, kActHaptic, "output/haptic");
            add(h, kActPose, "input/grip/pose");
        }
        XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = Path(p.path);
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        sb.suggestedBindings = b.data();
        XrResult r = fn.xrSuggestInteractionProfileBindings(g.instance, &sb);
        if (XR_SUCCEEDED(r)) ++profiles;
        else vr::log::Line(std::string("OPENXR: bindings for ") + p.path + " refused: " + ResultName(r));
    }
    vr::log::Line("OPENXR: bindings suggested for " + std::to_string(profiles) + " controller profiles.");
    return true;
}

const Profile* ProfileFor(const std::string& path) {
    for (const Profile& p : kProfiles)
        if (path == p.path) return &p;
    return nullptr;
}

void RefreshProfiles() {
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
        std::string prof;
        if (XR_SUCCEEDED(fn.xrGetCurrentInteractionProfile(g.session, g.handPath[h], &st)) && st.interactionProfile) {
            char b[XR_MAX_PATH_LENGTH] = {};
            uint32_t n = 0;
            fn.xrPathToString(g.instance, st.interactionProfile, sizeof(b), &n, b);
            prof = b;
        }
        HandState& hs = g.hand[h];
        if (prof != hs.profile) {
            hs.profile = prof;
            const Profile* p = ProfileFor(prof);
            hs.controllerType = p ? p->type : (prof.empty() ? "" : FileName(prof));
            vr::log::Line(std::string("OPENXR: ") + (h ? "right" : "left") + " hand profile " +
                          (prof.empty() ? "none" : prof) + ".");
        }
        hs.connected = !prof.empty();
    }
}

// Swapchain: formato scelto all'avvio; w/h della texture del mod.
int64_t PickFormat(const std::vector<int64_t>& avail, DXGI_FORMAT family) {
    DXGI_FORMAT want[2];
    if (family == DXGI_FORMAT_B8G8R8A8_UNORM || family == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        family == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        want[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        want[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
    } else {
        want[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        want[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    for (DXGI_FORMAT f : want)
        for (int64_t a : avail)
            if (a == f) return a;
    return 0;
}
std::vector<int64_t> g_formats;

void DestroySwap(Swap& s) {
    for (auto* t : s.images)
        if (t) t->Release();
    s.images.clear();
    if (s.sc) fn.xrDestroySwapchain(s.sc);
    s = Swap();
}

bool EnsureSwap(Swap& s, uint32_t w, uint32_t h, int64_t fmt) {
    if (s.sc && s.w == w && s.h == h && s.fmt == fmt) return true;
    DestroySwap(s);
    if (!w || !h || !fmt) return false;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!Check(fn.xrCreateSwapchain(g.session, &ci, &s.sc), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    fn.xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!Check(fn.xrEnumerateSwapchainImages(s.sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())),
               "xrEnumerateSwapchainImages")) {
        DestroySwap(s);
        return false;
    }
    for (auto& i : imgs) {
        i.texture->AddRef();
        s.images.push_back(i.texture);
    }
    s.w = w;
    s.h = h;
    s.fmt = fmt;
    return true;
}

// Scrive un'immagine nella swapchain: copia da texture o UpdateSubresource dai pixel.
bool WriteSwap(Swap& s, ID3D11Texture2D* src, const uint8_t* pixels, uint32_t pitch) {
    if (!s.sc) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(fn.xrAcquireSwapchainImage(s.sc, &ai, &idx), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;  // 100 ms
    XrResult wr = fn.xrWaitSwapchainImage(s.sc, &wi);
    if (XR_SUCCEEDED(wr) && idx < s.images.size()) {
        if (src) g.ctx->CopySubresourceRegion(s.images[idx], 0, 0, 0, 0, src, 0, nullptr);
        else if (pixels) g.ctx->UpdateSubresource(s.images[idx], 0, nullptr, pixels, pitch, 0);
    }
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_SUCCEEDED(wr)) Check(fn.xrReleaseSwapchainImage(s.sc, &ri), "xrReleaseSwapchainImage");
    else Check(wr, "xrWaitSwapchainImage");
    if (XR_SUCCEEDED(wr)) s.released = true;
    return XR_SUCCEEDED(wr);
}

// --- 4. istanza, sessione, frame loop -----------------------------------------------

bool CreateD3D11(const LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* found = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!found && d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            found = adapter;
            g.adapterIndex = static_cast<int32_t>(i);
        } else {
            adapter->Release();
        }
    }
    if (!found) {
        factory->Release();
        vr::log::Line("OPENXR: the runtime's GPU (LUID) was not found among the DXGI adapters.");
        return false;
    }
    DXGI_ADAPTER_DESC1 d;
    found->GetDesc1(&d);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &g.device, &level, &g.ctx);
    found->Release();
    factory->Release();
    if (FAILED(hr)) {
        vr::log::Line("OPENXR: D3D11CreateDevice failed (" + std::to_string(static_cast<unsigned long>(hr)) + ").");
        return false;
    }
    // Il contesto immediato viene usato dal thread di uscita del mod e dal
    // keepalive del backend: protezione multithread di D3D11.
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(g.ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt))) && mt) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    vr::log::Line(std::string("OPENXR: D3D11 device on ") + name + " (adapter " + std::to_string(g.adapterIndex) + ").");
    return true;
}

bool CreateInstanceAndSystem(std::string* error) {
    if (g.instance) return true;
    Load(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn.xrEnumerateInstanceExtensionProperties);
    Load(XR_NULL_HANDLE, "xrCreateInstance", &fn.xrCreateInstance);
    if (!fn.xrEnumerateInstanceExtensionProperties || !fn.xrCreateInstance) {
        if (error) *error = "runtime without xrCreateInstance";
        return false;
    }
    uint32_t n = 0;
    fn.xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    fn.xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    auto has = [&](const char* name) {
        for (const auto& p : props)
            if (strcmp(p.extensionName, name) == 0) return true;
        return false;
    };
    if (!has(XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
        if (error) *error = "runtime without XR_KHR_D3D11_enable";
        return false;
    }
    const char* wanted[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
                            XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
                            XR_EXT_USER_PRESENCE_EXTENSION_NAME, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME,
                            XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME,
                            XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME, XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME,
                            XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME};
    g_enabledExts.clear();
    std::vector<const char*> enable;
    std::string list;
    for (const char* w : wanted)
        if (has(w)) {
            g_enabledExts.push_back(w);
            list += std::string(" ") + w;
        }
    for (const auto& e : g_enabledExts) enable.push_back(e.c_str());
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "SpecOpsTheLineVR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "UE3 d3d9 proxy");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.enabledExtensionNames = enable.data();
    XrResult r = fn.xrCreateInstance(&ci, &g.instance);
    if (XR_FAILED(r)) {
        if (error) *error = "xrCreateInstance failed (" + std::to_string(r) + ")";
        g.instance = XR_NULL_HANDLE;
        return false;
    }
#define X(name) Load(g.instance, #name, &fn.name);
    XR_FUNCS(X)
    XR_EXT_FUNCS(X)
#undef X
    g.extPerfCounter = ExtEnabled(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) &&
                       fn.xrConvertWin32PerformanceCounterToTimeKHR;
    g.extVisMask = ExtEnabled(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME) && fn.xrGetVisibilityMaskKHR;
    g.extRefreshRate = ExtEnabled(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) && fn.xrGetDisplayRefreshRateFB;
    g.extPresence = ExtEnabled(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    fn.xrGetInstanceProperties(g.instance, &ip);
    g.runtimeName = ip.runtimeName;
    std::snprintf(g.runtimeVersion, sizeof(g.runtimeVersion), "%s %u.%u.%u", ip.runtimeName,
                  XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                  XR_VERSION_PATCH(ip.runtimeVersion));
    vr::log::Line(std::string("OPENXR: instance on ") + g.runtimeVersion + "; extensions:" + list);
    return true;
}

void DestroyAll();

bool CreateSession(std::string* error) {
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = fn.xrGetSystem(g.instance, &gi, &g.system);
    if (XR_FAILED(r)) {
        if (error) *error = r == XR_ERROR_FORM_FACTOR_UNAVAILABLE ? "headset not connected (form factor unavailable)"
                                                                  : "xrGetSystem failed (" + ResultName(r) + ")";
        return false;
    }
    XrSystemUserPresencePropertiesEXT pp = {XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT};
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    if (g.extPresence) sp.next = &pp;
    fn.xrGetSystemProperties(g.instance, g.system, &sp);
    g.systemName = sp.systemName;
    if (g.extPresence && !pp.supportsUserPresence) g.extPresence = false;
    uint32_t n = 0;
    fn.xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    if (n < 2) {
        if (error) *error = "no stereo view configuration";
        return false;
    }
    XrViewConfigurationView vc[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    fn.xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vc);
    g.viewCfg[0] = vc[0];
    g.viewCfg[1] = vc[1];
    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!fn.xrGetD3D11GraphicsRequirementsKHR ||
        !Check(fn.xrGetD3D11GraphicsRequirementsKHR(g.instance, g.system, &req), "xrGetD3D11GraphicsRequirementsKHR")) {
        if (error) *error = "D3D11 requirements unavailable";
        return false;
    }
    if (!g.device && !CreateD3D11(req.adapterLuid)) {
        if (error) *error = "D3D11 device for the headset GPU";
        return false;
    }
    XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g.device;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g.system;
    r = fn.xrCreateSession(g.instance, &sci, &g.session);
    if (XR_FAILED(r)) {
        if (error) *error = "xrCreateSession failed (" + ResultName(r) + ")";
        g.session = XR_NULL_HANDLE;
        return false;
    }
    // Spazi: STAGE (pavimento, origine della stanza) come l'universo
    // "standing" di OpenVR; LOCAL con -1,6 m se manca.
    uint32_t ns = 0;
    fn.xrEnumerateReferenceSpaces(g.session, 0, &ns, nullptr);
    std::vector<XrReferenceSpaceType> spaces(ns);
    fn.xrEnumerateReferenceSpaces(g.session, ns, &ns, spaces.data());
    bool hasStage = std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    rs.referenceSpaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!hasStage) rs.poseInReferenceSpace.position.y = -1.6f;
    g.stageType = rs.referenceSpaceType;
    if (!Check(fn.xrCreateReferenceSpace(g.session, &rs, &g.stage), "xrCreateReferenceSpace(stage)")) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rs.poseInReferenceSpace.position.y = 0.0f;
    if (!Check(fn.xrCreateReferenceSpace(g.session, &rs, &g.view), "xrCreateReferenceSpace(view)")) return false;
    // Formati delle swapchain.
    uint32_t nf = 0;
    fn.xrEnumerateSwapchainFormats(g.session, 0, &nf, nullptr);
    g_formats.assign(nf, 0);
    fn.xrEnumerateSwapchainFormats(g.session, nf, &nf, g_formats.data());
    g.eyeFormat = PickFormat(g_formats, DXGI_FORMAT_B8G8R8A8_UNORM);
    g.overlayFormat = PickFormat(g_formats, DXGI_FORMAT_R8G8B8A8_UNORM);
    std::string fl;
    for (int64_t f : g_formats) fl += " " + std::to_string(f);
    vr::log::Line("OPENXR: swapchain formats:" + fl + "; eyes " + std::to_string(g.eyeFormat) + ", overlays " +
                  std::to_string(g.overlayFormat) + (hasStage ? "; stage space" : "; LOCAL space (no stage)"));
    if (!g.eyeFormat) vr::log::Line("OPENXR: ERROR no BGRA swapchain format: the eye images cannot be sent.");
    if (!CreateActions()) return false;
    XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &g.actionSet;
    if (!Check(fn.xrAttachSessionActionSets(g.session, &at), "xrAttachSessionActionSets")) return false;
    if (!CreateHandSpaces()) return false;
    if (g.extRefreshRate) {
        float hz = 0.0f;
        if (XR_SUCCEEDED(fn.xrGetDisplayRefreshRateFB(g.session, &hz)) && hz > 0.0f) g.hz = hz;
    }
    char b[200];
    std::snprintf(b, sizeof(b), "OPENXR: session created: %s, %ux%u per eye recommended, %.0f Hz.", g.systemName.c_str(),
                  g.viewCfg[0].recommendedImageRectWidth, g.viewCfg[0].recommendedImageRectHeight, g.hz.load());
    vr::log::Line(b);
    return true;
}

void PushEvent(uint32_t type, vr::TrackedDeviceIndex_t dev = kHmd) {
    vr::VREvent_t ev = {};
    ev.eventType = type;
    ev.trackedDeviceIndex = dev;
    g.events.push_back(ev);
}

// Sincronizzazione delle azioni (al massimo una ogni 2 ms) e lettura degli stati.
void SyncActions(bool force) {
    if (!g.running) return;
    double now = QpcMs();
    if (!force && now - g.lastSyncMs < 2.0) return;
    g.lastSyncMs = now;
    XrActiveActionSet as = {g.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    XrResult r = fn.xrSyncActions(g.session, &si);
    bool ok = r == XR_SUCCESS;  // XR_SESSION_NOT_FOCUSED: azioni ferme
    if (XR_FAILED(r)) Check(r, "xrSyncActions");
    ++g.packet;
    for (int h = 0; h < 2; ++h) {
        HandState& hs = g.hand[h];
        if (!ok) {
            hs.active = false;
            hs.poseActive = false;
            continue;
        }
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = g.handPath[h];
        XrActionStatePose ps = {XR_TYPE_ACTION_STATE_POSE};
        gi.action = g.action[h][kActPose];
        fn.xrGetActionStatePose(g.session, &gi, &ps);
        hs.poseActive = ps.isActive;
        XrActionStateVector2f v2 = {XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = g.action[h][kActStick];
        fn.xrGetActionStateVector2f(g.session, &gi, &v2);
        hs.stick = v2.isActive ? v2.currentState : XrVector2f{0, 0};
        bool any = v2.isActive || ps.isActive;
        auto flt = [&](int a, float* out) {
            XrActionStateFloat f = {XR_TYPE_ACTION_STATE_FLOAT};
            gi.action = g.action[h][a];
            fn.xrGetActionStateFloat(g.session, &gi, &f);
            *out = f.isActive ? f.currentState : 0.0f;
            any = any || f.isActive;
        };
        auto bl = [&](int a, bool* out) {
            XrActionStateBoolean b = {XR_TYPE_ACTION_STATE_BOOLEAN};
            gi.action = g.action[h][a];
            fn.xrGetActionStateBoolean(g.session, &gi, &b);
            *out = b.isActive && b.currentState;
            any = any || b.isActive;
        };
        flt(kActTrigger, &hs.trigger);
        flt(kActGrip, &hs.grip);
        bl(kActPrimary, &hs.primary);
        bl(kActSecondary, &hs.secondary);
        bl(kActStickClick, &hs.stickClick);
        bl(kActMenu, &hs.menu);
        hs.active = any;
    }
    if (now - g.lastProfileMs > 1000.0) {
        g.lastProfileMs = now;
        RefreshProfiles();
    }
}

bool LocateHand(int h, XrTime t, Mat34* out) {
    if (!g.handSpace[h] || !g.running) return false;
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(fn.xrLocateSpace(g.handSpace[h], g.stage, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    *out = FromPose(loc.pose);
    return true;
}

bool LocateHmd(XrTime t, Mat34* out) {
    if (!g.view || !g.running) return false;
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(fn.xrLocateSpace(g.view, g.stage, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    *out = FromPose(loc.pose);
    return true;
}

// Posa dell'overlay nello spazio stage (per il puntatore).
bool OverlayInStage(const OverlayItem& o, XrTime t, Mat34* out) {
    if (o.attach == Attach::Absolute) {
        *out = o.xform;
        return true;
    }
    Mat34 base;
    if (o.attach == Attach::Hmd ? !LocateHmd(t, &base) : !LocateHand(o.hand, t, &base)) return false;
    *out = Mul(base, o.xform);
    return true;
}

float OverlayHeight(const OverlayItem& o) {
    float tw = o.tex ? static_cast<float>(o.swap.w) : static_cast<float>(o.pixW);
    float th = o.tex ? static_cast<float>(o.swap.h) : static_cast<float>(o.pixH);
    float bw = (o.bounds.uMax - o.bounds.uMin), bh = (o.bounds.vMax - o.bounds.vMin);
    if (o.flags & vr::VROverlayFlags_SideBySide_Parallel) bw *= 0.5f;
    if (tw <= 0 || th <= 0 || bw <= 0 || bh <= 0) return o.width;
    return o.width * (th * bh) / (tw * bw);
}

// Raggio (origine, direzione) contro il quadrato dell'overlay posato con m: punto, uv (GL: v=0 in basso), distanza.
bool Intersect(const OverlayItem& o, const Mat34& m, const float* org, const float* dir, float* point, float* u, float* v,
               float* dist) {
    float n[3] = {m.m[0][2], m.m[1][2], m.m[2][2]};
    float c[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
    float dn = dir[0] * n[0] + dir[1] * n[1] + dir[2] * n[2];
    if (std::fabs(dn) < 1e-5f) return false;
    float t = ((c[0] - org[0]) * n[0] + (c[1] - org[1]) * n[1] + (c[2] - org[2]) * n[2]) / dn;
    if (t <= 0.0f) return false;
    for (int i = 0; i < 3; ++i) point[i] = org[i] + dir[i] * t;
    float d[3] = {point[0] - c[0], point[1] - c[1], point[2] - c[2]};
    float lx = d[0] * m.m[0][0] + d[1] * m.m[1][0] + d[2] * m.m[2][0];
    float ly = d[0] * m.m[0][1] + d[1] * m.m[1][1] + d[2] * m.m[2][1];
    float w = o.width, h = OverlayHeight(o);
    if (std::fabs(lx) > w * 0.5f || std::fabs(ly) > h * 0.5f) return false;
    *u = lx / w + 0.5f;
    *v = ly / h + 0.5f;
    *dist = t;
    return true;
}

OverlayItem* EnsureCursor() {
    if (g.cursor && g.overlays.count(g.cursor)) return &g.overlays[g.cursor];
    OverlayItem o;
    o.handle = g.nextOverlay++;
    o.key = "specopsvr.xr.cursor";
    o.internal = true;
    o.width = 0.02f;
    o.sortOrder = 9999;
    o.pixW = o.pixH = 32;
    o.pixels.assign(32 * 32 * 4, 0);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            float dx = x - 15.5f, dy = y - 15.5f, d = std::sqrt(dx * dx + dy * dy);
            uint8_t* p = &o.pixels[(y * 32 + x) * 4];
            if (d < 10.0f) { p[0] = 0x30; p[1] = 0xA0; p[2] = 0xFF; p[3] = 0xFF; }       // azzurro (il mod usa rosso per il cinema)
            else if (d < 13.0f) { p[0] = p[1] = p[2] = 0xF0; p[3] = 0xFF; }
        }
    o.dirty = true;
    g.cursor = o.handle;
    g.overlays[o.handle] = o;
    return &g.overlays[o.handle];
}

// Puntatore laser sugli overlay interattivi (menu del mod): eventi mouse come
// SteamVR, dal controller che li colpisce (destro per primo).
void UpdatePointer(XrTime t) {
    std::vector<OverlayItem*> targets;
    for (auto& kv : g.overlays)
        if (kv.second.visible && !kv.second.internal && kv.second.input == vr::VROverlayInputMethod_Mouse &&
            (kv.second.flags & vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible))
            targets.push_back(&kv.second);
    OverlayItem* cursor = g.cursor ? &g.overlays[g.cursor] : nullptr;
    if (targets.empty()) {
        if (cursor) cursor->visible = false;
        for (auto& kv : g.overlays) kv.second.hovered = false;
        return;
    }
    std::sort(targets.begin(), targets.end(), [](OverlayItem* a, OverlayItem* b) { return a->sortOrder > b->sortOrder; });
    OverlayItem* hit = nullptr;
    float hu = 0, hv = 0, hp[3] = {}, hd = 0;
    Mat34 hitM = {};
    int hitHand = -1;
    for (int h : {1, 0}) {
        Mat34 hm;
        if (!g.hand[h].poseActive || !LocateHand(h, t, &hm)) continue;
        float org[3] = {hm.m[0][3], hm.m[1][3], hm.m[2][3]}, dir[3] = {-hm.m[0][2], -hm.m[1][2], -hm.m[2][2]};
        for (OverlayItem* o : targets) {
            Mat34 om;
            float u, v, d, p[3];
            if (OverlayInStage(*o, t, &om) && Intersect(*o, om, org, dir, p, &u, &v, &d)) {
                hit = o;
                hu = u; hv = v; hd = d;
                std::memcpy(hp, p, sizeof(hp));
                hitM = om;
                hitHand = h;
                break;
            }
        }
        if (hit) break;
    }
    for (OverlayItem* o : targets) {
        bool was = o->hovered;
        o->hovered = (o == hit);
        if (was && !o->hovered) o->lastU = o->lastV = -1.0f;
    }
    if (!hit) {
        if (cursor) cursor->visible = false;
        for (int h = 0; h < 2; ++h) g.triggerWas[h] = g.hand[h].trigger > 0.5f;
        return;
    }
    float mx = hu * hit->mouseScale.v[0], my = hv * hit->mouseScale.v[1];
    auto push = [&](uint32_t type, float dy) {
        vr::VREvent_t ev = {};
        ev.eventType = type;
        ev.trackedDeviceIndex = hitHand == 0 ? kLeft : kRight;
        if (type == vr::VREvent_ScrollDiscrete) {
            ev.data.scroll.ydelta = dy;
            ev.data.scroll.viewportscale = 1.0f;
        } else {
            ev.data.mouse.x = mx;
            ev.data.mouse.y = my;
            ev.data.mouse.button = vr::VRMouseButton_Left;
        }
        if (hit->events.size() < 64) hit->events.push_back(ev);
    };
    if (std::fabs(mx - hit->lastU) > 0.5f || std::fabs(my - hit->lastV) > 0.5f) {
        hit->lastU = mx;
        hit->lastV = my;
        push(vr::VREvent_MouseMove, 0.0f);
    }
    bool trig = g.hand[hitHand].trigger > 0.5f;
    if (trig != g.triggerWas[hitHand]) push(trig ? vr::VREvent_MouseButtonDown : vr::VREvent_MouseButtonUp, 0.0f);
    for (int h = 0; h < 2; ++h) g.triggerWas[h] = g.hand[h].trigger > 0.5f;
    if (hit->flags & vr::VROverlayFlags_SendVRDiscreteScrollEvents) {
        float sy = g.hand[hitHand].stick.y;
        int dir = sy > 0.6f ? 1 : (sy < -0.6f ? -1 : 0);
        double now = QpcMs();
        if (dir != 0 && (dir != g.scrollLatch[hitHand] || now - g.scrollRepeatMs[hitHand] > 350.0)) {
            g.scrollLatch[hitHand] = dir;
            g.scrollRepeatMs[hitHand] = now;
            push(vr::VREvent_ScrollDiscrete, static_cast<float>(dir));
        } else if (dir == 0) {
            g.scrollLatch[hitHand] = 0;
        }
    }
    // Puntino sul punto colpito, 1 cm davanti al pannello.
    OverlayItem* c = EnsureCursor();
    c->xform = hitM;
    for (int i = 0; i < 3; ++i) c->xform.m[i][3] = hp[i] + hitM.m[i][2] * 0.01f;
    c->attach = Attach::Absolute;
    c->visible = true;
    (void)hd;
}

float FadeAlphaNow() {
    if (g.fadeDurMs <= 0.0) return g.fadeTo;
    double k = (QpcMs() - g.fadeStartMs) / g.fadeDurMs;
    if (k >= 1.0) return g.fadeTo;
    if (k <= 0.0) return g.fadeFrom;
    return g.fadeFrom + (g.fadeTo - g.fadeFrom) * static_cast<float>(k);
}

// Carica nelle swapchain i contenuti cambiati (thread che tiene il frame).
void UploadOverlays() {
    for (auto& kv : g.overlays) {
        OverlayItem& o = kv.second;
        if (!o.dirty) continue;
        if (o.tex) {
            D3D11_TEXTURE2D_DESC d;
            o.tex->GetDesc(&d);
            int64_t fmt = PickFormat(g_formats, d.Format);
            if (!fmt) fmt = g.eyeFormat;
            if (EnsureSwap(o.swap, d.Width, d.Height, fmt)) WriteSwap(o.swap, o.tex, nullptr, 0);
        } else if (o.pixW && o.pixH && !o.pixels.empty()) {
            if (EnsureSwap(o.swap, o.pixW, o.pixH, g.overlayFormat)) WriteSwap(o.swap, nullptr, o.pixels.data(), o.pixW * 4);
        }
        o.dirty = false;
    }
    float a = FadeAlphaNow();
    g.fadeAlpha = a;
    if (a > 0.002f && std::fabs(a - g.fadeUploaded) > 1.0f / 255.0f) {
        if (EnsureSwap(g.fadeSwap, 2, 2, g.overlayFormat)) {
            uint8_t px[16] = {};
            for (int i = 0; i < 4; ++i) px[i * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
            if (WriteSwap(g.fadeSwap, nullptr, px, 8)) g.fadeUploaded = a;
        }
    }
}

// Con g.frameMutex preso: xrWaitFrame + xrBeginFrame e viste del frame.
bool WaitFrameLocked() {
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    XrResult r = fn.xrWaitFrame(g.session, &wi, &fs);
    if (XR_FAILED(r)) {
        Check(r, "xrWaitFrame");
        return false;
    }
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    r = fn.xrBeginFrame(g.session, &bi);
    if (XR_FAILED(r) && r != XR_FRAME_DISCARDED) {
        Check(r, "xrBeginFrame");
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.frameState = fs;
    g.lastPredicted = fs.predictedDisplayTime;
    g.lastPeriod = fs.predictedDisplayPeriod;
    g.frameBegun = true;
    g.frameOwner = GetCurrentThreadId();
    g.frameBeginMs = QpcMs();
    if (!g.extRefreshRate && fs.predictedDisplayPeriod > 0) g.hz = static_cast<float>(1e9 / fs.predictedDisplayPeriod);
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = g.stage;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrResult lv = fn.xrLocateViews(g.session, &li, &vs, 2, &n, views);
    if (XR_SUCCEEDED(lv) && n == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        g.frameViews[0] = views[0];
        g.frameViews[1] = views[1];
        g.frameViewsValid = true;
    } else {
        static double lastLog = 0.0;
        if (QpcMs() - lastLog > 5000.0) {
            lastLog = QpcMs();
            vr::log::Line("OPENXR: xrLocateViews(stage) " + ResultName(lv) + ", views " + std::to_string(n) + ", flags " +
                          std::to_string(static_cast<unsigned>(vs.viewStateFlags)) + " (no valid views yet).");
        }
    }
    li.space = g.view;
    if (XR_SUCCEEDED(fn.xrLocateViews(g.session, &li, &vs, 2, &n, views)) && n == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        bool first = !g.eyeInHeadValid;
        g.eyeInHead[0] = views[0];
        g.eyeInHead[1] = views[1];
        g.eyeInHeadValid = true;
        if (first) PushEvent(vr::VREvent_IpdChanged);  // proiezione da ricalcolare nel mod
    }
    ++g.frames;
    return true;
}

// Con il frame iniziato: compone i layer e chiude il frame; rilascia g.frameMutex.
void EndFrameLocked(bool newEyes) {
    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerQuad> quads;
    std::vector<OverlayItem*> ovs;
    XrTime display = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g.mutex);
        display = g.frameState.predictedDisplayTime;
        SyncActions(false);
        if (newEyes && g.submit[0].set && g.submit[1].set && g.eyeFormat) {
            bool ok = true;
            for (int e = 0; e < 2 && ok; ++e) {
                State::EyeSubmit& s = g.submit[e];
                D3D11_TEXTURE2D_DESC d;
                s.tex->GetDesc(&d);
                ok = EnsureSwap(g.eye[e], d.Width, d.Height, g.eyeFormat) && WriteSwap(g.eye[e], s.tex, nullptr, 0);
                if (!ok) break;
                XrCompositionLayerProjectionView& pv = g.projViews[e];
                pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv.subImage.swapchain = g.eye[e].sc;
                pv.subImage.imageRect.offset = {static_cast<int32_t>(s.bounds.uMin * d.Width),
                                                static_cast<int32_t>(s.bounds.vMin * d.Height)};
                pv.subImage.imageRect.extent = {static_cast<int32_t>((s.bounds.uMax - s.bounds.uMin) * d.Width),
                                                static_cast<int32_t>((s.bounds.vMax - s.bounds.vMin) * d.Height)};
                if (pv.subImage.imageRect.extent.width <= 0 || pv.subImage.imageRect.extent.height <= 0)
                    pv.subImage.imageRect.extent = {static_cast<int32_t>(d.Width), static_cast<int32_t>(d.Height)};
                if (s.hasPose && g.eyeInHeadValid) pv.pose = ToPose(Mul(s.pose, FromPose(g.eyeInHead[e].pose)));
                else pv.pose = g.frameViews[e].pose;
                pv.fov = g.frameViewsValid ? g.frameViews[e].fov : XrFovf{-0.785f, 0.785f, 0.785f, -0.785f};
            }
            if (ok) {
                g.projValid = true;
                g.projMs = QpcMs();
                ++g.eyeFrames;
            }
        }
        g.submit[0].set = g.submit[1].set = false;
        UpdatePointer(display);
        UploadOverlays();
        if (g.projValid && QpcMs() - g.projMs < 500.0 && g.eye[0].released && g.eye[1].released) {
            proj.space = g.stage;
            proj.viewCount = 2;
            proj.views = g.projViews;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));
        }
        for (auto& kv : g.overlays)
            if (kv.second.visible && kv.second.swap.released && kv.second.alpha > 0.01f) ovs.push_back(&kv.second);
        std::sort(ovs.begin(), ovs.end(), [](OverlayItem* a, OverlayItem* b) { return a->sortOrder < b->sortOrder; });
        quads.reserve(ovs.size() * 2 + 1);
        for (OverlayItem* o : ovs) {
            bool sbs = (o->flags & vr::VROverlayFlags_SideBySide_Parallel) != 0;
            for (int part = 0; part < (sbs ? 2 : 1); ++part) {
                XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.layerFlags = (o->flags & vr::VROverlayFlags_IgnoreTextureAlpha)
                                   ? 0
                                   : (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                      XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
                q.space = o->attach == Attach::Absolute ? g.stage : (o->attach == Attach::Hmd ? g.view : g.handSpace[o->hand]);
                q.eyeVisibility = !sbs ? XR_EYE_VISIBILITY_BOTH : (part == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);
                q.subImage.swapchain = o->swap.sc;
                float u0 = o->bounds.uMin, u1 = o->bounds.uMax;
                if (sbs) {
                    float half = (u1 - u0) * 0.5f;
                    u0 = u0 + half * part;
                    u1 = u0 + half;
                }
                q.subImage.imageRect.offset = {static_cast<int32_t>(u0 * o->swap.w), static_cast<int32_t>(o->bounds.vMin * o->swap.h)};
                q.subImage.imageRect.extent = {static_cast<int32_t>((u1 - u0) * o->swap.w),
                                               static_cast<int32_t>((o->bounds.vMax - o->bounds.vMin) * o->swap.h)};
                q.pose = ToPose(o->xform);
                q.size = {o->width, OverlayHeight(*o)};
                quads.push_back(q);
            }
        }
        if (g.fadeAlpha > 0.002f && g.fadeSwap.released) {
            XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            q.space = g.view;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = g.fadeSwap.sc;
            q.subImage.imageRect.extent = {2, 2};
            q.pose = {{0, 0, 0, 1}, {0, 0, -0.5f}};
            q.size = {8.0f, 8.0f};
            quads.push_back(q);
        }
        for (auto& q : quads) layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&q));
    }
    // Sotto g.mutex anche xrEndFrame: nessun overlay (e la sua swapchain) puo'
    // sparire mentre il layer lo cita.
    std::unique_lock<std::recursive_mutex> endLock(g.mutex);
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = display;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    XrResult r = fn.xrEndFrame(g.session, &ei);
    if (XR_FAILED(r)) {
        static double lastLog = 0.0;
        if (QpcMs() - lastLog > 5000.0) {
            lastLog = QpcMs();
            vr::log::Line("OPENXR: xrEndFrame failed: " + ResultName(r) + " (" + std::to_string(layers.size()) + " layers).");
        }
        if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST) g.dead = true;
    }
    {
        g.frameBegun = false;
        g.frameOwner = 0;
        g.lastEndMs = QpcMs();
        if (g.frames == 1 || g.frames % 600 == 0) {
            char b[240];
            std::snprintf(b, sizeof(b), "OPENXR: frame %u: %u with eyes, %u keepalive, %u layers, state %d, hands %c%c, %.0f Hz.",
                          g.frames, g.eyeFrames, g.keepalives, static_cast<unsigned>(layers.size()), static_cast<int>(g.state),
                          g.hand[0].active ? 'L' : '-', g.hand[1].active ? 'R' : '-', g.hz.load());
            vr::log::Line(b);
        }
    }
    endLock.unlock();
    g.frameMutex.unlock();
}

// Il runtime vuole un frame a ogni vsync anche quando il gioco non ne manda
// (menu del mod, finestra senza focus, avvio): ogni 100 ms senza frame il
// backend ne chiude uno con gli ultimi layer.
void KeepAliveLoop() {
    while (!g.stop) {
        Sleep(5);
        if (!g.running || g.dead) continue;
        {
            std::lock_guard<std::recursive_mutex> lock(g.mutex);
            if (g.frameBegun || QpcMs() - g.lastEndMs < 100.0) continue;
        }
        if (!g.frameMutex.try_lock()) continue;
        if (!WaitFrameLocked()) {
            g.frameMutex.unlock();
            Sleep(50);
            continue;
        }
        ++g.keepalives;
        EndFrameLocked(false);
    }
}

// Dal thread che vuole iniziare un frame (WaitGetPoses / Submit): chiude un
// frame proprio lasciato aperto, poi prende il frame mutex e inizia.
bool BeginFrameForCaller() {
    bool own = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g.mutex);
        own = g.frameBegun && g.frameOwner == GetCurrentThreadId();
    }
    if (own) EndFrameLocked(false);  // frame lasciato aperto (coppia scartata dal mod)
    g.frameMutex.lock();
    if (!WaitFrameLocked()) {
        g.frameMutex.unlock();
        return false;
    }
    return true;
}

bool FrameOwnedByCaller() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    return g.frameBegun && g.frameOwner == GetCurrentThreadId();
}

// --- 5. eventi e stato della sessione -------------------------------------------------

void OnSessionState(XrSessionState s) {
    XrSessionState old = g.state;
    g.state = s;
    switch (s) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (Check(fn.xrBeginSession(g.session, &bi), "xrBeginSession")) {
            g.running = true;
            g.lastEndMs = 0.0;
            vr::log::Line("OPENXR: session running.");
        }
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        g.running = false;
        g.focused = false;
        Check(fn.xrEndSession(g.session), "xrEndSession");
        vr::log::Line("OPENXR: session stopped by the runtime (headset idle or app switched).");
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        g.running = false;
        g.dead = true;
        vr::log::Line("OPENXR: session exiting / lost: the VR session ends, reconnecting in 5 s.");
        break;
    default:
        break;
    }
    bool focusedNow = s == XR_SESSION_STATE_FOCUSED;
    bool wasFocused = old == XR_SESSION_STATE_FOCUSED;
    if (g.running && s >= XR_SESSION_STATE_SYNCHRONIZED && s <= XR_SESSION_STATE_FOCUSED) {
        if (wasFocused && !focusedNow) PushEvent(vr::VREvent_DashboardActivated);
        if (!wasFocused && focusedNow && old >= XR_SESSION_STATE_SYNCHRONIZED) PushEvent(vr::VREvent_DashboardDeactivated);
    }
    g.focused = focusedNow;
    vr::log::Line("OPENXR: session state " + std::to_string(static_cast<int>(s)) + ".");
}

void PumpEvents() {
    if (!g.instance) return;
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (fn.xrPollEvent(g.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            if (e->session == g.session) OnSessionState(e->state);
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            g.dead = true;
            vr::log::Line("OPENXR: instance loss pending.");
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            auto* e = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
            if (e->referenceSpaceType == g.stageType) PushEvent(vr::VREvent_StandingZeroPoseReset);
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            RefreshProfiles();
            break;
        case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT: {
            auto* e = reinterpret_cast<XrEventDataUserPresenceChangedEXT*>(&ev);
            g.presenceKnown = true;
            g.present = e->isUserPresent == XR_TRUE;
            vr::log::Line(g.present ? "OPENXR: headset worn." : "OPENXR: headset taken off.");
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            auto* e = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev);
            g.hz = e->toDisplayRefreshRate;
            vr::VREvent_t pe = {};
            pe.eventType = vr::VREvent_PropertyChanged;
            pe.trackedDeviceIndex = kHmd;
            pe.data.property.prop = vr::Prop_DisplayFrequency_Float;
            g.events.push_back(pe);
            break;
        }
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void DestroyAll() {
    g.stop = true;
    if (g.keepalive.joinable()) g.keepalive.join();
    g.stop = false;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.running = false;
    g.focused = false;
    for (auto& kv : g.overlays) {
        DestroySwap(kv.second.swap);
        if (kv.second.tex) kv.second.tex->Release();
    }
    g.overlays.clear();
    g.cursor = 0;
    DestroySwap(g.eye[0]);
    DestroySwap(g.eye[1]);
    DestroySwap(g.fadeSwap);
    g.fadeUploaded = -1.0f;
    g.projValid = false;
    g.submit[0] = g.submit[1] = State::EyeSubmit();
    for (int h = 0; h < 2; ++h) {
        if (g.handSpace[h]) fn.xrDestroySpace(g.handSpace[h]);
        g.handSpace[h] = XR_NULL_HANDLE;
        g.hand[h] = HandState();
    }
    if (g.stage) fn.xrDestroySpace(g.stage);
    if (g.view) fn.xrDestroySpace(g.view);
    g.stage = g.view = XR_NULL_HANDLE;
    if (g.actionSet) fn.xrDestroyActionSet(g.actionSet);
    g.actionSet = XR_NULL_HANDLE;
    std::memset(g.action, 0, sizeof(g.action));
    if (g.session) fn.xrDestroySession(g.session);
    g.session = XR_NULL_HANDLE;
    if (g.instance) fn.xrDestroyInstance(g.instance);
    g.instance = XR_NULL_HANDLE;
    g.system = XR_NULL_SYSTEM_ID;
    g.state = XR_SESSION_STATE_UNKNOWN;
    g.frameBegun = false;
    g.frameViewsValid = g.eyeInHeadValid = false;
    g.events.clear();
    g.ham[0].clear();
    g.ham[1].clear();
    g.hamTried[0] = g.hamTried[1] = false;
    g.dead = false;
    // Il device D3D11 resta (vr_output lo condivide).
}

}  // namespace

// --- 6. interfacce OpenVR ------------------------------------------------------

SystemShim g_sys;
CompositorShim g_comp;
OverlayShim g_ov;
InputShim g_in;
RenderModelsShim g_rm;
SettingsShim g_set;
ApplicationsShim g_app;

// IVRSystem
void SystemShim::GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (w) *w = g.viewCfg[0].recommendedImageRectWidth;
    if (h) *h = g.viewCfg[0].recommendedImageRectHeight;
}

static void Tangents(int eye, float* l, float* r, float* u, float* d) {
    if (g.frameViewsValid) {
        const XrFovf& f = g.frameViews[eye].fov;
        *l = std::tan(f.angleLeft);
        *r = std::tan(f.angleRight);
        *u = std::tan(f.angleUp);
        *d = std::tan(f.angleDown);
    } else {
        *l = -1.0f; *r = 1.0f; *u = 1.0f; *d = -1.0f;
    }
}

vr::HmdMatrix44_t SystemShim::GetProjectionMatrix(vr::EVREye eye, float zNear, float zFar) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    float l, r, u, d;
    Tangents(eye, &l, &r, &u, &d);
    vr::HmdMatrix44_t m = {};
    m.m[0][0] = 2.0f / (r - l);
    m.m[0][2] = (r + l) / (r - l);
    m.m[1][1] = 2.0f / (u - d);
    m.m[1][2] = (u + d) / (u - d);
    m.m[2][2] = -(zFar + zNear) / (zFar - zNear);
    m.m[2][3] = -2.0f * zFar * zNear / (zFar - zNear);
    m.m[3][2] = -1.0f;
    return m;
}

void SystemShim::GetProjectionRaw(vr::EVREye eye, float* pl, float* pr, float* pt, float* pb) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    float l, r, u, d;
    Tangents(eye, &l, &r, &u, &d);
    if (pl) *pl = l;
    if (pr) *pr = r;
    if (pt) *pt = -u;  // convenzione OpenVR: alto negativo
    if (pb) *pb = -d;
}

vr::HmdMatrix34_t SystemShim::GetEyeToHeadTransform(vr::EVREye eye) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (!g.eyeInHeadValid) {
        Mat34 m = Identity();
        m.m[0][3] = eye == vr::Eye_Left ? -0.032f : 0.032f;
        return m;
    }
    return FromPose(g.eyeInHead[eye].pose);
}

bool SystemShim::GetTimeSinceLastVsync(float* since, uint64_t* counter) {
    float hz = g.hz.load();
    if (hz < 30.0f) return false;
    float frame = 1.0f / hz;
    float photons = SecondsToPhotons();
    if (since) *since = std::fmod(frame - std::fmod(photons, frame), frame);
    if (counter) *counter = g.frames;
    return true;
}

void SystemShim::GetDXGIOutputInfo(int32_t* idx) {
    if (idx) *idx = g.adapterIndex;
}

void SystemShim::GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float predictSec,
                                                 vr::TrackedDevicePose_t* poses, uint32_t count) {
    if (!poses || !count) return;
    std::memset(poses, 0, sizeof(vr::TrackedDevicePose_t) * count);
    for (uint32_t i = 0; i < count; ++i) poses[i].eTrackingResult = vr::TrackingResult_Uninitialized;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (!g.running) return;
    XrTime t = Now() + static_cast<XrTime>(predictSec * 1e9f);
    auto fill = [&](XrSpace space, vr::TrackedDevicePose_t* p) {
        if (!space) return;
        XrSpaceVelocity vel = {XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        loc.next = &vel;
        if (XR_FAILED(fn.xrLocateSpace(space, g.stage, t, &loc))) return;
        bool ok = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                  (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        p->bDeviceIsConnected = true;
        p->bPoseIsValid = ok;
        p->eTrackingResult = ok ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
        if (!ok) return;
        p->mDeviceToAbsoluteTracking = FromPose(loc.pose);
        if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
            p->vVelocity = {vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z};
        if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
            p->vAngularVelocity = {vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z};
    };
    fill(g.view, &poses[kHmd]);
    for (int h = 0; h < 2; ++h) {
        vr::TrackedDeviceIndex_t idx = h == 0 ? kLeft : kRight;
        if (idx >= count) break;
        if (!g.hand[h].poseActive) {
            poses[idx].bDeviceIsConnected = g.hand[h].connected;
            continue;
        }
        fill(g.handSpace[h], &poses[idx]);
    }
}

vr::TrackedDeviceIndex_t SystemShim::GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole role) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    int h = role == vr::TrackedControllerRole_LeftHand ? 0 : (role == vr::TrackedControllerRole_RightHand ? 1 : -1);
    if (h < 0 || !(g.hand[h].connected || g.hand[h].poseActive)) return vr::k_unTrackedDeviceIndexInvalid;
    return h == 0 ? kLeft : kRight;
}

vr::ETrackedControllerRole SystemShim::GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t i) {
    return i == kLeft ? vr::TrackedControllerRole_LeftHand
                      : (i == kRight ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_Invalid);
}

vr::ETrackedDeviceClass SystemShim::GetTrackedDeviceClass(vr::TrackedDeviceIndex_t i) {
    if (i == kHmd) return vr::TrackedDeviceClass_HMD;
    if (i == kLeft || i == kRight) return IsTrackedDeviceConnected(i) ? vr::TrackedDeviceClass_Controller : vr::TrackedDeviceClass_Invalid;
    return vr::TrackedDeviceClass_Invalid;
}

bool SystemShim::IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t i) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (i == kHmd) return g.session != XR_NULL_HANDLE;
    if (i == kLeft || i == kRight) {
        const HandState& h = g.hand[i == kLeft ? 0 : 1];
        return h.connected || h.poseActive;
    }
    return false;
}

bool SystemShim::GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) {
    if (e) *e = vr::TrackedProp_UnknownProperty;
    return false;
}

float SystemShim::GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, vr::ETrackedDeviceProperty prop,
                                                vr::ETrackedPropertyError* e) {
    if (e) *e = vr::TrackedProp_Success;
    if (i == kHmd) {
        if (prop == vr::Prop_DisplayFrequency_Float) return g.hz.load();
        if (prop == vr::Prop_SecondsFromVsyncToPhotons_Float) return 0.0f;
        if (prop == vr::Prop_UserIpdMeters_Float) {
            std::lock_guard<std::recursive_mutex> lock(g.mutex);
            if (g.eyeInHeadValid) return std::fabs(g.eyeInHead[1].pose.position.x - g.eyeInHead[0].pose.position.x);
        }
    }
    if (e) *e = vr::TrackedProp_UnknownProperty;
    return 0.0f;
}

int32_t SystemShim::GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) {
    if (e) *e = vr::TrackedProp_UnknownProperty;
    return 0;
}

static uint32_t CopyString(const std::string& s, char* out, uint32_t size, vr::ETrackedPropertyError* e) {
    if (e) *e = vr::TrackedProp_Success;
    if (out && size) {
        strncpy_s(out, size, s.c_str(), _TRUNCATE);
    }
    return static_cast<uint32_t>(s.size() + 1);
}

uint32_t SystemShim::GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, vr::ETrackedDeviceProperty prop, char* out,
                                                    uint32_t size, vr::ETrackedPropertyError* e) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    std::string rn = g.runtimeName;
    for (auto& c : rn) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (i == kHmd) {
        switch (prop) {
        case vr::Prop_TrackingSystemName_String: {
            // Il mod riconosce "oculus" (ASW di Link) e "virtual" (Virtual Desktop).
            std::string sys = rn.find("oculus") != std::string::npos ? "oculus"
                              : rn.find("virtual") != std::string::npos ? "virtualdesktop" : rn;
            for (auto& c : sys) if (c == ' ') c = '_';
            return CopyString(sys, out, size, e);
        }
        case vr::Prop_ModelNumber_String: return CopyString(g.systemName, out, size, e);
        case vr::Prop_ManufacturerName_String: return CopyString(g.runtimeName, out, size, e);
        case vr::Prop_SerialNumber_String: return CopyString("openxr-hmd", out, size, e);
        case vr::Prop_RenderModelName_String: return CopyString("generic_hmd", out, size, e);
        default: break;
        }
    } else if (i == kLeft || i == kRight) {
        int h = i == kLeft ? 0 : 1;
        switch (prop) {
        case vr::Prop_RenderModelName_String:
            // Le pose arrivano gia' nel riferimento "raw" dei Touch: nessuna correzione nel mod.
            return CopyString(h ? "oculus_quest2_controller_right" : "oculus_quest2_controller_left", out, size, e);
        case vr::Prop_ControllerType_String: return CopyString(g.hand[h].controllerType, out, size, e);
        case vr::Prop_ModelNumber_String: return CopyString(g.hand[h].profile.empty() ? "controller" : FileName(g.hand[h].profile), out, size, e);
        case vr::Prop_SerialNumber_String: return CopyString(h ? "openxr-right" : "openxr-left", out, size, e);
        case vr::Prop_ManufacturerName_String: return CopyString(g.runtimeName, out, size, e);
        default: break;
        }
    }
    if (e) *e = vr::TrackedProp_UnknownProperty;
    if (out && size) out[0] = 0;
    return 0;
}

bool SystemShim::PollNextEvent(vr::VREvent_t* ev, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    PumpEvents();
    if (g.events.empty() || !ev) return false;
    std::memcpy(ev, &g.events.front(), (std::min)(size, static_cast<uint32_t>(sizeof(vr::VREvent_t))));
    g.events.pop_front();
    return true;
}

vr::HiddenAreaMesh_t SystemShim::GetHiddenAreaMesh(vr::EVREye eye, vr::EHiddenAreaMeshType type) {
    vr::HiddenAreaMesh_t m = {};
    if (type != vr::k_eHiddenAreaMesh_Standard) return m;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    int e = eye == vr::Eye_Left ? 0 : 1;
    if (!g.hamTried[e] && g.extVisMask && g.session && g.frameViewsValid) {
        g.hamTried[e] = true;
        XrVisibilityMaskKHR vm = {XR_TYPE_VISIBILITY_MASK_KHR};
        if (XR_SUCCEEDED(fn.xrGetVisibilityMaskKHR(g.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, e,
                                                   XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &vm)) &&
            vm.vertexCountOutput && vm.indexCountOutput) {
            std::vector<XrVector2f> verts(vm.vertexCountOutput);
            std::vector<uint32_t> idx(vm.indexCountOutput);
            vm.vertexCapacityInput = vm.vertexCountOutput;
            vm.indexCapacityInput = vm.indexCountOutput;
            vm.vertices = verts.data();
            vm.indices = idx.data();
            if (XR_SUCCEEDED(fn.xrGetVisibilityMaskKHR(g.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, e,
                                                       XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &vm))) {
                float l, r, u, d;
                Tangents(e, &l, &r, &u, &d);
                g.ham[e].clear();
                for (uint32_t k : idx) {
                    if (k >= verts.size()) continue;
                    // Vertici sul piano z=-1 (tangenti) -> UV 0..1, v=0 in alto.
                    vr::HmdVector2_t p = {(verts[k].x - l) / (r - l), (u - verts[k].y) / (u - d)};
                    g.ham[e].push_back(p);
                }
                g.ham[e].resize(g.ham[e].size() / 3 * 3);
                vr::log::Line("OPENXR: visibility mask for eye " + std::to_string(e) + ": " +
                              std::to_string(g.ham[e].size() / 3) + " triangles.");
            }
        }
    }
    m.pVertexData = g.ham[e].data();
    m.unTriangleCount = static_cast<uint32_t>(g.ham[e].size() / 3);
    return m;
}

static void FillControllerState(int h, vr::VRControllerState_t* s) {
    const HandState& hs = g.hand[h];
    *s = {};
    s->unPacketNum = g.packet;
    if (!hs.active) return;
    s->rAxis[0] = {hs.stick.x, hs.stick.y};
    s->rAxis[1] = {hs.trigger, 0.0f};
    s->rAxis[2] = {hs.grip, 0.0f};
    uint64_t b = 0;
    if (hs.trigger > 0.5f) b |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
    if (hs.grip > 0.5f) b |= vr::ButtonMaskFromId(vr::k_EButton_Grip);
    if (hs.primary) b |= vr::ButtonMaskFromId(vr::k_EButton_A);
    if (hs.secondary) b |= vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu);
    if (hs.stickClick) b |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
    if (hs.menu) b |= vr::ButtonMaskFromId(vr::k_EButton_System);
    s->ulButtonPressed = b;
    s->ulButtonTouched = b;
}

bool SystemShim::GetControllerState(vr::TrackedDeviceIndex_t i, vr::VRControllerState_t* s, uint32_t) {
    if (!s || (i != kLeft && i != kRight)) return false;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (QpcMs() - g.lastSyncMs > 4.0) SyncActions(false);
    int h = i == kLeft ? 0 : 1;
    FillControllerState(h, s);
    return g.hand[h].active;
}

bool SystemShim::GetControllerStateWithPose(vr::ETrackingUniverseOrigin o, vr::TrackedDeviceIndex_t i,
                                            vr::VRControllerState_t* s, uint32_t size, vr::TrackedDevicePose_t* pose) {
    bool ok = GetControllerState(i, s, size);
    if (pose) {
        vr::TrackedDevicePose_t all[3];
        GetDeviceToAbsoluteTrackingPose(o, 0.0f, all, 3);
        *pose = all[i < 3 ? i : 0];
    }
    return ok;
}

static void Haptic(int h, float seconds, float freq, float amp) {
    if (!g.running || h < 0 || h > 1) return;
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = g.action[h][kActHaptic];
    hi.subactionPath = g.handPath[h];
    XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
    v.duration = seconds <= 0.0f ? XR_MIN_HAPTIC_DURATION : static_cast<XrDuration>(seconds * 1e9f);
    v.frequency = freq > 0.0f ? freq : XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = amp < 0.0f ? 0.0f : (amp > 1.0f ? 1.0f : amp);
    fn.xrApplyHapticFeedback(g.session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
}

void SystemShim::TriggerHapticPulse(vr::TrackedDeviceIndex_t i, uint32_t, unsigned short us) {
    if (i != kLeft && i != kRight) return;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    // Impulsi legacy ripetuti a ogni frame dal mod: 20 ms con ampiezza = durata/3900.
    Haptic(i == kLeft ? 0 : 1, 0.02f, 0.0f, us / 3900.0f);
}

bool SystemShim::IsInputAvailable() { return g.focused; }
bool SystemShim::IsSteamVRDrawingControllers() { return false; }
bool SystemShim::ShouldApplicationPause() { return false; }
bool SystemShim::ShouldApplicationReduceRenderingWork() { return false; }

vr::EDeviceActivityLevel SystemShim::GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t i) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (i != kHmd) return vr::k_EDeviceActivityLevel_Unknown;
    if (g.presenceKnown) return g.present ? vr::k_EDeviceActivityLevel_UserInteraction : vr::k_EDeviceActivityLevel_Standby;
    if (g.state == XR_SESSION_STATE_FOCUSED || g.state == XR_SESSION_STATE_VISIBLE) return vr::k_EDeviceActivityLevel_UserInteraction;
    if (g.state == XR_SESSION_STATE_SYNCHRONIZED) return vr::k_EDeviceActivityLevel_UserInteraction_Timeout;
    return vr::k_EDeviceActivityLevel_Idle;
}

void SystemShim::AcknowledgeQuit_Exiting() {}
const char* SystemShim::GetRuntimeVersion() { return g.runtimeVersion; }

// IVRCompositor
void CompositorShim::SetTrackingSpace(vr::ETrackingUniverseOrigin) {}
vr::ETrackingUniverseOrigin CompositorShim::GetTrackingSpace() { return vr::TrackingUniverseStanding; }

vr::EVRCompositorError CompositorShim::WaitGetPoses(vr::TrackedDevicePose_t* render, uint32_t nr, vr::TrackedDevicePose_t* game,
                                                    uint32_t ng) {
    if (!g.running || g.dead) {
        Sleep(10);
        return vr::VRCompositorError_DoNotHaveFocus;
    }
    if (!BeginFrameForCaller()) {
        Sleep(10);
        return vr::VRCompositorError_RequestFailed;
    }
    if (render && nr) g_sys.GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, render, nr);
    if (game && ng) g_sys.GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, game, ng);
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError CompositorShim::GetLastPoses(vr::TrackedDevicePose_t* render, uint32_t nr, vr::TrackedDevicePose_t* game,
                                                    uint32_t ng) {
    if (render && nr) g_sys.GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, render, nr);
    if (game && ng) g_sys.GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, game, ng);
    return vr::VRCompositorError_None;
}

vr::EVRCompositorError CompositorShim::Submit(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds,
                                              vr::EVRSubmitFlags flags) {
    if (!tex || tex->eType != vr::TextureType_DirectX || !tex->handle) return vr::VRCompositorError_InvalidTexture;
    if (!g.running || g.dead) return vr::VRCompositorError_DoNotHaveFocus;
    if (!FrameOwnedByCaller() && !BeginFrameForCaller()) return vr::VRCompositorError_RequestFailed;
    {
        std::lock_guard<std::recursive_mutex> lock(g.mutex);
        int e = eye == vr::Eye_Left ? 0 : 1;
        State::EyeSubmit& s = g.submit[e];
        s.tex = static_cast<ID3D11Texture2D*>(tex->handle);
        s.bounds = bounds ? *bounds : vr::VRTextureBounds_t{0, 0, 1, 1};
        s.hasPose = (flags & vr::Submit_TextureWithPose) != 0;
        if (s.hasPose) s.pose = static_cast<const vr::VRTextureWithPose_t*>(tex)->mDeviceToAbsoluteTracking;
        s.set = true;
        if (e == 0) return vr::VRCompositorError_None;
    }
    EndFrameLocked(true);
    return vr::VRCompositorError_None;
}

void CompositorShim::ClearLastSubmittedFrame() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.projValid = false;
}
void CompositorShim::PostPresentHandoff() {}
float CompositorShim::GetFrameTimeRemaining() { return 0.0f; }

void CompositorShim::FadeToColor(float seconds, float, float, float, float alpha, bool) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    g.fadeFrom = FadeAlphaNow();
    g.fadeTo = alpha;
    g.fadeStartMs = QpcMs();
    g.fadeDurMs = seconds * 1000.0;
}

vr::HmdColor_t CompositorShim::GetCurrentFadeColor(bool) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    return {0.0f, 0.0f, 0.0f, FadeAlphaNow()};
}

bool CompositorShim::IsFullscreen() { return true; }
uint32_t CompositorShim::GetCurrentSceneFocusProcess() { return GetCurrentProcessId(); }
bool CompositorShim::CanRenderScene() { return g.running; }
void CompositorShim::ShowMirrorWindow() {}
void CompositorShim::HideMirrorWindow() {}
bool CompositorShim::IsMirrorWindowVisible() { return false; }
vr::EVRCompositorError CompositorShim::GetMirrorTextureD3D11(vr::EVREye, void*, void** out) {
    if (out) *out = nullptr;
    return vr::VRCompositorError_RequestFailed;  // il mod usa la propria finestra specchio
}
void CompositorShim::ReleaseMirrorTextureD3D11(void*) {}

// IVROverlay
static OverlayItem* Find(vr::VROverlayHandle_t h) {
    auto it = g.overlays.find(h);
    return it == g.overlays.end() ? nullptr : &it->second;
}
#define OV_LOCK_FIND(h)                                        \
    std::lock_guard<std::recursive_mutex> lock(g.mutex);      \
    OverlayItem* o = Find(h);                                      \
    if (!o) return vr::VROverlayError_UnknownOverlay;

vr::EVROverlayError OverlayShim::FindOverlay(const char* key, vr::VROverlayHandle_t* out) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    for (auto& kv : g.overlays)
        if (key && kv.second.key == key) {
            if (out) *out = kv.first;
            return vr::VROverlayError_None;
        }
    return vr::VROverlayError_UnknownOverlay;
}

vr::EVROverlayError OverlayShim::CreateOverlay(const char* key, const char* name, vr::VROverlayHandle_t* out) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (!g.session) return vr::VROverlayError_RequestFailed;
    for (auto& kv : g.overlays)
        if (key && kv.second.key == key) return vr::VROverlayError_KeyInUse;
    OverlayItem o;
    o.handle = g.nextOverlay++;
    o.key = key ? key : "";
    o.name = name ? name : "";
    g.overlays[o.handle] = o;
    if (out) *out = o.handle;
    return vr::VROverlayError_None;
}

vr::EVROverlayError OverlayShim::DestroyOverlay(vr::VROverlayHandle_t h) {
    OV_LOCK_FIND(h);
    DestroySwap(o->swap);
    if (o->tex) o->tex->Release();
    g.overlays.erase(h);
    return vr::VROverlayError_None;
}

uint32_t OverlayShim::GetOverlayKey(vr::VROverlayHandle_t h, char* out, uint32_t size, vr::EVROverlayError* e) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    OverlayItem* o = Find(h);
    if (e) *e = o ? vr::VROverlayError_None : vr::VROverlayError_UnknownOverlay;
    if (!o) return 0;
    if (out && size) strncpy_s(out, size, o->key.c_str(), _TRUNCATE);
    return static_cast<uint32_t>(o->key.size() + 1);
}
uint32_t OverlayShim::GetOverlayName(vr::VROverlayHandle_t h, char* out, uint32_t size, vr::EVROverlayError* e) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    OverlayItem* o = Find(h);
    if (e) *e = o ? vr::VROverlayError_None : vr::VROverlayError_UnknownOverlay;
    if (!o) return 0;
    if (out && size) strncpy_s(out, size, o->name.c_str(), _TRUNCATE);
    return static_cast<uint32_t>(o->name.size() + 1);
}
vr::EVROverlayError OverlayShim::SetOverlayName(vr::VROverlayHandle_t h, const char* name) {
    OV_LOCK_FIND(h);
    o->name = name ? name : "";
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool on) {
    OV_LOCK_FIND(h);
    if (on) o->flags |= static_cast<uint32_t>(f);
    else o->flags &= ~static_cast<uint32_t>(f);
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool* on) {
    OV_LOCK_FIND(h);
    if (on) *on = (o->flags & static_cast<uint32_t>(f)) != 0;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayFlags(vr::VROverlayHandle_t h, uint32_t* f) {
    OV_LOCK_FIND(h);
    if (f) *f = o->flags;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayColor(vr::VROverlayHandle_t h, float, float, float) {
    OV_LOCK_FIND(h);
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayAlpha(vr::VROverlayHandle_t h, float a) {
    OV_LOCK_FIND(h);
    o->alpha = a;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayAlpha(vr::VROverlayHandle_t h, float* a) {
    OV_LOCK_FIND(h);
    if (a) *a = o->alpha;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t s) {
    OV_LOCK_FIND(h);
    o->sortOrder = s;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t* s) {
    OV_LOCK_FIND(h);
    if (s) *s = o->sortOrder;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float w) {
    OV_LOCK_FIND(h);
    o->width = w;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayWidthInMeters(vr::VROverlayHandle_t h, float* w) {
    OV_LOCK_FIND(h);
    if (w) *w = o->width;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayTextureBounds(vr::VROverlayHandle_t h, const vr::VRTextureBounds_t* b) {
    OV_LOCK_FIND(h);
    if (b) o->bounds = *b;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayTextureBounds(vr::VROverlayHandle_t h, vr::VRTextureBounds_t* b) {
    OV_LOCK_FIND(h);
    if (b) *b = o->bounds;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin,
                                                             const vr::HmdMatrix34_t* m) {
    OV_LOCK_FIND(h);
    if (m) o->xform = *m;
    o->attach = Attach::Absolute;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin* org,
                                                             vr::HmdMatrix34_t* m) {
    OV_LOCK_FIND(h);
    if (org) *org = vr::TrackingUniverseStanding;
    if (m) *m = o->xform;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t h, vr::TrackedDeviceIndex_t dev,
                                                                          const vr::HmdMatrix34_t* m) {
    OV_LOCK_FIND(h);
    if (m) o->xform = *m;
    if (dev == kHmd) o->attach = Attach::Hmd;
    else if (dev == kLeft || dev == kRight) {
        o->attach = Attach::Hand;
        o->hand = dev == kLeft ? 0 : 1;
    } else {
        return vr::VROverlayError_InvalidTrackedDevice;
    }
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t h, vr::TrackedDeviceIndex_t* dev,
                                                                          vr::HmdMatrix34_t* m) {
    OV_LOCK_FIND(h);
    if (dev) *dev = o->attach == Attach::Hmd ? kHmd : (o->hand == 0 ? kLeft : kRight);
    if (m) *m = o->xform;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::ShowOverlay(vr::VROverlayHandle_t h) {
    OV_LOCK_FIND(h);
    o->visible = true;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::HideOverlay(vr::VROverlayHandle_t h) {
    OV_LOCK_FIND(h);
    o->visible = false;
    o->hovered = false;
    return vr::VROverlayError_None;
}
bool OverlayShim::IsOverlayVisible(vr::VROverlayHandle_t h) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    OverlayItem* o = Find(h);
    return o && o->visible;
}
vr::EVROverlayError OverlayShim::GetTransformForOverlayCoordinates(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin,
                                                                   vr::HmdVector2_t uv, vr::HmdMatrix34_t* out) {
    OV_LOCK_FIND(h);
    Mat34 m;
    if (!OverlayInStage(*o, Now(), &m)) return vr::VROverlayError_RequestFailed;
    float lx = (uv.v[0] / (std::max)(o->mouseScale.v[0], 1e-3f) - 0.5f) * o->width;
    float ly = (uv.v[1] / (std::max)(o->mouseScale.v[1], 1e-3f) - 0.5f) * OverlayHeight(*o);
    for (int i = 0; i < 3; ++i) m.m[i][3] += m.m[i][0] * lx + m.m[i][1] * ly;
    if (out) *out = m;
    return vr::VROverlayError_None;
}
bool OverlayShim::PollNextOverlayEvent(vr::VROverlayHandle_t h, vr::VREvent_t* ev, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    OverlayItem* o = Find(h);
    if (!o || o->events.empty() || !ev) return false;
    std::memcpy(ev, &o->events.front(), (std::min)(size, static_cast<uint32_t>(sizeof(vr::VREvent_t))));
    o->events.pop_front();
    return true;
}
vr::EVROverlayError OverlayShim::GetOverlayInputMethod(vr::VROverlayHandle_t h, vr::VROverlayInputMethod* m) {
    OV_LOCK_FIND(h);
    if (m) *m = o->input;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayInputMethod(vr::VROverlayHandle_t h, vr::VROverlayInputMethod m) {
    OV_LOCK_FIND(h);
    o->input = m;
    if (m == vr::VROverlayInputMethod_None) o->hovered = false;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayMouseScale(vr::VROverlayHandle_t h, vr::HmdVector2_t* s) {
    OV_LOCK_FIND(h);
    if (s) *s = o->mouseScale;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayMouseScale(vr::VROverlayHandle_t h, const vr::HmdVector2_t* s) {
    OV_LOCK_FIND(h);
    if (s) o->mouseScale = *s;
    return vr::VROverlayError_None;
}
bool OverlayShim::ComputeOverlayIntersection(vr::VROverlayHandle_t h, const vr::VROverlayIntersectionParams_t* p,
                                             vr::VROverlayIntersectionResults_t* r) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    OverlayItem* o = Find(h);
    if (!o || !p || !r) return false;
    Mat34 m;
    if (!OverlayInStage(*o, Now(), &m)) return false;
    float pt[3], u, v, d;
    if (!Intersect(*o, m, p->vSource.v, p->vDirection.v, pt, &u, &v, &d)) return false;
    r->vPoint = {pt[0], pt[1], pt[2]};
    r->vNormal = {m.m[0][2], m.m[1][2], m.m[2][2]};
    r->vUVs = {u, v};
    r->fDistance = d;
    return true;
}
bool OverlayShim::IsHoverTargetOverlay(vr::VROverlayHandle_t h) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    OverlayItem* o = Find(h);
    return o && o->hovered;
}
vr::EVROverlayError OverlayShim::SetOverlayTexture(vr::VROverlayHandle_t h, const vr::Texture_t* t) {
    OV_LOCK_FIND(h);
    if (!t || t->eType != vr::TextureType_DirectX || !t->handle) return vr::VROverlayError_InvalidTexture;
    auto* tex = static_cast<ID3D11Texture2D*>(t->handle);
    if (o->tex != tex) {
        if (o->tex) o->tex->Release();
        tex->AddRef();
        o->tex = tex;
    }
    o->pixels.clear();
    o->pixW = o->pixH = 0;
    o->dirty = true;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::ClearOverlayTexture(vr::VROverlayHandle_t h) {
    OV_LOCK_FIND(h);
    if (o->tex) o->tex->Release();
    o->tex = nullptr;
    o->pixels.clear();
    o->pixW = o->pixH = 0;
    DestroySwap(o->swap);
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::SetOverlayRaw(vr::VROverlayHandle_t h, void* buf, uint32_t w, uint32_t h2, uint32_t bpp) {
    OV_LOCK_FIND(h);
    if (!buf || !w || !h2 || bpp != 4) return vr::VROverlayError_InvalidParameter;
    if (o->tex) o->tex->Release();
    o->tex = nullptr;
    o->pixels.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + static_cast<size_t>(w) * h2 * 4);
    o->pixW = w;
    o->pixH = h2;
    o->dirty = true;
    return vr::VROverlayError_None;
}
vr::EVROverlayError OverlayShim::GetOverlayTextureSize(vr::VROverlayHandle_t h, uint32_t* w, uint32_t* h2) {
    OV_LOCK_FIND(h);
    if (w) *w = o->tex ? o->swap.w : o->pixW;
    if (h2) *h2 = o->tex ? o->swap.h : o->pixH;
    return vr::VROverlayError_None;
}
bool OverlayShim::IsDashboardVisible() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    // VISIBLE senza focus = menu di sistema / dashboard sopra il gioco. SYNCHRONIZED
    // (non visibile) capita anche col visore appoggiato: non e' una dashboard.
    return g.running && g.state == XR_SESSION_STATE_VISIBLE;
}

// IVRInput: il manifesto del mod viene ignorato (i binding sono quelli
// suggeriti sopra); gli handle codificano (mano, azione).
static vr::VRActionHandle_t ActionHandle(int hand, int act) { return 1 + hand * 16 + act; }
static bool DecodeAction(vr::VRActionHandle_t h, int* hand, int* act) {
    if (h < 1 || h > 1 + 16 + kActCount) return false;
    *hand = static_cast<int>((h - 1) / 16);
    *act = static_cast<int>((h - 1) % 16);
    return *hand < 2 && *act < kActCount;
}

vr::EVRInputError InputShim::SetActionManifestPath(const char*) { return vr::VRInputError_None; }
vr::EVRInputError InputShim::GetActionSetHandle(const char*, vr::VRActionSetHandle_t* h) {
    if (h) *h = 1;
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::GetActionHandle(const char* name, vr::VRActionHandle_t* out) {
    if (!name || !out) return vr::VRInputError_InvalidParam;
    std::string n(name);
    size_t s = n.find_last_of('/');
    std::string leaf = s == std::string::npos ? n : n.substr(s + 1);
    int hand = -1, act = -1;
    if (leaf == "haptic_left") { hand = 0; act = kActHaptic; }
    else if (leaf == "haptic_right") { hand = 1; act = kActHaptic; }
    else {
        size_t u = leaf.find('_');
        if (u != std::string::npos) {
            std::string side = leaf.substr(0, u), what = leaf.substr(u + 1);
            hand = side == "left" ? 0 : (side == "right" ? 1 : -1);
            for (int a = 0; a < kActCount; ++a)
                if (what == kActNames[a]) act = a;
        }
    }
    if (hand < 0 || act < 0) {
        *out = vr::k_ulInvalidActionHandle;
        return vr::VRInputError_NameNotFound;
    }
    *out = ActionHandle(hand, act);
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::GetInputSourceHandle(const char*, vr::VRInputValueHandle_t* h) {
    if (h) *h = vr::k_ulInvalidInputValueHandle;
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::UpdateActionState(vr::VRActiveActionSet_t*, uint32_t, uint32_t) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    SyncActions(false);
    return g.running ? vr::VRInputError_None : vr::VRInputError_NoActiveActionSet;
}
vr::EVRInputError InputShim::GetDigitalActionData(vr::VRActionHandle_t a, vr::InputDigitalActionData_t* d, uint32_t,
                                                  vr::VRInputValueHandle_t) {
    int hand, act;
    if (!d || !DecodeAction(a, &hand, &act)) return vr::VRInputError_InvalidHandle;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    *d = {};
    const HandState& hs = g.hand[hand];
    bool v = false;
    switch (act) {
    case kActGrip: v = hs.grip > 0.5f; break;
    case kActPrimary: v = hs.primary; break;
    case kActSecondary: v = hs.secondary; break;
    case kActStickClick: v = hs.stickClick; break;
    case kActMenu: v = hs.menu; break;
    case kActTrigger: v = hs.trigger > 0.5f; break;
    case kActDpadL: case kActDpadR: d->bActive = false; return vr::VRInputError_None;
    default: break;
    }
    d->bActive = hs.active;
    d->bState = hs.active && v;
    d->activeOrigin = vr::k_ulInvalidInputValueHandle;
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::GetAnalogActionData(vr::VRActionHandle_t a, vr::InputAnalogActionData_t* d, uint32_t,
                                                 vr::VRInputValueHandle_t) {
    int hand, act;
    if (!d || !DecodeAction(a, &hand, &act)) return vr::VRInputError_InvalidHandle;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    *d = {};
    const HandState& hs = g.hand[hand];
    d->bActive = hs.active;
    d->activeOrigin = vr::k_ulInvalidInputValueHandle;
    if (!hs.active) return vr::VRInputError_None;
    if (act == kActStick) { d->x = hs.stick.x; d->y = hs.stick.y; }
    else if (act == kActTrigger) d->x = hs.trigger;
    else if (act == kActGrip) d->x = hs.grip;
    else d->bActive = false;
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::GetPoseActionDataRelativeToNow(vr::VRActionHandle_t a, vr::ETrackingUniverseOrigin o, float sec,
                                                            vr::InputPoseActionData_t* d, uint32_t, vr::VRInputValueHandle_t) {
    int hand, act;
    if (!d || !DecodeAction(a, &hand, &act)) return vr::VRInputError_InvalidHandle;
    vr::TrackedDevicePose_t all[3];
    g_sys.GetDeviceToAbsoluteTrackingPose(o, sec, all, 3);
    *d = {};
    d->bActive = g.hand[hand].poseActive;
    d->pose = all[hand == 0 ? kLeft : kRight];
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::TriggerHapticVibrationAction(vr::VRActionHandle_t a, float, float dur, float freq, float amp,
                                                          vr::VRInputValueHandle_t) {
    int hand, act;
    if (!DecodeAction(a, &hand, &act) || act != kActHaptic) return vr::VRInputError_InvalidHandle;
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    Haptic(hand, dur, freq, amp);
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::GetOriginLocalizedName(vr::VRInputValueHandle_t, char* out, uint32_t size, int32_t) {
    if (out && size) out[0] = 0;
    return vr::VRInputError_None;
}
vr::EVRInputError InputShim::GetOriginTrackedDeviceInfo(vr::VRInputValueHandle_t, vr::InputOriginInfo_t* i, uint32_t) {
    if (i) *i = {};
    return vr::VRInputError_InvalidHandle;
}

// IVRRenderModels: niente modelli (le pose sono gia' nel riferimento dei Touch).
vr::EVRRenderModelError RenderModelsShim::LoadRenderModel_Async(const char*, vr::RenderModel_t** m) {
    if (m) *m = nullptr;
    return vr::VRRenderModelError_NotSupported;
}
void RenderModelsShim::FreeRenderModel(vr::RenderModel_t*) {}
uint32_t RenderModelsShim::GetRenderModelName(uint32_t, char* out, uint32_t size) {
    if (out && size) out[0] = 0;
    return 0;
}
uint32_t RenderModelsShim::GetRenderModelCount() { return 0; }
uint32_t RenderModelsShim::GetComponentCount(const char*) { return 0; }
uint32_t RenderModelsShim::GetComponentName(const char*, uint32_t, char* out, uint32_t size) {
    if (out && size) out[0] = 0;
    return 0;
}
bool RenderModelsShim::GetComponentState(const char*, const char*, const vr::VRControllerState_t*,
                                         const vr::RenderModel_ControllerMode_State_t*, vr::RenderModel_ComponentState_t*) {
    return false;
}

// IVRSettings / IVRApplications: niente impostazioni di SteamVR.
const char* SettingsShim::GetSettingsErrorNameFromEnum(vr::EVRSettingsError) { return "not available (OpenXR)"; }
void SettingsShim::SetBool(const char*, const char*, bool, vr::EVRSettingsError* e) { if (e) *e = vr::VRSettingsError_WriteFailed; }
void SettingsShim::SetInt32(const char*, const char*, int32_t, vr::EVRSettingsError* e) { if (e) *e = vr::VRSettingsError_WriteFailed; }
void SettingsShim::SetFloat(const char*, const char*, float, vr::EVRSettingsError* e) { if (e) *e = vr::VRSettingsError_WriteFailed; }
void SettingsShim::SetString(const char*, const char*, const char*, vr::EVRSettingsError* e) { if (e) *e = vr::VRSettingsError_WriteFailed; }
bool SettingsShim::GetBool(const char*, const char*, vr::EVRSettingsError* e) { if (e) *e = vr::VRSettingsError_ReadFailed; return false; }
int32_t SettingsShim::GetInt32(const char*, const char*, vr::EVRSettingsError* e) { if (e) *e = vr::VRSettingsError_ReadFailed; return 0; }
float SettingsShim::GetFloat(const char*, const char*, vr::EVRSettingsError* e) { if (e) *e = vr::VRSettingsError_ReadFailed; return 0.0f; }
void SettingsShim::GetString(const char*, const char*, char* out, uint32_t size, vr::EVRSettingsError* e) {
    if (out && size) out[0] = 0;
    if (e) *e = vr::VRSettingsError_ReadFailed;
}
vr::EVRApplicationError ApplicationsShim::GetApplicationKeyByProcessId(uint32_t, char* out, uint32_t size) {
    if (out && size) out[0] = 0;
    return vr::VRApplicationError_UnknownApplication;
}
const char* ApplicationsShim::GetApplicationsErrorNameFromEnum(vr::EVRApplicationError) { return "not available (OpenXR)"; }

// --- API pubblica ------------------------------------------------------------------

bool ActiveRuntime(const std::string& overrideJson, std::string* name) {
    std::string json = RuntimeJsonPath(overrideJson);
    if (name) *name = json.empty() ? "" : FileName(json);
    return !json.empty() && GetFileAttributesA(json.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool Init(const std::string& overrideJson, std::string* error) {
    if (!LoadRuntime(overrideJson, error)) return false;
    if (g.session) return true;
    if (!CreateInstanceAndSystem(error)) return false;
    if (!CreateSession(error)) {
        DestroyAll();
        return false;
    }
    g.stop = false;
    g.keepalive = std::thread(KeepAliveLoop);
    // Primo giro di eventi: READY arriva subito dopo la creazione.
    for (int i = 0; i < 20 && !g.running; ++i) {
        {
            std::lock_guard<std::recursive_mutex> lock(g.mutex);
            PumpEvents();
        }
        if (!g.running) Sleep(50);
    }
    return true;
}

void Shutdown() {
    if (!g.instance) return;
    vr::log::Line("OPENXR: shutting down the session.");
    DestroyAll();
}

bool Alive() { return g.instance != XR_NULL_HANDLE && !g.dead; }
const char* RuntimeName() { return g.runtimeName.c_str(); }

vr::IVRSystem* System() { return g.session ? &g_sys : nullptr; }
vr::IVRCompositor* Compositor() { return g.session ? &g_comp : nullptr; }
vr::IVROverlay* Overlay() { return g.session ? &g_ov : nullptr; }
vr::IVRInput* Input() { return g.session ? &g_in : nullptr; }
vr::IVRRenderModels* RenderModels() { return g.session ? &g_rm : nullptr; }
vr::IVRSettings* Settings() { return g.session ? &g_set : nullptr; }
vr::IVRApplications* Applications() { return g.session ? &g_app : nullptr; }

ID3D11Device* D3D11Device() { return g.device; }

float SecondsToPhotons() {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (!g.running || !g.lastPredicted) return 0.0f;
    XrTime now = Now();
    XrTime t = g.frameBegun ? g.frameState.predictedDisplayTime : g.lastPredicted;
    XrDuration period = g.lastPeriod > 0 ? g.lastPeriod : 13888889;
    int guard = 0;
    while (t < now && guard++ < 16) t += period;
    double s = static_cast<double>(t - now) * 1e-9;
    return s < 0.0 ? 0.0f : static_cast<float>(s);
}

void SetGripFix(bool on, float pitchDeg) {
    std::lock_guard<std::recursive_mutex> lock(g.mutex);
    if (g.gripFix == on && std::fabs(g.gripPitch - pitchDeg) < 0.01f) return;
    g.gripFix = on;
    g.gripPitch = pitchDeg;
    if (g.session && g.action[0][kActPose]) CreateHandSpaces();
    vr::log::Line(std::string("OPENXR: grip pose correction ") + (on ? "on, pitch " + std::to_string(pitchDeg) : "off") + ".");
}

}  // namespace vr::xr
