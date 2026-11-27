// Enclave: startup switches applied before anything else reads the command line.

#ifndef CHROME_APP_ENCLAVE_PRIVACY_DEFAULTS_H_
#define CHROME_APP_ENCLAVE_PRIVACY_DEFAULTS_H_

#include <string>

#include "base/command_line.h"

namespace enclave {

// Features that talk to Google or burn RAM/CPU on things we do not use.
// Names are verified against Chromium 158 (BASE_FEATURE drops the leading k
// unless a name string is given). Sync, sign-in, search suggestions and
// preloading do not depend on any of them.
inline constexpr char kDisabledFeatures[] =
    // Gemini in Chrome.
    "Glic,GlicRollout,GlicActor,"
    // Optimization guide: hints, model downloads, server AI, page text.
    "OptimizationHints,OptimizationTargetPrediction,"
    "OptimizationGuideModelExecution,ModelQualityLogging,"
    "OptimizationGuidePageContentExtraction,PermissionsAIP92,"
    // Local UKM recording and the ML ranking that feeds on it.
    "Ukm,SegmentationPlatform,"
    // Ad measurement APIs.
    "BrowsingTopics,ConversionMeasurement,"
    // Clock checks against clients2.google.com.
    "NetworkTimeServiceQuerying,"
    // Android Discover feed and NTP promos.
    "InterestFeedV2,DiscoFeedEndpoint,NtpMiddleSlotPromo,NtpOneGoogleBar,"
    "NtpComposebox,"
    // Form votes and queries to content-autofill.googleapis.com.
    "AutofillServerCommunication";

inline void ApplyPrivacyDefaults(base::CommandLine* command_line) {
  // Stops downloads of the Finch seed and ignores bundled field trials.
  command_line->AppendSwitch("disable-variations-seed-fetch");
  command_line->AppendSwitch("disable-field-trial-config");
  // Enclave's design is dark only (ui/base/ui_base_switches.h).
  command_line->AppendSwitch("force-dark-mode");

  // Keeps whatever the user passed and adds ours.
  std::string disabled = command_line->GetSwitchValueASCII("disable-features");
  disabled = disabled.empty() ? kDisabledFeatures
                              : disabled + "," + kDisabledFeatures;
  command_line->AppendSwitchASCII("disable-features", disabled);
}

}  // namespace enclave

#endif  // CHROME_APP_ENCLAVE_PRIVACY_DEFAULTS_H_
