# Chromium 158 audit: what can go

Checked against 158.0.8127.88. "Done" means it is in `config/gn/common.gn` or
`src_overlay/chrome/app/bezel_privacy_defaults.h`.

## Already off in an unbranded build (no action)

- UMA/UKM upload: `components/metrics/metrics_service_accessor.cc` refuses reporting
  unless Google Chrome branded. Domain reliability needs metrics consent, so it is off too.
- Crash upload: `components/crash/core/app/crash_reporter_client.cc` has no URL unless
  branded and official.
- RLZ, Google updater, What's New, widevine on desktop, cast receiver. NaCl is gone.

## Done

| What | How |
|---|---|
| Finch seed, field trial config | switches `disable-variations-seed-fetch`, `disable-field-trial-config` |
| Gemini in Chrome | features `Glic`, `GlicRollout`, `GlicActor` |
| Optimization guide hints, model downloads, server AI, page text | features `OptimizationHints`, `OptimizationTargetPrediction`, `OptimizationGuideModelExecution`, `ModelQualityLogging`, `OptimizationGuidePageContentExtraction`, `PermissionsAIP92` |
| Local UKM and ML ranking | features `Ukm`, `SegmentationPlatform` |
| Topics, attribution reporting | features `BrowsingTopics`, `ConversionMeasurement` |
| Time checks to clients2.google.com | feature `NetworkTimeServiceQuerying` |
| Discover feed, NTP promos | features `InterestFeedV2`, `DiscoFeedEndpoint`, `NtpMiddleSlotPromo`, `NtpOneGoogleBar`, `NtpComposebox` |
| Autofill server votes | feature `AutofillServerCommunication` |
| On-device model service, remoting, WebXR, Network Error Logging | GN `use_on_device_model_service`, `enable_remoting`, `enable_vr`, `enable_reporting` = false |

## Tried and dropped (GN asserts, need patches)

`enable_compose`, `enable_background_mode`, `enable_on_device_translation`,
`enable_mdns` / `enable_service_discovery`: setting them false fails `gn gen` because
other targets depend on them unconditionally.

## Next candidates

- Component updater: remove registrations in `chrome/browser/component_updater/registration.cc`
  that fetch models and lists we don't use (OnDeviceHeadSuggest is optional; keep CRLSet,
  PKIMetadata, OriginTrials, SSLErrorAssistant, FileTypePolicies, Zxcvbn).
- `enable_lens_desktop`, `enable_screen_ai_service`, `enable_speech_service`,
  `enable_media_remoting`: GN args, not tried yet.
- Android: GMS cast and vision dependencies in `chrome/android/BUILD.gn` (patch).
- Translate script fetch, Linux spellcheck dictionary downloads: off by pref.
- `optimize_for_size = true`: about 10-20% smaller binary, some CPU cost. Measure first.

## Keep: sign-in and sync depend on these

- `enable_dice_support`, `enable_bound_session_credentials`, `enable_supervised_users`.
- GCM (`components/gcm_driver`): sync invalidations. Without it sync only polls.
- Android: `google_play_services_auth_base_java` is required for Google sign-in.
- Never use `--disable-background-networking`: it also stops extension and sync traffic.
