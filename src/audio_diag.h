#pragma once
#include <string>
// Diagnostica audio: dispositivo di riproduzione predefinito di Windows.
namespace vr::audio {
std::string DefaultDeviceName();
// Scrive nel log il nome del dispositivo, solo quando cambia rispetto all'ultima chiamata.
void LogDefaultDevice(const char* when);
}  // namespace vr::audio
