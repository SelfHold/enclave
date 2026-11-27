// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_PAGE_H_
#define CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_PAGE_H_

namespace bezel {

// The files of chrome://cloudsync, served from memory (no grit). Dark-only
// design tokens and `hm-*` components, with a phone layout below 768 px.
extern const char kBezelCloudSyncHtml[];  // "" and "index.html"
extern const char kBezelCloudSyncCss[];   // "cloudsync.css"
extern const char kBezelCloudSyncJs[];    // "cloudsync.js"

}  // namespace bezel

#endif  // CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_PAGE_H_
