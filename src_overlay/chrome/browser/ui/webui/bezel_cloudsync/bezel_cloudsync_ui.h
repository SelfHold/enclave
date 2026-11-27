// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_UI_H_
#define CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

namespace bezel {

class BezelCloudSyncUI;

// chrome://cloudsync, "Sync via CloudSync" (docs/CLOUDSYNC.md). Desktop and
// Android.
class BezelCloudSyncUIConfig
    : public content::DefaultWebUIConfig<BezelCloudSyncUI> {
 public:
  BezelCloudSyncUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIBezelCloudSyncHost) {}
};

// Serves the page from string constants (no grit, no chrome://resources) and
// owns the message handler.
class BezelCloudSyncUI : public content::WebUIController {
 public:
  explicit BezelCloudSyncUI(content::WebUI* web_ui);
  BezelCloudSyncUI(const BezelCloudSyncUI&) = delete;
  BezelCloudSyncUI& operator=(const BezelCloudSyncUI&) = delete;
  ~BezelCloudSyncUI() override;
};

}  // namespace bezel

#endif  // CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_UI_H_
