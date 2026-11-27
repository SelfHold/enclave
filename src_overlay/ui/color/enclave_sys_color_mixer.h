// Enclave: system color tokens of Enclave's dark-only design palette.
//
// Runs right after the sys mixers, so every leaf color in Views and WebUI that
// is derived from a kColorSys* token picks up the Enclave palette. The palette
// is dark only: light mode gets the same values.

#ifndef UI_COLOR_ENCLAVE_SYS_COLOR_MIXER_H_
#define UI_COLOR_ENCLAVE_SYS_COLOR_MIXER_H_

#include "third_party/skia/include/core/SkColor.h"
#include "ui/color/color_id.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_recipe.h"
#include "ui/color/color_transform.h"

namespace enclave {

// Core palette.
inline constexpr SkColor kBg = SkColorSetRGB(0x08, 0x0c, 0x10);
inline constexpr SkColor kSurface = SkColorSetRGB(0x0d, 0x11, 0x17);
inline constexpr SkColor kBorder = SkColorSetRGB(0x1c, 0x23, 0x33);
inline constexpr SkColor kAccent = SkColorSetRGB(0x00, 0xd4, 0xff);
inline constexpr SkColor kText = SkColorSetRGB(0xe6, 0xed, 0xf3);
inline constexpr SkColor kMuted = SkColorSetRGB(0x8b, 0x94, 0x9e);

// Status colors.
inline constexpr SkColor kError = SkColorSetRGB(0xef, 0x44, 0x44);
inline constexpr SkColor kPaused = SkColorSetRGB(0x6b, 0x72, 0x80);
inline constexpr SkColor kPurpleLight = SkColorSetRGB(0xa7, 0x8b, 0xfa);

// Tints. Container fills must be opaque, so the palette's rgba values
// are pre-blended over --hm-surface.
inline constexpr SkColor kAccentTint = SkColorSetRGB(0x0c, 0x25, 0x2e);  // 10%
inline constexpr SkColor kErrorTint = SkColorSetRGB(0x1f, 0x15, 0x19);   // 8%
// Overlays keep their alpha: resting 0.10, hover/pressed step +0.10.
inline constexpr SkColor kAccentHover = SkColorSetARGB(0x33, 0x00, 0xd4, 0xff);
inline constexpr SkColor kAccentSelect = SkColorSetARGB(0x1a, 0x00, 0xd4, 0xff);
inline constexpr SkColor kTextHover = SkColorSetARGB(0x1a, 0xe6, 0xed, 0xf3);
inline constexpr SkColor kScrim = SkColorSetARGB(0x99, 0x00, 0x00, 0x00);

inline void AddSysColorOverrides(ui::ColorProvider* provider) {
  ui::ColorMixer& m = provider->AddMixer();

  m[ui::kColorSysPrimary] = {kAccent};
  m[ui::kColorSysOnPrimary] = {kBg};
  m[ui::kColorSysPrimaryContainer] = {kAccentTint};
  m[ui::kColorSysOnPrimaryContainer] = {kAccent};
  m[ui::kColorSysSecondary] = {kMuted};
  m[ui::kColorSysOnSecondary] = {kBg};
  m[ui::kColorSysSecondaryContainer] = {kBorder};
  m[ui::kColorSysOnSecondaryContainer] = {kText};
  m[ui::kColorSysTertiary] = {kPurpleLight};
  m[ui::kColorSysOnTertiary] = {kBg};
  m[ui::kColorSysTertiaryContainer] = {kBorder};
  m[ui::kColorSysOnTertiaryContainer] = {kPurpleLight};
  m[ui::kColorSysError] = {kError};
  m[ui::kColorSysOnError] = {kBg};
  m[ui::kColorSysErrorContainer] = {kErrorTint};
  m[ui::kColorSysOnErrorContainer] = {kError};

  // Page canvas is --hm-bg; anything sitting on it is --hm-surface.
  for (ui::ColorId id :
       {ui::kColorSysSurface, ui::kColorSysSurface1, ui::kColorSysSurface2,
        ui::kColorSysSurface3, ui::kColorSysSurface4, ui::kColorSysSurface5,
        ui::kColorSysSurfaceContainer, ui::kColorSysSurfaceContainerLow,
        ui::kColorSysSurfaceContainerHigh,
        ui::kColorSysSurfaceContainerHighest, ui::kColorSysSurfaceVariant,
        ui::kColorSysSurfaceSection, ui::kColorSysBaseContainer,
        ui::kColorSysBaseContainerElevated, ui::kColorSysNeutralContainer,
        ui::kColorSysHeaderContainer, ui::kColorSysHeaderContainerInactive}) {
    m[id] = {kSurface};
  }
  // Inputs recess into the surface around them (surface vs. bg rule).
  for (ui::ColorId id :
       {ui::kColorSysBase, ui::kColorSysSurfaceContainerLowest,
        ui::kColorSysBaseContainerOnSurface, ui::kColorSysHeader,
        ui::kColorSysHeaderInactive, ui::kColorSysOmniboxContainer}) {
    m[id] = {kBg};
  }
  m[ui::kColorSysTonalContainer] = {kAccentTint};
  m[ui::kColorSysOnTonalContainer] = {kAccent};
  m[ui::kColorSysBaseTonalContainer] = {kAccentTint};
  m[ui::kColorSysOnBaseTonalContainer] = {kAccent};

  m[ui::kColorSysOnSurface] = {kText};
  m[ui::kColorSysOnSurfacePrimary] = {kAccent};
  m[ui::kColorSysOnSurfacePrimaryInactive] = {kMuted};
  m[ui::kColorSysOnSurfaceSecondary] = {kMuted};
  m[ui::kColorSysOnSurfaceSubtle] = {kMuted};
  m[ui::kColorSysOnSurfaceVariant] = {kMuted};
  m[ui::kColorSysSurfaceNumberedForeground] = {kMuted};
  m[ui::kColorSysOnHeaderPrimary] = {kText};
  m[ui::kColorSysOnHeaderPrimaryInactive] = {kMuted};

  for (ui::ColorId id :
       {ui::kColorSysOutline, ui::kColorSysNeutralOutline,
        ui::kColorSysTonalOutline, ui::kColorSysDivider,
        ui::kColorSysOnHeaderDivider, ui::kColorSysOnHeaderDividerInactive}) {
    m[id] = {kBorder};
  }

  m[ui::kColorSysInverseSurface] = {kText};
  m[ui::kColorSysInverseOnSurface] = {kBg};
  m[ui::kColorSysInversePrimary] = {kAccent};
  m[ui::kColorSysInverseSurfacePrimary] = {kText};

  // § 8.1: no shadows; § 14: modal overlay rgba(0, 0, 0, 0.6).
  m[ui::kColorSysShadow] = {SK_ColorTRANSPARENT};
  m[ui::kColorSysStateScrim] = {kScrim};

  m[ui::kColorSysStateFocusRing] = {kAccent};
  m[ui::kColorSysStateFocusRingInverse] = {kBg};
  m[ui::kColorSysStateFocusHighlight] = {kAccentSelect};
  m[ui::kColorSysStateTextHighlight] = {kAccentHover};
  m[ui::kColorSysStateOnTextHighlight] = {kText};
  m[ui::kColorSysStateHover] = {kTextHover};
  m[ui::kColorSysStateHoverOnSubtle] = {kTextHover};
  m[ui::kColorSysStateHoverOnProminent] = {kAccentHover};
  m[ui::kColorSysStateHeaderHover] = {kTextHover};
  m[ui::kColorSysStateHeaderHoverInactive] = {kTextHover};
  m[ui::kColorSysStateHeaderSelect] = {kAccentSelect};
  m[ui::kColorSysStatePressed] = {kAccentHover};
  m[ui::kColorSysStateRipplePrimary] = {kAccentHover};
  m[ui::kColorSysStateRippleNeutralOnSubtle] = {kTextHover};
  m[ui::kColorSysStateRippleNeutralOnProminent] = {kTextHover};
  m[ui::kColorSysStateDisabled] = {kPaused};
  m[ui::kColorSysStateDisabledContainer] = {kBorder};
}

}  // namespace enclave

#endif  // UI_COLOR_ENCLAVE_SYS_COLOR_MIXER_H_
