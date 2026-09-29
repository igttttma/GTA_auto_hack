#pragma once

// Board grid geometry, in reference-board pixels (864 x 482 at 1080p).
// Single source of truth shared by the vision snap (SnapMirrorCenter) and the
// level matcher grid; previously duplicated as literals in vision.cpp.

namespace grid {

constexpr double kRefW = 864.0;
constexpr double kRefH = 482.0;
constexpr double kStep = 54.0;
constexpr double kFirstX = 26.5;
constexpr double kFirstY = 23.5;

}  // namespace grid
