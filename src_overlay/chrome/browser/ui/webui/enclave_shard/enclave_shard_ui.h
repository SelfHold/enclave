// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_UI_H_
#define CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

namespace enclave {

class EnclaveShardUI;

// chrome://shard, "Sync via Shard" (docs/SHARD.md). Desktop and
// Android.
class EnclaveShardUIConfig
    : public content::DefaultWebUIConfig<EnclaveShardUI> {
 public:
  EnclaveShardUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIEnclaveShardHost) {}
};

// Serves the page from string constants (no grit, no chrome://resources) and
// owns the message handler.
class EnclaveShardUI : public content::WebUIController {
 public:
  explicit EnclaveShardUI(content::WebUI* web_ui);
  EnclaveShardUI(const EnclaveShardUI&) = delete;
  EnclaveShardUI& operator=(const EnclaveShardUI&) = delete;
  ~EnclaveShardUI() override;
};

}  // namespace enclave

#endif  // CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_UI_H_
