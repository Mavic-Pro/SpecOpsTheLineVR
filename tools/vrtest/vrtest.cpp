// Test standalone (nessuna dipendenza dal gioco) per verificare che
// SteamVR e il Quest 3 siano raggiungibili via OpenVR prima di integrare
// tutto nella DLL del mod. Stampa la posa dell'HMD e dei controller per
// qualche secondo.
#include <openvr.h>
#include <cstdio>
#include <windows.h>

void PrintPose(const char* label, const vr::TrackedDevicePose_t& pose) {
    if (!pose.bPoseIsValid) {
        printf("%s: posa non valida (dispositivo non tracciato)\n", label);
        return;
    }
    const auto& m = pose.mDeviceToAbsoluteTracking;
    printf("%s: pos=(%.3f, %.3f, %.3f)\n", label, m.m[0][3], m.m[1][3], m.m[2][3]);
}

int main() {
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
