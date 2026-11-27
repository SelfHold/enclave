// Enclave: uBlock Origin bundled as a component extension.

#ifndef CHROME_BROWSER_EXTENSIONS_ENCLAVE_UBLOCK_H_
#define CHROME_BROWSER_EXTENSIONS_ENCLAVE_UBLOCK_H_

#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "build/build_config.h"

namespace enclave {

// Directory of the unpacked extension, relative to chrome::DIR_RESOURCES on
// desktop and to base::DIR_ANDROID_APP_DATA on Android.
inline constexpr char kUblockDirName[] = "enclave_ublock";

// Name of the zipped copy among the APK assets.
inline constexpr char kUblockApkAsset[] = "assets/enclave_ublock.zip";

// The extension's manifest, compiled into the binary.
std::string_view UblockManifest();

#if BUILDFLAG(IS_ANDROID)
// Unpacks the APK copy into app data once per extension version, removing
// older versions. Blocks on disk IO. Returns an empty path on failure.
base::FilePath EnsureUblockExtracted(const std::string& version);
#endif

}  // namespace enclave

#endif  // CHROME_BROWSER_EXTENSIONS_ENCLAVE_UBLOCK_H_
