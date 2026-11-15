#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/plugin-host/HostApi.h"

namespace pluginhost::vst3adapter
{

// Opens one .vst3 bundle, discovers its audio-effect classes, and registers
// each through the same format-neutral host API used by the CLAP adapter.
size_t registerFromBundle(const char *bundlePath,
                          double sampleRate,
                          uint32_t maxBlock,
                          const BareDAW_HostApi &hostApi,
                          std::vector<void *> &ownedRegistrationData);

bool hasLiveInstances(const void *registrationData);
void releaseRegistrationData(void *registrationData);
std::string registrationTypeName(const void *registrationData);
std::string registrationDisplayName(const void *registrationData);

}
