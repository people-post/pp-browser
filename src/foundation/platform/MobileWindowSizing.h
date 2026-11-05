#pragma once

namespace pbr {

void ResolveMobileWindowSize(int& width, int& height);

/** Refresh rate of the primary display in Hz (SDL current display mode); 0 when unknown. */
float PrimaryDisplayRefreshHz();

} // namespace pbr
