// Test standalone (nessuna dipendenza dal gioco) per verificare che
// SteamVR e il Quest 3 siano raggiungibili via OpenVR prima di integrare
// tutto nella DLL del mod. Stampa la posa dell'HMD e dei controller per
// qualche secondo.
#include <openvr.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

void PrintPose(const char* label, const vr::TrackedDevicePose_t& pose) {
    if (!pose.bPoseIsValid) {
        printf("%s: posa non valida (dispositivo non tracciato)\n", label);
        return;
    }
    const auto& m = pose.mDeviceToAbsoluteTracking;
    printf("%s: pos=(%.3f, %.3f, %.3f)\n", label, m.m[0][3], m.m[1][3], m.m[2][3]);
}

// "vrtest proj": tangenti degli occhi da GetProjectionRaw e dalla matrice di
// GetProjectionMatrix (stile OpenGL), per controllare il verso di top/bottom.
int PrintProjection() {
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem* sys = vr::VR_Init(&err, vr::VRApplication_Scene);
    if (err != vr::VRInitError_None) {
        printf("VR_Init fallita: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    for (int i = 0; i < 60; ++i) {  // l'HMD arriva qualche secondo dopo l'avvio
        float l0, r0, t0, b0;
        sys->GetProjectionRaw(vr::Eye_Left, &l0, &r0, &t0, &b0);
        if (r0 - l0 > 0.1f) break;
        Sleep(500);
    }
    for (int e = 0; e < 2; ++e) {
        float l, r, t, b;
        sys->GetProjectionRaw(static_cast<vr::EVREye>(e), &l, &r, &t, &b);
        vr::HmdMatrix44_t p = sys->GetProjectionMatrix(static_cast<vr::EVREye>(e), 1.0f, 1000.0f);
        printf("%s raw: left %.3f right %.3f top %.3f bottom %.3f\n", e ? "R" : "L", l, r, t, b);
        printf("  matrix: m00 %.3f m02 %.3f m11 %.3f m12 %.3f -> left %.3f right %.3f up %.3f down %.3f\n",
               p.m[0][0], p.m[0][2], p.m[1][1], p.m[1][2], (p.m[0][2] - 1) / p.m[0][0], (p.m[0][2] + 1) / p.m[0][0],
               (p.m[1][2] + 1) / p.m[1][1], (1 - p.m[1][2]) / p.m[1][1]);
    }
    vr::VR_Shutdown();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "proj") == 0) return PrintProjection();
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem* vrSystem = vr::VR_Init(&err, vr::VRApplication_Scene);
    if (err != vr::VRInitError_None) {
        printf("VR_Init fallita: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    printf("OpenVR inizializzato correttamente.\n");

    char buf[256];
    vrSystem->GetStringTrackedDeviceProperty(
        vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String, buf, sizeof(buf));
    printf("Sistema di tracking HMD: %s\n", buf);

    for (int i = 0; i < 100; ++i) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vrSystem->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);

        PrintPose("HMD", poses[vr::k_unTrackedDeviceIndex_Hmd]);

        for (vr::TrackedDeviceIndex_t d = 0; d < vr::k_unMaxTrackedDeviceCount; ++d) {
            if (d == vr::k_unTrackedDeviceIndex_Hmd) continue;
            if (vrSystem->GetTrackedDeviceClass(d) == vr::TrackedDeviceClass_Controller) {
                char label[64];
                snprintf(label, sizeof(label), "Controller[%u]", d);
                PrintPose(label, poses[d]);
            }
        }
        Sleep(500);
    }

    vr::VR_Shutdown();
    return 0;
}
