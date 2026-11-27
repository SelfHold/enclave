// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_BEZEL_CLOUDSYNC_ANDROID_ANDROID_CONNECTOR_TRANSPORT_H_
#define CHROME_BROWSER_BEZEL_CLOUDSYNC_ANDROID_ANDROID_CONNECTOR_TRANSPORT_H_

#include <memory>

#include "components/bezel_cloudsync/connector_transport.h"

namespace bezel {

// The Connector API over CloudSync's bound service on Android
// (CONNECTOR.md § 3.2): ICloudSyncConnector.call / watch through JNI.
// Used from the UI thread; replies come back there.
std::unique_ptr<cloudsync::ConnectorTransport>
CreateAndroidConnectorTransport();

}  // namespace bezel

#endif  // CHROME_BROWSER_BEZEL_CLOUDSYNC_ANDROID_ANDROID_CONNECTOR_TRANSPORT_H_
