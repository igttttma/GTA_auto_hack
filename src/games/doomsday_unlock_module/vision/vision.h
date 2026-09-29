#pragma once

// C++ port of src/vision.py (BoardVision live recognition) and the entity
// model shared with the tracker / observer / overlay. Semantics follow the
// Python reference including its rounding conventions (Python round() =
// round-half-even -> std::rint) and numpy uint8 wrap-around in colour gates.

#include <string>
#include <vector>

#include "imgproc/imgproc.h"

namespace vision {

struct LongEdgePairEvidence {
  bool valid = false;
  double angle_deg = 0;
  double edge_angles[2] = {0, 0};
  double edge_lengths[2] = {0, 0};
  double parallel_error_deg = 0;
  double separation_px = 0;
  double normal_center_error_px = 0;
  double tangent_center_error_px = 0;
  double center_offset_x = 0, center_offset_y = 0;
  double length_ratio = 0;
  double confidence = 0;
};

struct Mirror {
  std::string id;
  double cx = 0, cy = 0;                    // snapped centre
  double radius = 0;
  double anchor_x = 0, anchor_y = 0;        // observation anchor (raw centre)
  double observed_angle_deg = 0;
  double current_angle_deg = 0;             // snapped to 5 deg
  double angle_error_deg = 0;
  double confidence = 0;
  double ring_support = 0;
  bool hidden = false;
  LongEdgePairEvidence pair;
};

struct AutoMirror {
  std::string id;
  double cx = 0, cy = 0, radius = 0;
  double direction_deg = 0;
  double dir_dx = 0, dir_dy = 0;
  double polygon[8][2] = {};
  double confidence = 0;
  double angle_error_deg = 0;
  double rim_metal = 0, dark_core = 0, white_core = 0, yellow_rim = 0;
  double arrow_offset_deg = 0;
};

struct Target {
  std::string id;
  double cx = 0, cy = 0, radius = 0;
  double rect[4] = {0, 0, 0, 0};  // x, y, w, h
  double confidence = 0;
  bool alive = true;
  double core_cyan = 0, ring_yellow = 0, glass = 0;
};

struct GateResult {
  std::string gate;
  std::string status;   // "YES" / "NO" / "UNCERTAIN"
  std::string reason;
  double confidence = 0;
  int rect[4] = {0, 0, 0, 0};
  bool has_rect = false;
};

// Cached minigame anchors for the cheap per-frame liveness gate (same pattern
// as flashing_module's ValidateMinigameGeometry): the two purple LCD readouts
// plus the board rect, filled by FindBoard.  IngameFast sparse-samples these
// instead of re-running the full HSV/connected-components gate.
struct IngameAnchors {
  bool valid = false;
  int rect[4] = {0, 0, 0, 0};
  int lcd[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
};

struct SceneModel {
  int processing_w = 0, processing_h = 0;
  double beam_half_width = 0;
  int playfield[4] = {0, 0, 0, 0};
  std::vector<Mirror> mirrors;
  std::vector<AutoMirror> auto_mirrors;
  std::vector<Target> targets;
};

class BoardVision {
 public:
  GateResult FindBoard(const imgproc::Mat8& frame,
                       IngameAnchors* anchors = nullptr);
  GateResult Ingame(const imgproc::Mat8& frame,
                    IngameAnchors* anchors = nullptr);
  // Cheap liveness gate on cached anchors: sparse-samples the two purple LCD
  // readouts and the board fill.  Microseconds, not the ~135 ms full gate.
  // Optional out: {lcd0 purple frac, lcd1 purple frac, board blue frac}.
  bool IngameFast(const imgproc::Mat8& frame, const IngameAnchors& anchors,
                  double* fracsOut = nullptr) const;
  bool IngameFastBgra(const std::uint32_t* bgra, int w, int h,
                      const IngameAnchors& anchors,
                      double* fracsOut = nullptr) const;
  // Detects mirrors / auto mirrors / targets on `frame` (the processing
  // frame) restricted to board_rect plus the fixed margin; coordinates are
  // full-frame. Optionally fills the annotated evidence image.
  GateResult DetectLiveScene(const imgproc::Mat8& frame,
                             const int board_rect[4], SceneModel* out,
                             imgproc::Mat8* annotated = nullptr);
  // Per-frame liveness at a cached target position, and the init-search
  // acceptance gate, share one geometry-only test: 16 rays from the centre;
  // per ray the largest 1-px grey step within [0.8r, 1.2r]; the weakest ray
  // decides. A real chip shows the glass->PCB boundary on every ray (min is
  // high); a laser beam, the bare PCB or a destroyed socket have rays with
  // no edge at all (min ~0). Colour-free on purpose — survives tone mapping.
  // Threshold in vision.cpp (kTargetEdgeMin).
  double TargetRadialEdgeMetric(const imgproc::Mat8& frame, double cx,
                                double cy, double radius);
  bool TargetPresent(const imgproc::Mat8& frame, const Target& expected,
                     double* metricOut = nullptr);

  // Mirror edge-pair measurement on an arbitrary BGR ROI + its HSV. Used by
  // detection (sub frame) and by the fixed-coordinate tracker (per mirror,
  // matching src/mirror_tracking.py's per-frame read).
  static LongEdgePairEvidence MirrorLongEdgePair(
      const imgproc::Mat8& frame, const imgproc::Mat8& hsv, int cx, int cy,
      int radius, double scale);

 private:
  struct MirrorLayer {
    std::vector<Mirror> mirrors;
    std::vector<AutoMirror> auto_mirrors;
    std::vector<Target> targets;
  };
  MirrorLayer DetectMirrorTargetLayer(const imgproc::Mat8& frame,
                                      const imgproc::Mat8& hsv,
                                      const int board[4], double scale);
  std::vector<Mirror> DetectMirrors(const imgproc::Mat8& frame,
                                    const imgproc::Mat8& hsv,
                                    const int board[4], double scale,
                                    double min_ring_support);
  static std::vector<AutoMirror> DetectAutoMirrors(
      const imgproc::Mat8& frame, const imgproc::Mat8& hsv,
      const int board[4], double scale);
  std::vector<Target> DetectTargets(const imgproc::Mat8& frame,
                                    const imgproc::Mat8& hsv,
                                    const int board[4], double scale);
  std::vector<Mirror> DetectLaserOccludedMirrors(
      const imgproc::Mat8& frame, const imgproc::Mat8& hsv,
      const int board[4], const std::vector<Mirror>& known, double scale);
};

// Shared helpers (also used by the tracker and parity dumps).
double Round5(double angle);

}  // namespace vision
