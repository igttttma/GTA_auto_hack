#pragma once

// C++ port of src/imgproc.py (pure-numpy image operators for the live
// recognizer). Operator semantics follow the handoff contract: cvRound-style
// round-half-even (std::rint), fixed-point HSV identical to cv2's 8U path,
// L1-gradient aperture-3 Canny, deterministic progressive HoughLinesP, and
// run-based union-find connected components. Scaling is NOT here by design:
// the capture module owns it.

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace imgproc {

struct Mat8 {
  int rows = 0;
  int cols = 0;
  int cn = 1;
  std::vector<std::uint8_t> a;

  Mat8() = default;
  Mat8(int r, int c, int ch = 1)
      : rows(r), cols(c), cn(ch), a(static_cast<std::size_t>(r) * c * ch, 0) {}
  bool Empty() const { return a.empty(); }
  std::uint8_t* Ptr(int y) { return a.data() + static_cast<std::size_t>(y) * cols * cn; }
  const std::uint8_t* Ptr(int y) const {
    return a.data() + static_cast<std::size_t>(y) * cols * cn;
  }
  std::size_t Size() const { return a.size(); }
};

struct Mat16 {
  int rows = 0;
  int cols = 0;
  std::vector<std::int16_t> a;

  Mat16() = default;
  Mat16(int r, int c) : rows(r), cols(c), a(static_cast<std::size_t>(r) * c, 0) {}
  std::int16_t* Ptr(int y) { return a.data() + static_cast<std::size_t>(y) * cols; }
  const std::int16_t* Ptr(int y) const {
    return a.data() + static_cast<std::size_t>(y) * cols;
  }
};

struct Mat32 {
  int rows = 0;
  int cols = 0;
  std::vector<std::int32_t> a;

  Mat32() = default;
  Mat32(int r, int c) : rows(r), cols(c), a(static_cast<std::size_t>(r) * c, 0) {}
  std::int32_t* Ptr(int y) { return a.data() + static_cast<std::size_t>(y) * cols; }
  const std::int32_t* Ptr(int y) const {
    return a.data() + static_cast<std::size_t>(y) * cols;
  }
};

struct Segment {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct Circle {
  float x = 0, y = 0, r = 0;
};

// connectedComponentsWithStats column layout (matches the cv2 usage)
enum CcStat { kCcLeft = 0, kCcTop = 1, kCcWidth = 2, kCcHeight = 3, kCcArea = 4 };

struct CcResult {
  // cv2 retval includes the background label (N components -> N+1)
  int count = 0;
  Mat32 labels;                       // per-pixel labels, 0 = background
  std::vector<std::array<std::int32_t, 5>> stats;  // count rows
  std::vector<std::array<double, 2>> centroids;    // count rows
};

// colour conversion (BGR uint8, H x W x 3)
Mat8 CvtBgraToBgr(const Mat8& img);
Mat8 CvtBgrToGray(const Mat8& img);
Mat8 CvtBgrToHsv(const Mat8& img);

// morphology (binary masks, 0/255)
using Kernel = std::vector<std::uint8_t>;  // k x k row-major
Kernel StructuringElementEllipse(int ksize);
Mat8 Dilate(const Mat8& mask, const Kernel& kernel, int iterations = 1);
Mat8 Erode(const Mat8& mask, const Kernel& kernel, int iterations = 1);

Mat8 GaussianBlur3x3(const Mat8& img);

// cv2.Canny equivalent: L1 gradient, aperture 3. maxClosureIters < 0 runs
// the hysteresis to convergence.
Mat8 Canny(const Mat8& gray, int low, int high, int maxClosureIters = -1);

// Straight-segment extraction over a binary edge mask. Deterministic
// progressive peak-consumption algorithm (NOT segment-identical to cv2).
// Returns false when no segment is found (Python returns None).
// `trace` (optional) receives one line per accumulator peak visited, for
// parity debugging against the Python implementation.
bool HoughLinesP(const Mat8& edges, double rho, double theta, int threshold,
                 int minLineLength, int maxLineGap, std::vector<Segment>* out,
                 std::FILE* trace = nullptr);

// Simplified gradient voting circle detector (F6 init only, not hot path).
// Kasa refinement uses normal equations instead of SVD lstsq (same solution
// mathematically; known non-bit-exact spot, tolerance unchanged).
bool HoughCircles(const Mat8& image, double dp, double minDist, int param1,
                  int param2, int minRadius, int maxRadius, bool binary,
                  std::vector<Circle>* out);

bool ConnectedComponentsWithStats(const Mat8& mask, CcResult* out);

}  // namespace imgproc
