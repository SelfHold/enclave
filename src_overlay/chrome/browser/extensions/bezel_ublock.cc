// Bezel: uBlock Origin bundled as a component extension.

#include "chrome/browser/extensions/bezel_ublock.h"

#include "build/build_config.h"

#if BUILDFLAG(IS_ANDROID)
#include <cstdint>
#include <vector>

#include "base/android/apk_assets.h"
#include "base/base_paths.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "third_party/zlib/google/zip.h"
#endif  // BUILDFLAG(IS_ANDROID)

namespace bezel {

std::string_view UblockManifest() {
  static constexpr char kManifest[] =
#include "chrome/browser/extensions/bezel_ublock_manifest.inc"
      ;
  return kManifest;
}

#if BUILDFLAG(IS_ANDROID)
namespace {

constexpr base::FilePath::CharType kManifestFile[] =
    FILE_PATH_LITERAL("manifest.json");

bool Unpack(const base::FilePath& root, const base::FilePath& target) {
  base::MemoryMappedFile::Region region;
  int fd = base::android::OpenApkAsset(kUblockApkAsset, &region);
  if (fd < 0) {
    LOG(ERROR) << "Bezel: " << kUblockApkAsset << " is missing from the APK.";
    return false;
  }
  // The asset is stored uncompressed, so it is a plain byte range of the APK.
  base::File apk(fd);
  std::vector<uint8_t> bytes(region.size);
  if (!apk.ReadAndCheck(region.offset, bytes)) {
    return false;
  }

  // Removes older versions and anything left by an interrupted unpack.
  if (!base::DeletePathRecursively(root) || !base::CreateDirectory(root)) {
    return false;
  }
  const base::FilePath zip_path = root.AppendASCII("bundle.zip");
  const base::FilePath staging = root.AppendASCII("staging");
  const bool unpacked = base::WriteFile(zip_path, bytes) &&
                        zip::Unzip(zip_path, staging) &&
                        base::PathExists(staging.Append(kManifestFile));
  base::DeleteFile(zip_path);
  return unpacked && base::Move(staging, target);
}

}  // namespace

base::FilePath EnsureUblockExtracted(const std::string& version) {
  base::FilePath data;
  if (version.empty() ||
      version.find_first_not_of("0123456789.") != std::string::npos ||
      !base::PathService::Get(base::DIR_ANDROID_APP_DATA, &data)) {
    return base::FilePath();
  }
  const base::FilePath root = data.AppendASCII(kUblockDirName);
  const base::FilePath target = root.AppendASCII(version);
  if (base::PathExists(target.Append(kManifestFile))) {
    return target;
  }
  if (!Unpack(root, target)) {
    LOG(ERROR) << "Bezel: failed to unpack uBlock Origin " << version;
    base::DeletePathRecursively(root);
    return base::FilePath();
  }
  return target;
}
#endif  // BUILDFLAG(IS_ANDROID)

}  // namespace bezel
