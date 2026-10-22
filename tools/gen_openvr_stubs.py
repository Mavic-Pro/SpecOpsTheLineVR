"""Generates src/xr_shim_ifaces.h: OpenXR-backed classes that implement the
OpenVR interfaces the mod uses (IVRSystem, IVRCompositor, IVROverlay, IVRInput,
IVRRenderModels, IVRSettings, IVRApplications).

Every pure virtual method of vendor/openvr/headers/openvr.h gets an override.
The methods listed in IMPLEMENTED are only declared (their bodies live in
src/vr_openxr.cpp); every other method is an inline stub returning a
value-initialised result, so the classes stay instantiable when openvr.h
changes. Run after updating vendor/openvr:

    python tools/gen_openvr_stubs.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "vendor", "openvr", "headers", "openvr.h")
OUT = os.path.join(ROOT, "src", "xr_shim_ifaces.h")

# Methods implemented by hand in vr_openxr.cpp, per interface.
IMPLEMENTED = {
    "IVRSystem": [
        "GetRecommendedRenderTargetSize", "GetProjectionMatrix", "GetProjectionRaw", "GetEyeToHeadTransform",
        "GetTimeSinceLastVsync", "GetDXGIOutputInfo", "GetDeviceToAbsoluteTrackingPose",
        "GetTrackedDeviceIndexForControllerRole", "GetControllerRoleForTrackedDeviceIndex", "GetTrackedDeviceClass",
        "IsTrackedDeviceConnected", "GetBoolTrackedDeviceProperty", "GetFloatTrackedDeviceProperty",
        "GetInt32TrackedDeviceProperty", "GetStringTrackedDeviceProperty", "PollNextEvent", "GetHiddenAreaMesh",
        "GetControllerState", "GetControllerStateWithPose", "TriggerHapticPulse", "IsInputAvailable",
        "GetTrackedDeviceActivityLevel", "AcknowledgeQuit_Exiting", "GetRuntimeVersion", "ShouldApplicationPause",
        "ShouldApplicationReduceRenderingWork", "IsSteamVRDrawingControllers",
    ],
    "IVRCompositor": [
        "WaitGetPoses", "GetLastPoses", "Submit", "PostPresentHandoff", "FadeToColor", "GetCurrentFadeColor",
        "ShowMirrorWindow", "HideMirrorWindow", "IsMirrorWindowVisible", "GetMirrorTextureD3D11",
        "ReleaseMirrorTextureD3D11", "SetTrackingSpace", "GetTrackingSpace", "ClearLastSubmittedFrame",
        "IsFullscreen", "CanRenderScene", "GetFrameTimeRemaining", "GetCurrentSceneFocusProcess",
    ],
    "IVROverlay": [
        "FindOverlay", "CreateOverlay", "DestroyOverlay", "GetOverlayKey", "GetOverlayName", "SetOverlayName",
        "SetOverlayFlag", "GetOverlayFlag", "GetOverlayFlags", "SetOverlayColor", "SetOverlayAlpha",
        "GetOverlayAlpha", "SetOverlayWidthInMeters", "GetOverlayWidthInMeters", "SetOverlaySortOrder",
        "GetOverlaySortOrder", "SetOverlayTextureBounds", "GetOverlayTextureBounds", "SetOverlayTransformAbsolute",
        "GetOverlayTransformAbsolute", "SetOverlayTransformTrackedDeviceRelative",
        "GetOverlayTransformTrackedDeviceRelative", "ShowOverlay", "HideOverlay", "IsOverlayVisible",
        "SetOverlayTexture", "ClearOverlayTexture", "SetOverlayRaw", "SetOverlayInputMethod",
        "GetOverlayInputMethod", "SetOverlayMouseScale", "GetOverlayMouseScale", "PollNextOverlayEvent",
        "ComputeOverlayIntersection", "IsDashboardVisible", "GetTransformForOverlayCoordinates",
        "IsHoverTargetOverlay", "GetOverlayTextureSize",
    ],
    "IVRInput": [
        "SetActionManifestPath", "GetActionSetHandle", "GetActionHandle", "GetInputSourceHandle",
        "UpdateActionState", "GetDigitalActionData", "GetAnalogActionData", "GetPoseActionDataRelativeToNow",
        "TriggerHapticVibrationAction", "GetOriginTrackedDeviceInfo", "GetOriginLocalizedName",
    ],
    "IVRRenderModels": ["GetComponentCount", "LoadRenderModel_Async", "FreeRenderModel", "GetComponentState",
                        "GetComponentName", "GetRenderModelName", "GetRenderModelCount"],
    "IVRSettings": ["GetSettingsErrorNameFromEnum", "GetInt32", "SetInt32", "GetString", "GetBool", "GetFloat",
                    "SetBool", "SetFloat", "SetString"],
    "IVRApplications": ["GetApplicationKeyByProcessId", "GetApplicationsErrorNameFromEnum"],
}

CLASS = {"IVRSystem": "SystemShim", "IVRCompositor": "CompositorShim", "IVROverlay": "OverlayShim",
         "IVRInput": "InputShim", "IVRRenderModels": "RenderModelsShim", "IVRSettings": "SettingsShim",
         "IVRApplications": "ApplicationsShim"}


def split_params(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out]


def clean_param(p):
    p = re.sub(r"\b(VR_ARRAY_COUNT|VR_OUT_ARRAY_COUNT|VR_OUT_STRING|VR_OUT_STRUCT|VR_OUT_ARRAY_D|VR_IN_ARRAY_COUNT)\s*\([^)]*\)", "", p)
    p = re.sub(r"\s*=\s*[^,]+$", "", p)  # default argument
    return re.sub(r"\s+", " ", p).strip()


def parse(src, name):
    m = re.search(r"class %s\s*\{" % name, src)
    if not m:
        raise SystemExit("interface %s not found" % name)
    # Corpo della classe fino alla graffa che la chiude (le interfacce sono indentate con tab).
    depth, i = 1, m.end()
    while depth and i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    body = re.sub(r"/\*.*?\*/", "", src[m.end():i - 1], flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    methods = []
    for mm in re.finditer(r"virtual\s+(.+?)\s*\(\s*(.*?)\s*\)\s*(const)?\s*=\s*0\s*;", body, re.S):
        head = re.sub(r"\s+", " ", mm.group(1)).strip()
        const = " const" if mm.group(3) else ""
        parts = head.split(" ")
        mname = parts[-1].lstrip("*&")
        ret = " ".join(parts[:-1])
        if parts[-1].startswith("*"):
            ret += " *"
        params = [clean_param(p) for p in split_params(mm.group(2))] if mm.group(2).strip() else []
        params = [p for p in params if p and p != "void"]
        methods.append((ret, mname, params, const))
    return methods


def main():
    src = open(HEADER, encoding="utf-8", errors="replace").read()
    lines = ["// Generated by tools/gen_openvr_stubs.py from vendor/openvr/headers/openvr.h.",
             "// Do not edit: the methods without a body are implemented in vr_openxr.cpp, the",
             "// others are stubs the mod never calls.", "#pragma once", "#include <openvr.h>", "",
             "namespace vr::xr {", ""]
    for iface, cls in CLASS.items():
        methods = parse(src, iface)
        impl = set(IMPLEMENTED[iface])
        names = {m[1] for m in methods}
        missing = impl - names
        if missing:
            raise SystemExit("%s: not in openvr.h: %s" % (iface, sorted(missing)))
        lines.append("class %s : public vr::%s {" % (cls, iface))
        lines.append("public:")
        for ret, mname, params, const in methods:
            sig = "%s %s(%s)%s override" % (ret, mname, ", ".join(params), const)
            if mname in impl:
                lines.append("    %s;" % sig)
            elif ret == "void":
                lines.append("    %s {}" % sig)
            else:
                lines.append("    %s { return {}; }" % sig)
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace vr::xr")
    lines.append("")
    text = "\n".join(lines)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    total = sum(len(parse(src, i)) for i in CLASS)
    print("%s: %d methods, %d implemented by hand" % (OUT, total, sum(len(v) for v in IMPLEMENTED.values())))


if __name__ == "__main__":
    main()
