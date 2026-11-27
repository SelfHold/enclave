// Bezel: browser chrome leaf colors that Enclave's design sets explicitly.
// Everything else follows the kColorSys* overrides in
// ui/color/bezel_sys_color_mixer.h, which also owns the palette.

#ifndef CHROME_BROWSER_UI_COLOR_BEZEL_COLOR_MIXER_H_
#define CHROME_BROWSER_UI_COLOR_BEZEL_COLOR_MIXER_H_

#include "chrome/browser/ui/color/chrome_color_id.h"
#include "ui/color/bezel_sys_color_mixer.h"
#include "ui/color/color_id.h"
#include "ui/color/color_mixer.h"
#include "ui/color/color_provider.h"
#include "ui/color/color_recipe.h"
#include "ui/color/color_transform.h"

namespace bezel {

inline void AddColorMixer(ui::ColorProvider* provider) {
  ui::ColorMixer& m = provider->AddMixer();
  // The window frame is the bezel: the deepest layer.
  m[ui::kColorFrameActive] = {kBg};
  m[ui::kColorFrameInactive] = {kBg};
  m[kColorToolbar] = {kSurface};
  m[kColorToolbarText] = {kText};
  m[kColorToolbarButtonIcon] = {kText};
  m[kColorToolbarContentAreaSeparator] = {kBorder};
  m[kColorBookmarkBarBackground] = {kSurface};
  // Active tab joins the toolbar; inactive tabs sink into the frame.
  m[kColorTabBackgroundActiveFrameActive] = {kSurface};
  m[kColorTabBackgroundActiveFrameInactive] = {kSurface};
  m[kColorTabBackgroundInactiveFrameActive] = {kBg};
  m[kColorTabBackgroundInactiveFrameInactive] = {kBg};
  m[kColorTabForegroundActiveFrameActive] = {kAccent};
  m[kColorTabForegroundInactiveFrameActive] = {kMuted};
  m[kColorOmniboxText] = {kText};
  m[kColorOmniboxResultsBackground] = {kSurface};
  m[kColorNewTabPageBackground] = {kBg};
  m[kColorNewTabPageText] = {kText};
}

}  // namespace bezel

#endif  // CHROME_BROWSER_UI_COLOR_BEZEL_COLOR_MIXER_H_
