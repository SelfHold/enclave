// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_ENCLAVE_SHARD_ANDROID_ANDROID_CONNECTOR_TRANSPORT_H_
#define CHROME_BROWSER_ENCLAVE_SHARD_ANDROID_ANDROID_CONNECTOR_TRANSPORT_H_

#include <memory>

#include "components/enclave_shard/connector_transport.h"

namespace enclave {

// The Connector API over Shard's bound service on Android
// (CONNECTOR.md § 3.2): IShardConnector.call / watch through JNI.
// Used from the UI thread; replies come back there.
std::unique_ptr<shard::ConnectorTransport>
CreateAndroidConnectorTransport();

}  // namespace enclave

#endif  // CHROME_BROWSER_ENCLAVE_SHARD_ANDROID_ANDROID_CONNECTOR_TRANSPORT_H_
