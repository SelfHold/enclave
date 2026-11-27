// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_PAGE_H_
#define CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_PAGE_H_

namespace enclave {

// The files of chrome://shard, served from memory (no grit). Dark-only
// design tokens and `hm-*` components, with a phone layout below 768 px.
extern const char kEnclaveShardHtml[];  // "" and "index.html"
extern const char kEnclaveShardCss[];   // "shard.css"
extern const char kEnclaveShardJs[];    // "shard.js"

}  // namespace enclave

#endif  // CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_PAGE_H_
