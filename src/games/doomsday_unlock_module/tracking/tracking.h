#pragma once

// C++ port of src/mirror_tracking.py (FixedMirrorAngleTracker): measure
// mirror edge pairs at immutable coordinates from one global scan, with the
// selection-ring detector for the currently selected mirror.

#include <string>
#include <vector>

#include "imgproc/imgproc.h"
#include "vision/vision.h"

namespace tracking {

struct TrackedMirror {
  std::string mirror_id;
  double cx = 0, cy = 0;                     // fixed centre (processing px)
  double anchor_x = 0, anchor_y = 0;         // fixed observation anchor
  double radius = 0;
  bool has_angle = false;
  double angle_deg = 0;
  double snapped_angle_deg = 0;
  double confidence = 0;
  int misses = 0;
  vision::LongEdgePairEvidence evidence;
  bool selected = false;
  double selection_confidence = 0;
  int selection_arc_bins = 0;
};

struct TrackingFrame {
  std::string status;   // YES / DEGRADED / NO
  std::string reason;
  std::vector<TrackedMirror> mirrors;
  double processing_ms = 0;
  int detected_count = 0;
};

class FixedMirrorAngleTracker {
 public:
  FixedMirrorAngleTracker(const std::vector<vision::Mirror>& mirror_model,
                          int processing_width, int processing_height);

  // Union of every ROI this tracker reads, in processing pixels
  // (x0, y0, width, height).
  void ObservedBounds(int out[4]) const;

  TrackingFrame Update(const imgproc::Mat8& frame);

  const std::string& selected_id() const { return selected_id_; }

 private:
  struct MirrorRoi {
    int bounds[4] = {0, 0, 0, 0};  // x0, y0, x1, y1 (frame coords)
    double anchor_x = 0, anchor_y = 0;
    std::vector<int> annulus_y, annulus_x;
    std::vector<std::uint8_t> annulus_bins;
  };

  MirrorRoi BuildRoi(const TrackedMirror& mirror) const;
  std::vector<int> SelectionScanIndices() const;
  struct RingScore {
    bool is_selected = false;
    double confidence = 0;
    double coverage = 0;
    int max_arc_bins = 0;
  };
  static RingScore SelectionRingScore(const imgproc::Mat8& roi_hsv,
                                      const MirrorRoi& roi);
  void UpdateSelected(const std::string& candidate_id);

  std::pair<int, int> processing_size_;
  std::vector<TrackedMirror> mirrors_;
  std::vector<MirrorRoi> rois_;
  std::string selected_id_;
  std::string pending_selected_id_;
  int pending_frames_ = 0;
  int selection_missing_frames_ = 0;
  mutable int selection_scan_cursor_ = 0;
  int idle_measure_cursor_ = 0;
};

}  // namespace tracking
