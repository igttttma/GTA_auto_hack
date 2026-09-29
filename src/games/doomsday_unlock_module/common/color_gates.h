#pragma once

// Single table of all HSV colour gates (cv2 8-bit convention: H 0-179,
// S/V 0-255). Previously scattered as literals across vision.cpp and
// tracking.cpp — tune HDR adaptation HERE only.
//
// HDR tuning rationale (SDR capture path tone-maps HDR content): hue is the
// discriminator — it is exposure-invariant, so hue bands stay primary and
// narrow. S/V floors are coarse filters and are aligned with the direction
// tone mapping shifts values: highlight compression lowers bright emissive
// colours (raise nothing, lower bright V floors), shadow lift raises dark
// values (raise dark ceilings). Where a gate is used as negative evidence
// (accepting more pixels rejects candidates) the value stays conservative.
//
// Whenever a value changes here, apply the same change in src/vision.py and
// src/mirror_tracking.py — the parity suites compare the two live.

#include <cstdint>

namespace col {

struct HsvGate {
  int h_lo, h_hi;  // inclusive hue band
  int s_min;       // 0 = no saturation floor
  int v_min;       // 0 = no value floor
};

// h_lo > h_hi means the band wraps through 0 (red), e.g. {170, 10} covers
// 170-179 plus 0-10.
inline bool InHueBand(const HsvGate& g, std::uint8_t h) {
  return g.h_lo <= g.h_hi ? (h >= g.h_lo && h <= g.h_hi)
                          : (h >= g.h_lo || h <= g.h_hi);
}

inline bool Pass(const HsvGate& g, std::uint8_t h, std::uint8_t s,
                 std::uint8_t v) {
  return InHueBand(g, h) && s >= g.s_min && v >= g.v_min;
}

// --- emissive cyan: packet cores + laser ---
// v floor kept at the original 90: packet chips have a natural breathing dim
// phase (measured core v median dips to ~157; at v=120 dim chips fell below
// the alive gate -> false "destroyed" marks). PCB background stays far below
// (v p99 = 55-73). Cyan is CORROBORATION only — the primary target evidence
// is the colour-free radial-edge metric; red-negative rejects firewalls.
constexpr HsvGate kTargetCyanCore{75, 105, 35, 90};
// Laser-beam mask for occlusion recovery: v 120 keeps the beam core (the
// beam is the brightest thing in the scene). Python is frozen at 120 too —
// keep both equal or hidden-mirror recovery diverges.
constexpr HsvGate kLaserBeam{78, 103, 35, 120};
// Edge-suppression mask (harm gate: a miss pollutes measured mirror edges,
// so prefer wide). s 75->45 for desaturated laser over bright backgrounds.
constexpr HsvGate kLaserHalo{70, 110, 45, 0};

// --- board UI blues / purple anchor ---
constexpr HsvGate kBoardBlueFill{78, 108, 75, 0};   // s 90->75
constexpr HsvGate kBoardBorderBlue{72, 112, 55, 0}; // s 65->55
constexpr HsvGate kPurpleLcd{110, 145, 60, 0};

// Firewall discriminator: red LEDs inside the glass (negative evidence —
// redFrac > 0.3 rejects a target candidate, so the s floor stays
// conservative). Hue band wraps through 0.
constexpr HsvGate kFirewallRed{170, 10, 60, 0};

// --- mirrors ---
constexpr HsvGate kMirrorYellow{15, 45, 60, 60};
// Arrow: also negative evidence (rimYellow <= 0.12 rejects candidates), so
// its strict floors are deliberate — do not loosen without live testing.
constexpr HsvGate kAutoYellowArrow{15, 45, 90, 80};
constexpr HsvGate kSelectionRing{72, 110, 45, 65};  // s 55->45

// --- octagon auto-mirror bright/dark binnings (absolute V) ---
constexpr int kOctoDarkVMax = 100;   // was 80 (shadow lift)
constexpr int kOctoWhiteSMax = 60;
constexpr int kOctoWhiteVMin = 190;  // was 200 (highlight compression; 190
                                     // keeps 2x margin on the 0.08 gate)
constexpr int kMetalSMax = 110;
constexpr int kMetalVMin = 120;
// metal excludes the cyan hue band or a beam cross scores as a metal rim
constexpr int kMetalCyanHueLo = 70;
constexpr int kMetalCyanHueHi = 108;

}  // namespace col
