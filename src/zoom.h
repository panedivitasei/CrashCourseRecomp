// Zoom setting: how close the course camera's picture is when playing alone.

#pragma once

namespace cc1::zoom {

// cc1_zoom runs from -kSteps (twice as far) to kSteps (twice as close).
inline constexpr int kSteps = 5;

// True in a course played alone and off the network, the only place the setting applies; game thread only.
bool Available();

}  // namespace cc1::zoom
