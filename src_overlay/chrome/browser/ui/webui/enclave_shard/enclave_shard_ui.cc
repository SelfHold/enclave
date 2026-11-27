// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/ui/webui/enclave_shard/enclave_shard_ui.h"

#include <memory>
#include <string>
#include <string_view>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/enclave_shard/enclave_shard_handler.h"
#include "chrome/browser/ui/webui/enclave_shard/enclave_shard_page.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"

namespace enclave {

namespace {

// "" (chrome://shard/), "index.html", "shard.css", "shard.js";
// the query and the fragment are ignored.
std::string_view StripQuery(const std::string& path) {
  std::string_view view(path);
  size_t end = view.find_first_of("?#");
  return end == std::string_view::npos ? view : view.substr(0, end);
}

std::string_view BodyFor(std::string_view path) {
  if (path.empty() || path == "index.html") {
    return kEnclaveShardHtml;
  }
  if (path == "shard.css") {
    return kEnclaveShardCss;
  }
  if (path == "shard.js") {
    return kEnclaveShardJs;
  }
  return {};
}

bool ShouldHandleRequest(const std::string& path) {
  return !BodyFor(StripQuery(path)).empty();
}

void HandleRequest(const std::string& path,
                   content::WebUIDataSource::GotDataCallback callback) {
  std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
      std::string(BodyFor(StripQuery(path)))));
}

}  // namespace

EnclaveShardUI::EnclaveShardUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      profile, chrome::kChromeUIEnclaveShardHost);
  // The default WebUI CSP stays: script-src chrome://resources 'self',
  // Trusted Types required with no policy (the page never parses HTML from
  // strings), frame-ancestors 'none'. The MIME type comes from the path
  // extension (WebUIDataSourceImpl::GetMimeType).
  source->SetRequestFilter(base::BindRepeating(&ShouldHandleRequest),
                           base::BindRepeating(&HandleRequest));

  web_ui->AddMessageHandler(std::make_unique<EnclaveShardHandler>(profile));
}

EnclaveShardUI::~EnclaveShardUI() = default;

}  // namespace enclave
