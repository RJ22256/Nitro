// Logs fatal CPU exceptions (with a call stack) to Nitro's log.
#pragma once

#include <string>

namespace lsfgi {

// symbolPath: folders to look for NitroHook.pdb in (';'-separated).
void InstallCrashLogger(const std::wstring& symbolPath);

} // namespace lsfgi
