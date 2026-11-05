#pragma once

namespace kobalt {
// Called on the GUI thread after Nickel has loaded its image plugins.
// Refuse reader startup if a detected Screensaver chain cannot be joined safely.
bool connectScreensaverPowerOff();
} // namespace kobalt
