#include "tracking/tracking.h"

#include "common/color_gates.h"
#include "common/processing.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <set>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace tracking {
namespace {

double NormalizeDeg360(double v) {
  double m = std::fmod(v, 360.0);
  if (m < 0) m += 360.0;
  return m;
}

double NormalizeDeg180(double v) {
  double m = std::fmod(v, 180.0);
  if (m < 0) m += 180.0;
  return m;
}

}  // namespace

FixedMirrorAngleTracker::FixedMirrorAngleTracker(
    const std::vector<vision::Mirror>& mirror_model, int processing_width,
    int processing_height)
    : processing_size_(processing_width, processing_height) {
  for (const auto& item : mirror_model) {
    TrackedMirror m;
    m.mirror_id = item.id;
    m.cx = item.cx;
    m.cy = item.cy;
    m.anchor_x = item.anchor_x;
    m.anchor_y = item.anchor_y;
    m.radius = item.radius;
    m.angle_deg = item.observed_angle_deg;
    m.snapped_angle_deg = item.current_angle_deg;
    m.has_angle = true;  // the model always carries an initial measurement
    m.confidence = item.confidence;
    m.evidence = item.pair;
    mirrors_.push_back(m);
  }
  rois_.reserve(mirrors_.size());
  for (const auto& m : mirrors_) rois_.push_back(BuildRoi(m));
}

void FixedMirrorAngleTracker::ObservedBounds(int out[4]) const {
  if (rois_.empty()) {
    out[0] = out[1] = out[2] = out[3] = 0;
    return;
  }
  int x0 = rois_[0].bounds[0], y0 = rois_[0].bounds[1];
  int x1 = rois_[0].bounds[2], y1 = rois_[0].bounds[3];
  for (const auto& roi : rois_) {
    x0 = std::min(x0, roi.bounds[0]);
    y0 = std::min(y0, roi.bounds[1]);
    x1 = std::max(x1, roi.bounds[2]);
    y1 = std::max(y1, roi.bounds[3]);
  }
  out[0] = x0;
  out[1] = y0;
  out[2] = x1 - x0;
  out[3] = y1 - y0;
}

FixedMirrorAngleTracker::MirrorRoi FixedMirrorAngleTracker::BuildRoi(
    const TrackedMirror& mirror) const {
  const int width = processing_size_.first, height = processing_size_.second;
  const int cx = static_cast<int>(std::rint(mirror.anchor_x));
  const int cy = static_cast<int>(std::rint(mirror.anchor_y));
  const int pad = std::max(static_cast<int>(std::rint(mirror.radius + 6)),
                           static_cast<int>(std::rint(mirror.radius * 1.42)));
  const int x0 = std::max(0, cx - pad);
  const int y0 = std::max(0, cy - pad);
  const int x1 = std::min(width, cx + pad + 1);
  const int y1 = std::min(height, cy + pad + 1);
  MirrorRoi roi;
  roi.bounds[0] = x0;
  roi.bounds[1] = y0;
  roi.bounds[2] = x1;
  roi.bounds[3] = y1;
  const double center_x = mirror.cx - x0;
  const double center_y = mirror.cy - y0;
  roi.anchor_x = cx - x0;
  roi.anchor_y = cy - y0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      // ROI-relative pixel offset from the (ROI-relative) centre, exactly
      // like the numpy build: indices are relative to the cropped window
      const double dx = static_cast<double>(x - x0) - center_x;
      const double dy = static_cast<double>(y - y0) - center_y;
      // np.indices(float32) minus a float64 centre promotes to float64, so
      // the distance test is plain double arithmetic on exact values
      const double distance_sq = dx * dx + dy * dy;
      const double lo = mirror.radius * 0.98;
      const double hi = mirror.radius * 1.34;
      if (distance_sq >= lo * lo && distance_sq <= hi * hi) {
        roi.annulus_y.push_back(y - y0);
        roi.annulus_x.push_back(x - x0);
        const double angle = NormalizeDeg360(
            std::atan2(static_cast<double>(y) - y0 - center_y,
                       static_cast<double>(x) - x0 - center_x) *
            180.0 / M_PI);
        roi.annulus_bins.push_back(
            static_cast<std::uint8_t>((static_cast<int>(angle / 5.0)) % 72));
      }
    }
  }
  return roi;
}

FixedMirrorAngleTracker::RingScore FixedMirrorAngleTracker::SelectionRingScore(
    const imgproc::Mat8& roi_hsv, const MirrorRoi& roi) {
  RingScore out;
  if (roi_hsv.a.empty() || roi.annulus_y.empty()) return out;
  const std::size_t nPixels = roi.annulus_y.size();
  std::size_t blueCount = 0;
  bool bins[72] = {false};
  for (std::size_t i = 0; i < nPixels; ++i) {
    const std::size_t idx =
        (static_cast<std::size_t>(roi.annulus_y[i]) * roi_hsv.cols +
         roi.annulus_x[i]) * 3;
    const std::uint8_t h = roi_hsv.a[idx + 0];
    const std::uint8_t s = roi_hsv.a[idx + 1];
    const std::uint8_t v = roi_hsv.a[idx + 2];
    if (col::Pass(col::kSelectionRing, h, s, v)) {
      ++blueCount;
      bins[roi.annulus_bins[i]] = true;
    }
  }
  const double coverage = static_cast<double>(blueCount) / nPixels;
  if (getenv("TRACKER_DUMP_RING")) {
    std::fprintf(stderr, "ring: n=%zu blue=%zu\n", nPixels, blueCount);
  }
  int best;
  int missing = -1;
  for (int b = 0; b < 72; ++b) {
    if (!bins[b]) { missing = b; break; }
  }
  if (missing < 0) {
    best = 72;
  } else {
    best = 0;
    int run = 0;  // longest run of set bins wrapping around
    for (int k = 0; k < 72; ++k) {
      const bool set = bins[(missing + k) % 72];
      if (set) ++run;
      else {
        best = std::max(best, run);
        run = 0;
      }
    }
    best = std::max(best, run);
  }
  const double arcScore = std::min(1.0, best / 48.0);
  const double coverageScore = std::min(1.0, coverage / 0.38);
  out.confidence = std::rint((arcScore * 0.65 + coverageScore * 0.35) * 10000) / 10000.0;
  // experiment: breathing rings sit below the coverage gate on their dim
  // phase, delaying the auto-selected mirror's detection by seconds; a
  // confidence >= 0.5 catch (arc>=24 still implied for typical scores)
  // confirms them on the first frame
  out.is_selected = (best >= 24 && coverage >= 0.18) || out.confidence >= 0.5;
  out.coverage = std::rint(coverage * 10000) / 10000.0;
  out.max_arc_bins = best;
  return out;
}

std::vector<int> FixedMirrorAngleTracker::SelectionScanIndices() const {
  std::set<int> selected;
  if (selected_id_.empty()) {
    for (int i = 0; i < static_cast<int>(mirrors_.size()); ++i) selected.insert(i);
    return {selected.begin(), selected.end()};
  }
  std::unordered_map<std::string, int> byId;
  for (int i = 0; i < static_cast<int>(mirrors_.size()); ++i) {
    byId.emplace(mirrors_[i].mirror_id, i);
  }
  auto it = byId.find(selected_id_);
  if (it != byId.end()) selected.insert(it->second);
  auto pit = byId.find(pending_selected_id_);
  if (pit != byId.end()) selected.insert(pit->second);
  const int rounds = std::min(2, static_cast<int>(mirrors_.size()));
  for (int k = 0; k < rounds; ++k) {
    selected.insert(selection_scan_cursor_ % static_cast<int>(mirrors_.size()));
    ++selection_scan_cursor_;
  }
  return {selected.begin(), selected.end()};
}

void FixedMirrorAngleTracker::UpdateSelected(const std::string& candidate_id) {
  if (candidate_id == selected_id_) {
    pending_selected_id_.clear();
    pending_frames_ = 0;
    selection_missing_frames_ = 0;
    return;
  }
  if (candidate_id.empty()) {
    ++selection_missing_frames_;
    if (selection_missing_frames_ > 2) selected_id_.clear();
    return;
  }
  selection_missing_frames_ = 0;
  if (selected_id_.empty()) {
    selected_id_ = candidate_id;
    pending_selected_id_.clear();
    pending_frames_ = 0;
    return;
  }
  if (candidate_id != pending_selected_id_) {
    pending_selected_id_ = candidate_id;
    pending_frames_ = 1;
    return;
  }
  ++pending_frames_;
  if (pending_frames_ >= 2) {
    selected_id_ = candidate_id;
    pending_selected_id_.clear();
    pending_frames_ = 0;
  }
}

TrackingFrame FixedMirrorAngleTracker::Update(const imgproc::Mat8& frame) {
  const auto started = std::chrono::steady_clock::now();
  const int height = frame.rows, width = frame.cols;
  TrackingFrame out;
  out.mirrors = mirrors_;
  if (width != processing_size_.first || height != processing_size_.second) {
    out.status = "NO";
    out.reason = "PROCESSING_SIZE_CHANGED";
    return out;
  }

  std::vector<std::pair<double, std::string>> selection_candidates;
  const std::vector<int> selection_scan = SelectionScanIndices();
  const bool idle = selected_id_.empty();
  const int idle_index = idle_measure_cursor_ % static_cast<int>(mirrors_.size());
  for (int index = 0; index < static_cast<int>(mirrors_.size()); ++index) {
    TrackedMirror& mirror = out.mirrors[index];
    const MirrorRoi& roi = rois_[index];
    const int x0 = roi.bounds[0], y0 = roi.bounds[1];
    const int x1 = roi.bounds[2], y1 = roi.bounds[3];
    imgproc::Mat8 roiBgr(y1 - y0, x1 - x0, 3);
    for (int y = 0; y < y1 - y0; ++y) {
      std::memcpy(roiBgr.Ptr(y), frame.Ptr(y0 + y) + static_cast<std::size_t>(x0) * 3,
                  static_cast<std::size_t>(x1 - x0) * 3);
    }
    const imgproc::Mat8 roiHsv = imgproc::CvtBgrToHsv(roiBgr);
    bool currentIsSelected = false;
    const bool inScan =
        std::find(selection_scan.begin(), selection_scan.end(), index) !=
        selection_scan.end();
    if (inScan) {
      const RingScore selection = SelectionRingScore(roiHsv, roi);
      mirror.selection_confidence = selection.confidence;
      mirror.selection_arc_bins = selection.max_arc_bins;
      if (selection.is_selected) {
        selection_candidates.push_back({selection.confidence, mirror.mirror_id});
        currentIsSelected = true;
      }
    }
    bool shouldMeasureAngle =
        (mirror.mirror_id == selected_id_) || currentIsSelected;
    if (idle && !shouldMeasureAngle) {
      shouldMeasureAngle = !mirror.has_angle || mirror.misses > 0 ||
                           index == idle_index;
    }
    if (!shouldMeasureAngle) continue;
    // Live angle measurement = the same edge-pair read Python uses
    // (BoardVision._mirror_long_edge_pair on the fixed ROI): Canny(42,132)
    // edges inside 0.94R, cyan laser halo suppressed via dilated hue mask,
    // HoughLinesP with radius-derived vote/gap, then the scored parallel
    // pair acceptance gates. The in-house bar-contrast replacement
    // (MirrorBarAngle) was abandoned on 2026-09-26 -- Python is the
    // authority again.
    const vision::LongEdgePairEvidence pair = vision::BoardVision::MirrorLongEdgePair(
        roiBgr, roiHsv, roi.anchor_x, roi.anchor_y,
        static_cast<int>(std::rint(mirror.radius)),
        static_cast<double>(height) / 1080.0);
    if (!pair.valid) {
      ++mirror.misses;
      mirror.confidence = 0.0;
      continue;
    }
    mirror.angle_deg = pair.angle_deg;
    mirror.snapped_angle_deg = vision::Round5(pair.angle_deg);
    mirror.confidence = pair.confidence;
    mirror.evidence = pair;
    mirror.misses = 0;
    mirror.has_angle = true;
  }

  if (idle) ++idle_measure_cursor_;
  std::string candidateId;
  double bestConf = 0;
  for (const auto& cand : selection_candidates) {
    if (candidateId.empty() || cand.first > bestConf) {
      bestConf = cand.first;
      candidateId = cand.second;
    }
  }
  UpdateSelected(candidateId);
  int detected = 0;
  for (auto& mirror : out.mirrors) {
    mirror.selected = mirror.mirror_id == selected_id_;
    if (mirror.has_angle && mirror.misses == 0) ++detected;
  }
  out.detected_count = detected;
  out.processing_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
          .count();
  if (detected == static_cast<int>(out.mirrors.size())) {
    out.status = "YES";
    out.reason = "ALL_MIRROR_ANGLES_OBSERVED";
  } else if (detected > 0) {
    out.status = "DEGRADED";
    out.reason = "SOME_MIRROR_ANGLES_MISSING";
  } else {
    out.status = "NO";
    out.reason = "ALL_MIRROR_ANGLES_MISSING";
  }
  mirrors_ = out.mirrors;
  return out;
}

}  // namespace tracking
