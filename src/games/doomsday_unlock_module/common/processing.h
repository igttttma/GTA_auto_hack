#pragma once

// Single definition of the processing-frame scale metric.
//
// The capture module owns downscaling (client taller than 1080 -> 1080; see
// game_window.cpp / dxgi_capture.cpp — public code, keep their copies). Every
// consumer outside capture derives pixel thresholds from the processing frame
// height via ProcessingScale, and the observer re-derives capture's
// client-side factor via ClientCaptureScale. The minigame panel is natively
// 16:9 with its height pinned to the client height (narrower clients crop the
// panel sides, wider ones letterbox), so height-normalized geometry is valid
// at any client aspect ratio.

namespace geom {

constexpr double kRefHeight = 1080.0;

// Processing-frame px per reference-1080 px. <= 1 by construction.
inline double ProcessingScale(int processing_height) {
  return processing_height / kRefHeight;
}

// Client -> processing factor as applied by the capture module's downscale
// policy (mirrored here; capture itself is the authority).
inline double ClientCaptureScale(int client_height) {
  return client_height > kRefHeight ? kRefHeight / client_height : 1.0;
}

}  // namespace geom
