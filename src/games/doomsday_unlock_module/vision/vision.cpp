#include "vision/vision.h"

#include "common/board_grid.h"
#include "common/color_gates.h"
#include "common/processing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vision {
namespace {

constexpr double kEdgePairVoteFraction = 0.25;
constexpr double kEdgePairGapFraction = 0.50;
constexpr double kEdgePairParallelToleranceDeg = 3.5;
// Target radial-edge acceptance (init search + per-frame liveness). Measured
// on 1080p captures: real chips (all rays see the boundary) >= 17; laser
// emitter/beam, bare PCB and destroyed sockets <= 1.
constexpr double kTargetEdgeMin = 10.0;
// Occluded-target special case: the top-left help tooltip can cover the top
// of a target, leaving only its bottom part.  Acceptance then falls back to
// the longest contiguous run of strong rays: it must span at least this many
// of the kTargetRays (60%), include the downward ray, and not be the full
// circle (otherwise the normal gate would already have passed).
constexpr int kTargetRays = 16;
constexpr int kMinStrongRun = 10;
// Cyan-core fraction, counted over whatever wedge is visible.
constexpr double kCoreCyanMin = 0.18;

void TargetRaySteps(const imgproc::Mat8& frame, double cx, double cy,
                    double radius, double* steps, double* tbest);
double TargetRayAngle(int k);
bool OccludedBottomGeometry(const imgproc::Mat8& frame, double cx, double cy,
                            double radius, int* runStart, int* runLen,
                            double* fitCx, double* fitCy, double* fitR);

// Python round() == round-half-even; numpy rint identical. All Python
// `round(x)` / `round(x, n)` sites in vision.py map to these.
inline int PyRound(double v) { return static_cast<int>(std::rint(v)); }
inline double PyRoundN(double v, int digits) {
  const double f = std::pow(10.0, digits);
  return std::rint(v * f) / f;
}

// np.percentile linear interpolation
double Percentile(std::vector<std::uint8_t> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double n = static_cast<double>(values.size());
  const double idx = (n - 1) * p / 100.0;
  const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
  const std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
  const double frac = idx - std::floor(idx);
  const double v = values[lo] + (values[hi] - values[lo]) * frac;
  return v;
}

double Median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const std::size_t n = values.size();
  if (n % 2 == 1) return values[n / 2];
  return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

inline double PyMod(double v, double m) {
  // Python % semantics: result is non-negative for m > 0 (C fmod is not)
  const double r = std::fmod(v, m);
  return r < 0 ? r + m : r;
}

inline double LineAngleDelta(double a, double b) {
  return std::abs(PyMod(a - b + 90.0, 180.0) - 90.0);
}

double NormalizeDeg180(double v);

double AxialMean(double a, double b, double weightA, double weightB) {
  const double ar = a * 2.0 * M_PI / 180.0;
  const double br = b * 2.0 * M_PI / 180.0;
  const double x = std::cos(ar) * weightA + std::cos(br) * weightB;
  const double y = std::sin(ar) * weightA + std::sin(br) * weightB;
  return NormalizeDeg180(std::atan2(y, x) * 180.0 / M_PI / 2.0);
}

double NormalizeDeg180(double v) {
  double m = std::fmod(v, 180.0);
  if (m < 0) m += 180.0;
  return m;
}

double NormalizeDeg360(double v) {
  double m = std::fmod(v, 360.0);
  if (m < 0) m += 360.0;
  return m;
}

// HSV channels of a BGR image (3-channel), split for gate reads.
struct HsvImage {
  int rows = 0, cols = 0;
  std::vector<std::uint8_t> h, s, v;
};
HsvImage SplitHsv(const imgproc::Mat8& hsv) {
  HsvImage out;
  out.rows = hsv.rows;
  out.cols = hsv.cols;
  out.h.resize(hsv.rows * hsv.cols);
  out.s.resize(hsv.rows * hsv.cols);
  out.v.resize(hsv.rows * hsv.cols);
  for (std::size_t i = 0; i < out.h.size(); ++i) {
    out.h[i] = hsv.a[3 * i + 0];
    out.s[i] = hsv.a[3 * i + 1];
    out.v[i] = hsv.a[3 * i + 2];
  }
  return out;
}

struct SnapResult {
  double x, y;
  bool snapped;  // false -> kept as measured (beyond 0.45 steps)
};

SnapResult SnapMirrorCenter(double cx, double cy, const int board[4]) {
  const double bx = board[0], by = board[1], bw = board[2], bh = board[3];
  const double stepX = grid::kStep * bw / grid::kRefW;
  const double stepY = grid::kStep * bh / grid::kRefH;
  const double firstX = bx + grid::kFirstX * bw / grid::kRefW;
  const double firstY = by + grid::kFirstY * bh / grid::kRefH;
  // Snap to the HALF-step grid: some authored mirrors sit on half nodes
  // (level10 M06 at step y≈4.5). The old integer-node snap with a 0.45-step
  // tolerance swallowed such mirrors whole (measured 4.44 -> node 4, half a
  // node off). Half-node quantisation + a tight 0.2-full-step tolerance
  // normalises positions while never moving a mirror more than a few px.
  const double qx = stepX * 0.5, qy = stepY * 0.5;
  const double nx = firstX + std::rint((cx - firstX) / qx) * qx;
  const double ny = firstY + std::rint((cy - firstY) / qy) * qy;
  if (std::abs(cx - nx) > 0.2 * stepX || std::abs(cy - ny) > 0.2 * stepY) {
    return {PyRoundN(cx, 1), PyRoundN(cy, 1), false};
  }
  return {PyRoundN(nx, 3), PyRoundN(ny, 3), true};
}

}  // namespace

double Round5(double angle) {
  return std::fmod(std::rint(angle / 5.0) * 5.0, 180.0);
}

GateResult BoardVision::FindBoard(const imgproc::Mat8& frame,
                                  IngameAnchors* anchors) {
  const int height = frame.rows, width = frame.cols;
  const imgproc::Mat8 hsv = imgproc::CvtBgrToHsv(frame);
  const HsvImage ch = SplitHsv(hsv);
  const auto n = static_cast<std::size_t>(height) * width;

  // Primary: anchor on the two purple LCD readouts.
  bool anchored = false;
  int rect[4] = {0, 0, 0, 0};
  {
    imgproc::Mat8 purple(height, width, 1);
    for (std::size_t i = 0; i < n; ++i) {
      purple.a[i] = col::Pass(col::kPurpleLcd, ch.h[i], ch.s[i], ch.v[i]) ? 255 : 0;
    }
    imgproc::CcResult cc;
    imgproc::ConnectedComponentsWithStats(purple, &cc);
    const double minArea = 0.0015 * height * height;
    struct Comp { int area, x, y, w, h; };
    std::vector<Comp> comps;
    for (int i = 1; i < cc.count; ++i) {
      const int x = cc.stats[i][imgproc::kCcLeft];
      const int y = cc.stats[i][imgproc::kCcTop];
      const int w = cc.stats[i][imgproc::kCcWidth];
      const int hgt = cc.stats[i][imgproc::kCcHeight];
      const int area = cc.stats[i][imgproc::kCcArea];
      if (area < minArea) continue;
      const double rh = static_cast<double>(hgt) / height;
      const double rw = static_cast<double>(w) / height;
      if (!(0.055 <= rh && rh <= 0.11 && 0.09 <= rw && rw <= 0.26)) continue;
      comps.push_back({area, x, y, w, hgt});
    }
    if (comps.size() >= 2) {
      // Python: comps.sort(reverse=True) on (area, x, y, w, hgt) tuples
      std::sort(comps.begin(), comps.end(), [](const Comp& a, const Comp& b) {
        if (a.area != b.area) return a.area > b.area;
        if (a.x != b.x) return a.x > b.x;
        if (a.y != b.y) return a.y > b.y;
        if (a.w != b.w) return a.w > b.w;
        return a.h > b.h;
      });
      const Comp& c1 = comps[0];
      const Comp& c2 = comps[1];
      if (std::abs(c1.y - c2.y) <= 0.012 * height) {
        const int ux = std::min(c1.x, c2.x);
        const int uy = std::min(c1.y, c2.y);
        const int unionW = std::max(c1.x + c1.w, c2.x + c2.w) - ux;
        const double uw = static_cast<double>(unionW) / height;
        if (0.33 <= uw && uw <= 0.46) {
          const int x = PyRound(ux + (-0.85616) * height);
          const int y = PyRound(uy + 0.12060 * height);
          const int bw = PyRound(1.20 * height);
          const int bh = PyRound(0.67 * height);
          if (x >= 0 && y >= 0 && x + bw <= width && y + bh <= height) {
            rect[0] = x; rect[1] = y; rect[2] = bw; rect[3] = bh;
            anchored = true;
            if (anchors != nullptr) {
              anchors->lcd[0][0] = c1.x; anchors->lcd[0][1] = c1.y;
              anchors->lcd[0][2] = c1.w; anchors->lcd[0][3] = c1.h;
              anchors->lcd[1][0] = c2.x; anchors->lcd[1][1] = c2.y;
              anchors->lcd[1][2] = c2.w; anchors->lcd[1][3] = c2.h;
            }
          }
        }
      }
    }
  }

  const int boardW = PyRound(height * 1.20);
  const int boardH = PyRound(height * 0.67);
  if (anchored) {
    const int px = PyRound(static_cast<double>(width - boardW) / 2);
    const int py = PyRound(height * 0.175);
    if (std::abs(rect[0] - px) <= 0.005 * height) rect[0] = px;
    if (std::abs(rect[1] - py) <= 0.005 * height) rect[1] = py;
  } else {
    const int x = PyRound(static_cast<double>(width - boardW) / 2);
    const int y = PyRound(height * 0.175);
    if (x < 0 || y < 0 || x + boardW > width || y + boardH > height) {
      GateResult g;
      g.gate = "BOARD"; g.status = "NO"; g.reason = "CLIENT_ASPECT_UNSUPPORTED";
      return g;
    }
    rect[0] = x; rect[1] = y; rect[2] = boardW; rect[3] = boardH;
  }
  const int x = rect[0], y = rect[1], w = rect[2], hgt = rect[3];

  // appearance evidence on the HSV patch
  imgproc::Mat8 patch(hgt, w, 3);
  for (int yy = 0; yy < hgt; ++yy) {
    std::memcpy(patch.Ptr(yy), hsv.Ptr(y + yy) + static_cast<std::size_t>(x) * 3,
                static_cast<std::size_t>(w) * 3);
  }
  const HsvImage p = SplitHsv(patch);
  const std::size_t pn = p.h.size();
  std::vector<std::uint8_t> patchV(p.v), frameV(ch.v);
  const double p76 = Percentile(patchV, 76);
  const double v55 = Percentile(frameV, 55);
  std::size_t blueCount = 0;
  for (std::size_t i = 0; i < pn; ++i) {
    if (col::Pass(col::kBoardBlueFill, p.h[i], p.s[i], p.v[i]) && p.v[i] <= p76) ++blueCount;
  }
  const double blueFraction = static_cast<double>(blueCount) / pn;

  const int border = std::max(3, PyRound(height * 0.008));
  // strips: top border rows, bottom border rows, left cols, right cols
  std::vector<std::uint8_t> sh, ss_;
  auto addStrip = [&](int ry0, int ry1, int rx0, int rx1) {
    for (int yy = ry0; yy < ry1; ++yy) {
      for (int xx = rx0; xx < rx1; ++xx) {
        sh.push_back(p.h[yy * w + xx]);
        ss_.push_back(p.s[yy * w + xx]);
      }
    }
  };
  addStrip(0, border, 0, w);
  addStrip(hgt - border, hgt, 0, w);
  addStrip(0, hgt, 0, border);
  addStrip(0, hgt, w - border, w);
  std::size_t borderBlue = 0;
  for (std::size_t i = 0; i < sh.size(); ++i) {
    if (col::Pass(col::kBoardBorderBlue, sh[i], ss_[i], 255)) ++borderBlue;
  }
  const double borderBlueFraction = static_cast<double>(borderBlue) / sh.size();

  std::size_t darkCount = 0;
  for (std::size_t i = 0; i < pn; ++i) {
    if (p.v[i] <= v55) ++darkCount;
  }
  const double darkFraction = static_cast<double>(darkCount) / pn;

  const double score =
      std::min(1.0, blueFraction / 0.48) * 0.65 +
      std::min(1.0, borderBlueFraction / 0.48) * 0.25 +
      std::min(1.0, darkFraction / 0.55) * 0.10;

  GateResult g;
  g.gate = "BOARD";
  if (score < 0.58) {
    g.status = "NO"; g.reason = "PLAYFIELD_APPEARANCE_MISMATCH";
    g.confidence = score;
    return g;
  }
  if (score < 0.70) {
    g.status = "UNCERTAIN"; g.reason = "BOARD_CONFIDENCE_LOW";
    g.confidence = score;
    return g;
  }
  g.status = "YES"; g.reason = "BOARD_FOUND";
  g.confidence = std::min(1.0, score / 0.55);
  g.rect[0] = x; g.rect[1] = y; g.rect[2] = boardW; g.rect[3] = boardH;
  g.has_rect = true;
  if (anchors != nullptr) {
    anchors->valid = anchored;
    std::copy(g.rect, g.rect + 4, anchors->rect);
  }
  return g;
}

GateResult BoardVision::Ingame(const imgproc::Mat8& frame,
                               IngameAnchors* anchors) {
  GateResult board = FindBoard(frame, anchors);
  GateResult g;
  g.gate = "INGAME";
  if (board.status != "YES") {
    g.status = board.status == "NO" ? "NO" : "UNCERTAIN";
    g.reason = board.reason;
    g.confidence = board.confidence;
    return g;
  }
  const int x = board.rect[0], y = board.rect[1], w = board.rect[2], h = board.rect[3];
  // Height term: the found board must match the height model (boardH =
  // 0.67*clientH). Unlike the old board-area-over-client-area check (0.42,
  // a 16:9 fit), this is aspect-independent: the panel is natively 16:9 with
  // its height pinned to the client height (sides crop/letterbox). At 16:9
  // both terms cap at exactly 1.0, so behavior is bit-identical; Python keeps
  // the old area gate (see handoff).
  const double heightTerm =
      std::min(1.0, (static_cast<double>(h) / frame.rows) / 0.67);
  const double aspect = static_cast<double>(w) / h;
  const double confidence = std::min(
      board.confidence,
      std::min(heightTerm,
               1.0 - std::min(std::abs(aspect - 1.78) / 0.8, 0.7)));
  if (confidence < 0.62) {
    g.status = "UNCERTAIN"; g.reason = "PLAYFIELD_GEOMETRY_UNCERTAIN";
    g.confidence = confidence;
    g.has_rect = true;
    std::copy(board.rect, board.rect + 4, g.rect);
    return g;
  }
  g.status = "YES"; g.reason = "HACKING_PLAYFIELD_PRESENT";
  g.confidence = confidence;
  g.has_rect = true;
  std::copy(board.rect, board.rect + 4, g.rect);
  return g;
}

namespace {

// Scalar twin of imgproc::CvtBgrToHsv (cv2 8-bit convention) for sparse
// sampling: converting a few thousand pixels beats converting the frame.
void BgrToHsvScalar(int b, int g, int r, std::uint8_t* h, std::uint8_t* s,
                    std::uint8_t* v) {
  const int vmax = std::max(std::max(b, g), r);
  const int vmin = std::min(std::min(b, g), r);
  const int diff = vmax - vmin;
  *v = static_cast<std::uint8_t>(vmax);
  *s = vmax == 0 ? 0
                 : static_cast<std::uint8_t>(std::rint(255.0 * diff / vmax));
  int hpre;
  if (diff == 0) {
    hpre = 0;
  } else if (vmax == r) {
    hpre = g - b;
  } else if (vmax == g) {
    hpre = b - r + 2 * diff;
  } else {
    hpre = r - g + 4 * diff;
  }
  int hh = static_cast<int>(std::rint(30.0 * hpre / diff));
  if (diff == 0) hh = 0;
  if (hh < 0) hh += 180;
  *h = static_cast<std::uint8_t>(hh);
}

// Sparse anchor validation (flashing_module's ValidateMinigameGeometry
// pattern): both purple LCD readouts must still read purple on a coarse grid
// and the board fill must still read board-blue.  A few thousand pixel reads.
constexpr double kLcdPurpleMin = 0.35;
constexpr double kBoardBlueMin = 0.20;

template <typename Get>  // Get(x, y) -> packed 0xBBGGRR
bool IngameFastSample(int w, int h, const IngameAnchors& anchors, Get get,
                      double* fracsOut) {
  if (!anchors.valid) return false;
  double fracs[3] = {0, 0, 0};
  for (int lcd = 0; lcd < 2; ++lcd) {
    const int lx = std::clamp(anchors.lcd[lcd][0], 0, w);
    const int ly = std::clamp(anchors.lcd[lcd][1], 0, h);
    const int lx1 = std::clamp(anchors.lcd[lcd][0] + anchors.lcd[lcd][2], 0, w);
    const int ly1 = std::clamp(anchors.lcd[lcd][1] + anchors.lcd[lcd][3], 0, h);
    if (lx1 <= lx || ly1 <= ly) return false;
    const int step = std::max(1, std::min(lx1 - lx, ly1 - ly) / 8);
    int hit = 0, total = 0;
    for (int y = ly; y < ly1; y += step) {
      for (int x = lx; x < lx1; x += step) {
        const std::uint32_t p = get(x, y);
        std::uint8_t hh, ss, vv;
        BgrToHsvScalar(p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, &hh, &ss,
                       &vv);
        if (col::Pass(col::kPurpleLcd, hh, ss, vv)) ++hit;
        ++total;
      }
    }
    fracs[lcd] = total ? static_cast<double>(hit) / total : 0.0;
    if (fracs[lcd] < kLcdPurpleMin) {
      if (fracsOut) std::copy(fracs, fracs + 3, fracsOut);
      return false;
    }
  }
  const int bx = std::clamp(anchors.rect[0], 0, w);
  const int by = std::clamp(anchors.rect[1], 0, h);
  const int bx1 = std::clamp(anchors.rect[0] + anchors.rect[2], 0, w);
  const int by1 = std::clamp(anchors.rect[1] + anchors.rect[3], 0, h);
  if (bx1 <= bx || by1 <= by) return false;
  const int bstep = std::max(4, anchors.rect[2] / 48);
  int hit = 0, total = 0;
  for (int y = by; y < by1; y += bstep) {
    for (int x = bx; x < bx1; x += bstep) {
      const std::uint32_t p = get(x, y);
      std::uint8_t hh, ss, vv;
      BgrToHsvScalar(p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, &hh, &ss,
                     &vv);
      if (col::Pass(col::kBoardBlueFill, hh, ss, vv)) ++hit;
      ++total;
    }
  }
  fracs[2] = total ? static_cast<double>(hit) / total : 0.0;
  if (fracsOut) std::copy(fracs, fracs + 3, fracsOut);
  return fracs[2] >= kBoardBlueMin;
}

}  // namespace

bool BoardVision::IngameFast(const imgproc::Mat8& frame,
                             const IngameAnchors& anchors,
                             double* fracsOut) const {
  return IngameFastSample(
      frame.cols, frame.rows, anchors,
      [&](int x, int y) {
        const std::uint8_t* p = frame.Ptr(y) + static_cast<std::size_t>(x) * 3;
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16);
      },
      fracsOut);
}

bool BoardVision::IngameFastBgra(const std::uint32_t* bgra, int w, int h,
                                 const IngameAnchors& anchors,
                                 double* fracsOut) const {
  return IngameFastSample(
      w, h, anchors,
      [&](int x, int y) {
        return bgra[static_cast<std::size_t>(y) * w + x];
      },
      fracsOut);
}

LongEdgePairEvidence BoardVision::MirrorLongEdgePair(
    const imgproc::Mat8& frame, const imgproc::Mat8& hsv, int cx, int cy,
    int radius, double scale) {
  const int pad = std::max(radius + 3, PyRound(radius * 1.16));
  const int x0 = std::max(0, cx - pad);
  const int y0 = std::max(0, cy - pad);
  const int x1 = std::min(frame.cols, cx + pad + 1);
  const int y1 = std::min(frame.rows, cy + pad + 1);
  if (x1 <= x0 || y1 <= y0) return {};
  imgproc::Mat8 crop(y1 - y0, x1 - x0, 3);
  for (int y = 0; y < y1 - y0; ++y) {
    std::memcpy(crop.Ptr(y), frame.Ptr(y0 + y) + static_cast<std::size_t>(x0) * 3,
                static_cast<std::size_t>(x1 - x0) * 3);
  }
  const imgproc::Mat8 gray = imgproc::CvtBgrToGray(crop);
  const int localCx = cx - x0, localCy = cy - y0;

  imgproc::Mat8 edges = imgproc::Canny(gray, 42, 132);
  const HsvImage lh = SplitHsv(hsv);
  for (int y = 0; y < y1 - y0; ++y) {
    for (int x = 0; x < x1 - x0; ++x) {
      const double dx = x - localCx, dy = y - localCy;
      if (std::sqrt(dx * dx + dy * dy) > radius * 0.94) {
        edges.Ptr(y)[x] = 0;
        continue;
      }
      const std::size_t idx = static_cast<std::size_t>(y0 + y) * hsv.cols + (x0 + x);
      const std::uint8_t h = lh.h[idx], s = lh.s[idx];
      // cyan laser halo (kLaserHalo): suppress its pixels from the edge map
      if (col::Pass(col::kLaserHalo, h, s, lh.v[idx])) edges.Ptr(y)[x] = 0;
    }
  }
  const int kernelSize = std::max(3, PyRound(5 * scale)) | 1;
  const imgproc::Kernel laserKernel =
      imgproc::StructuringElementEllipse(kernelSize);
  imgproc::Mat8 cyanMask(y1 - y0, x1 - x0, 1);
  for (int y = 0; y < y1 - y0; ++y) {
    for (int x = 0; x < x1 - x0; ++x) {
      const std::size_t idx = static_cast<std::size_t>(y0 + y) * hsv.cols + (x0 + x);
      const std::uint8_t h = lh.h[idx], s = lh.s[idx];
      cyanMask.Ptr(y)[x] = col::Pass(col::kLaserHalo, h, s, lh.v[idx]) ? 255 : 0;
    }
  }
  const imgproc::Mat8 cyanDilated = imgproc::Dilate(cyanMask, laserKernel, 1);
  for (std::size_t i = 0; i < cyanDilated.a.size(); ++i) {
    if (cyanDilated.a[i] > 0) edges.a[i] = 0;
  }

  std::vector<imgproc::Segment> lines;
  const int threshold = std::max(6, PyRound(radius * kEdgePairVoteFraction));
  const int minLength = std::max(14, PyRound(radius * 0.72));
  const int maxGap = std::max(4, PyRound(radius * kEdgePairGapFraction));
  if (!imgproc::HoughLinesP(edges, 1.0, M_PI / 360.0, threshold, minLength,
                            maxGap, &lines)) {
    return {};
  }

  struct Seg { double angle, length, mx, my; };
  std::vector<Seg> segments;
  for (const auto& line : lines) {
    const double dx = static_cast<double>(line.x2 - line.x1);
    const double dy = static_cast<double>(line.y2 - line.y1);
    const double length = std::hypot(dx, dy);
    if (length < radius * 0.72) continue;
    Seg seg;
    seg.angle = NormalizeDeg180(std::atan2(-dy, dx) * 180.0 / M_PI);
    seg.length = length;
    seg.mx = (line.x1 + line.x2) / 2.0 - localCx;
    seg.my = (line.y1 + line.y2) / 2.0 - localCy;
    segments.push_back(seg);
  }

  bool hasBest = false;
  double bestScore = 0;
  LongEdgePairEvidence best;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    for (std::size_t j = i + 1; j < segments.size(); ++j) {
      const Seg& first = segments[i];
      const Seg& second = segments[j];
      const double parallelError = LineAngleDelta(first.angle, second.angle);
      if (parallelError > kEdgePairParallelToleranceDeg) continue;
      const double angle = AxialMean(first.angle, second.angle,
                                     first.length, second.length);
      const double radians = angle * M_PI / 180.0;
      const double tangent[2] = {std::cos(radians), -std::sin(radians)};
      const double normal[2] = {-tangent[1], tangent[0]};
      const double n1 = first.mx * normal[0] + first.my * normal[1];
      const double n2 = second.mx * normal[0] + second.my * normal[1];
      const double t1 = first.mx * tangent[0] + first.my * tangent[1];
      const double t2 = second.mx * tangent[0] + second.my * tangent[1];
      const double separation = std::abs(n1 - n2);
      const double normalCenterError = std::abs((n1 + n2) / 2.0);
      const double tangentCenterError = std::abs((t1 + t2) / 2.0);
      const double lengthRatio =
          std::min(first.length, second.length) /
          std::max(first.length, second.length);

      if (!(3.0 * scale <= separation && separation <= 15.0 * scale)) continue;
      if (normalCenterError > 12.0 * scale || tangentCenterError > 12.0 * scale) continue;
      if (lengthRatio < 0.50) continue;

      const double lengthScore =
          std::min(1.0, std::min(first.length, second.length) / (radius * 1.45));
      const double parallelScore =
          std::max(0.0, 1.0 - parallelError / kEdgePairParallelToleranceDeg);
      const double symmetryScore =
          std::max(0.0, 1.0 - normalCenterError / (4.5 * scale));
      const double score = lengthScore * 0.42 + parallelScore * 0.30 +
                           symmetryScore * 0.18 + lengthRatio * 0.10;

      LongEdgePairEvidence ev;
      ev.valid = true;
      ev.angle_deg = PyRoundN(angle, 4);
      ev.edge_angles[0] = PyRoundN(first.angle, 4);
      ev.edge_angles[1] = PyRoundN(second.angle, 4);
      ev.edge_lengths[0] = PyRoundN(first.length, 3);
      ev.edge_lengths[1] = PyRoundN(second.length, 3);
      ev.parallel_error_deg = PyRoundN(parallelError, 4);
      ev.separation_px = PyRoundN(separation, 3);
      ev.normal_center_error_px = PyRoundN(normalCenterError, 3);
      ev.tangent_center_error_px = PyRoundN(tangentCenterError, 3);
      ev.center_offset_x = PyRoundN((first.mx + second.mx) / 2.0, 3);
      ev.center_offset_y = PyRoundN((first.my + second.my) / 2.0, 3);
      ev.length_ratio = PyRoundN(lengthRatio, 4);
      ev.confidence = PyRoundN(score, 4);
      if (!hasBest || score > bestScore) {
        hasBest = true;
        bestScore = score;
        best = ev;
      }
    }
  }
  return best;
}

std::vector<Mirror> BoardVision::DetectMirrors(
    const imgproc::Mat8& frame, const imgproc::Mat8& hsv, const int board[4],
    double scale, double min_ring_support) {
  const int bx = board[0], by = board[1], bw = board[2], bh = board[3];
  const HsvImage ch = SplitHsv(hsv);
  imgproc::Mat8 yellow(hsv.rows, hsv.cols, 1);
  for (std::size_t i = 0; i < yellow.a.size(); ++i) {
    yellow.a[i] = col::Pass(col::kMirrorYellow, ch.h[i], ch.s[i], ch.v[i])
        ? 255 : 0;
  }
  // restrict to the board rectangle
  for (int y = 0; y < hsv.rows; ++y) {
    std::uint8_t* row = yellow.Ptr(y);
    for (int x = 0; x < hsv.cols; ++x) {
      if (y < by || y >= by + bh || x < bx || x >= bx + bw) row[x] = 0;
    }
  }
  const int minR = std::max(12, PyRound(25 * scale));
  const int maxR = std::max(20, PyRound(50 * scale));
  std::vector<imgproc::Circle> circles;
  imgproc::HoughCircles(yellow, 1.15, std::max(30, PyRound(52 * scale)), 100,
                        std::max(10, PyRound(15 * scale)), minR, maxR, false,
                        &circles);
  if (getenv("VISION_DUMP_CIRCLES")) {
    if (std::FILE* f = std::fopen("mirror_circles_cpp.txt", "w")) {
      for (const auto& c : circles) {
        std::fprintf(f, "%g %g %g\n", c.x, c.y, c.r);
      }
      std::fclose(f);
    }
  }

  struct Proposal { Mirror m; };
  std::vector<Proposal> result;
  for (const auto& c : circles) {
    const int cx = PyRound(c.x), cy = PyRound(c.y);
    const int radius = PyRound(c.r);
    const int pad = static_cast<int>(radius * 1.08) + 2;
    const int wx0 = std::max(0, cx - pad);
    const int wx1 = std::min(frame.cols, cx + pad + 1);
    const int wy0 = std::max(0, cy - pad);
    const int wy1 = std::min(frame.rows, cy + pad + 1);
    std::size_t ringCount = 0, ringYellow = 0;
    for (int y = wy0; y < wy1; ++y) {
      for (int x = wx0; x < wx1; ++x) {
        const double dx = x - cx, dy = y - cy;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (dist < radius * 0.62 || dist > radius * 1.08) continue;
        ++ringCount;
        if (yellow.Ptr(y)[x] > 0) ++ringYellow;
      }
    }
    const double ringSupport = ringCount
        ? static_cast<double>(ringYellow) / ringCount : 0.0;
    const LongEdgePairEvidence pair =
        MirrorLongEdgePair(frame, hsv, cx, cy, radius, scale);
    if (ringSupport < min_ring_support || !pair.valid) continue;

    Mirror m;
    const SnapResult snap = SnapMirrorCenter(cx, cy, board);
    m.cx = snap.x;
    m.cy = snap.y;
    m.radius = static_cast<double>(radius);
    m.anchor_x = cx;
    m.anchor_y = cy;
    const double angle = pair.angle_deg;
    m.observed_angle_deg = PyRoundN(angle, 3);
    m.current_angle_deg = Round5(angle);
    const double errMod = std::fmod(angle - m.current_angle_deg + 90.0, 180.0);
    const double error = std::abs((errMod < 0 ? errMod + 180.0 : errMod) - 90.0);
    m.angle_error_deg = PyRoundN(error, 3);
    m.confidence = PyRoundN(std::min(1.0, ringSupport / 0.22) * pair.confidence, 4);
    m.ring_support = PyRoundN(ringSupport, 4);
    m.pair = pair;
    m.hidden = false;
    result.push_back({m});
  }

  // row clustering then left-to-right numbering (stable IDs across frames)
  const double rowTolerance = std::max(8.0, 12.0 * scale);
  std::vector<Mirror> ordered;
  ordered.reserve(result.size());
  for (auto& p : result) ordered.push_back(p.m);
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Mirror& a, const Mirror& b) { return a.cy < b.cy; });
  std::vector<std::vector<Mirror>> rows;
  for (const auto& item : ordered) {
    if (rows.empty()) {
      rows.push_back({item});
      continue;
    }
    std::vector<double> ys;
    for (const auto& member : rows.back()) ys.push_back(member.cy);
    const double rowY = Median(ys);
    if (std::abs(item.cy - rowY) <= rowTolerance) {
      rows.back().push_back(item);
    } else {
      rows.push_back({item});
    }
  }
  std::vector<Mirror> mirrors;
  for (auto& row : rows) {
    std::stable_sort(row.begin(), row.end(),
                     [](const Mirror& a, const Mirror& b) { return a.cx < b.cx; });
    for (auto& m : row) mirrors.push_back(m);
  }
  for (std::size_t i = 0; i < mirrors.size(); ++i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "M%02zu", i + 1);
    mirrors[i].id = buf;
  }
  return mirrors;
}

std::vector<AutoMirror> BoardVision::DetectAutoMirrors(
    const imgproc::Mat8& frame, const imgproc::Mat8& hsv, const int board[4],
    double scale) {
  const int bx = board[0], by = board[1], bw = board[2], bh = board[3];
  const HsvImage ch = SplitHsv(hsv);
  const auto n = ch.h.size();
  const double factor = 1.5 * scale;
  const double radius = 24.0 * factor;
  const double areaScale = factor * factor;

  imgproc::Mat8 yellow(hsv.rows, hsv.cols, 1);
  imgproc::Mat8 mask(hsv.rows, hsv.cols, 1);
  std::vector<std::uint8_t> metal(n), darkM(n), whiteM(n), arrowM(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t h = ch.h[i], s = ch.s[i], v = ch.v[i];
    yellow.a[i] = col::Pass(col::kAutoYellowArrow, h, s, v) ? 255 : 0;
    metal[i] = (s < col::kMetalSMax && v > col::kMetalVMin &&
                !(h >= col::kMetalCyanHueLo && h <= col::kMetalCyanHueHi)) ? 255 : 0;
    darkM[i] = (v < col::kOctoDarkVMax) ? 255 : 0;
    whiteM[i] = (s < col::kOctoWhiteSMax && v > col::kOctoWhiteVMin) ? 255 : 0;
    arrowM[i] = col::Pass(col::kAutoYellowArrow, h, s, v) ? 255 : 0;
  }
  for (int y = by; y < by + bh; ++y) {
    std::memcpy(mask.Ptr(y) + bx, yellow.Ptr(y) + bx, bw);
  }
  imgproc::CcResult cc;
  imgproc::ConnectedComponentsWithStats(mask, &cc);
  const int intRadius = static_cast<int>(std::ceil(radius));

  struct Found { AutoMirror m; };
  std::vector<Found> found;
  for (int index = 1; index < cc.count; ++index) {
    const int area = cc.stats[index][imgproc::kCcArea];
    const int w = cc.stats[index][imgproc::kCcWidth];
    const int hgt = cc.stats[index][imgproc::kCcHeight];
    if (!(30 * areaScale <= area && area <= 450 * areaScale)) continue;
    if (static_cast<double>(area) / std::max(w * hgt, 1) < 0.45) continue;
    if (std::max(w, hgt) > radius * 0.55) continue;
    const double blobX = cc.centroids[index][0];
    const double blobY = cc.centroids[index][1];

    // shared crop around the blob
    const int reachPx = static_cast<int>(radius * 0.55) + 2;
    const int half = static_cast<int>(radius * 1.3) + reachPx + 2;
    const int cx0 = PyRound(blobX), cy0 = PyRound(blobY);
    const int x0 = cx0 - half, y0 = cy0 - half;
    const int x1 = cx0 + half + 1, y1 = cy0 + half + 1;
    if (x0 < 0 || y0 < 0 || x1 > frame.cols || y1 > frame.rows) continue;
    const int baseX = x0 + half, baseY = y0 + half;
    auto pix = [&](const std::vector<std::uint8_t>& m, int y, int x) {
      return m[static_cast<std::size_t>(y) * hsv.cols + x];
    };

    auto scoreAt = [&](double dx, double dy, double out[4]) -> bool {
      const int ix = PyRound(dx), iy = PyRound(dy);
      const int ex = baseX + ix, ey = baseY + iy;
      if (!(x0 + intRadius < ex && ex < x1 - intRadius &&
            y0 + intRadius < ey && ey < y1 - intRadius)) return false;
      const int sx = half + ix, sy = half + iy;
      std::size_t rimCount = 0, coreCount = 0;
      std::size_t rimMetal = 0, coreDark = 0, coreWhite = 0, rimYellow = 0;
      for (int wy = sy - intRadius; wy <= sy + intRadius; ++wy) {
        for (int wx = sx - intRadius; wx <= sx + intRadius; ++wx) {
          const double ddx = wx - sx, ddy = wy - sy;
          const double dist = std::hypot(ddx, ddy);
          if (dist >= radius * 0.75 && dist <= radius * 1.1) {
            ++rimCount;
            if (pix(metal, y0 + wy, x0 + wx)) ++rimMetal;
            if (pix(arrowM, y0 + wy, x0 + wx)) ++rimYellow;
          }
          if (dist <= radius * 0.55) {
            ++coreCount;
            if (pix(darkM, y0 + wy, x0 + wx)) ++coreDark;
            if (pix(whiteM, y0 + wy, x0 + wx)) ++coreWhite;
          }
        }
      }
      out[0] = rimCount ? static_cast<double>(rimMetal) / rimCount : 0.0;
      out[1] = coreCount ? static_cast<double>(coreDark) / coreCount : 0.0;
      out[2] = coreCount ? static_cast<double>(coreWhite) / coreCount : 0.0;
      out[3] = rimCount ? static_cast<double>(rimYellow) / rimCount : 0.0;
      return true;
    };

    const int reach = static_cast<int>(radius * 0.55);
    const int step = 2;
    bool hasBest = false;
    double best[7] = {0, 0, 0, 0, 0, 0, 0};  // rim, dark, white, yellow, cx, cy
    for (int oy = -reach; oy <= reach; oy += step) {
      for (int ox = -reach; ox <= reach; ox += step) {
        double sc[4];
        if (!scoreAt(ox, oy, sc)) continue;
        if (sc[1] < 0.25 || sc[2] > 0.08 || sc[3] > 0.12) continue;
        if (!hasBest || sc[0] > best[0]) {
          best[0] = sc[0]; best[1] = sc[1]; best[2] = sc[2]; best[3] = sc[3];
          best[4] = blobX + ox; best[5] = blobY + oy;
          hasBest = true;
        }
      }
    }
    if (!hasBest || best[0] < 0.20) continue;
    double rimMetal = best[0], darkCore = best[1], whiteCore = best[2];
    double yellowRim = best[3];
    double cx = best[4], cy = best[5];
    const double coarseCx = best[4], coarseCy = best[5];
    // 1 px descent, capped, within 3 px of the coarse best
    for (int iter = 0; iter < 3; ++iter) {
      bool moved = false;
      const double deltas[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& d : deltas) {
        if (std::hypot(cx + d[0] - coarseCx, cy + d[1] - coarseCy) > 3.0) continue;
        double sc[4];
        if (!scoreAt(cx - blobX + d[0], cy - blobY + d[1], sc)) continue;
        if (sc[0] > rimMetal + 1e-6 && sc[1] >= 0.25 && sc[2] <= 0.08 && sc[3] <= 0.12) {
          rimMetal = sc[0]; darkCore = sc[1]; whiteCore = sc[2]; yellowRim = sc[3];
          cx += d[0]; cy += d[1];
          moved = true;
          break;
        }
      }
      if (!moved) break;
    }
    const SnapResult snap = SnapMirrorCenter(cx, cy, board);
    if (std::hypot(snap.x - cx, snap.y - cy) > 0.60 * radius) continue;
    cx = snap.x;
    cy = snap.y;
    const double dirDx = blobX - cx, dirDy = blobY - cy;
    if (std::hypot(dirDx, dirDy) < 1e-3) continue;
    const double observed = NormalizeDeg360(std::atan2(dirDy, dirDx) * 180.0 / M_PI);
    const double directionDeg = std::fmod(std::rint(observed / 45.0) * 45.0, 360.0);
    const double error = std::abs(std::fmod(observed - directionDeg + 180.0, 360.0) - 180.0);
    AutoMirror m;
    m.cx = PyRoundN(cx, 3);
    m.cy = PyRoundN(cy, 3);
    m.radius = PyRoundN(radius, 3);
    m.direction_deg = directionDeg;
    m.dir_dx = PyRoundN(std::cos(directionDeg * M_PI / 180.0), 6);
    m.dir_dy = PyRoundN(std::sin(directionDeg * M_PI / 180.0), 6);
    for (int i = 0; i < 8; ++i) {
      m.polygon[i][0] = PyRoundN(cx + radius * std::cos(i * 45.0 * M_PI / 180.0), 3);
      m.polygon[i][1] = PyRoundN(cy + radius * std::sin(i * 45.0 * M_PI / 180.0), 3);
    }
    m.confidence = PyRoundN(std::min(1.0, rimMetal / 0.34), 4);
    m.angle_error_deg = PyRoundN(error, 3);
    m.rim_metal = PyRoundN(rimMetal, 4);
    m.dark_core = PyRoundN(darkCore, 4);
    m.white_core = PyRoundN(whiteCore, 4);
    m.yellow_rim = PyRoundN(yellowRim, 4);
    m.arrow_offset_deg = PyRoundN(observed, 2);
    found.push_back({m});
  }
  // one entity per device: strongest rim first, then radius exclusion
  std::stable_sort(found.begin(), found.end(),
                   [](const Found& a, const Found& b) {
                     return a.m.rim_metal > b.m.rim_metal;
                   });
  std::vector<AutoMirror> kept;
  for (auto& f : found) {
    bool tooClose = false;
    for (const auto& other : kept) {
      if (std::hypot(f.m.cx - other.cx, f.m.cy - other.cy) < f.m.radius * 1.6) {
        tooClose = true;
        break;
      }
    }
    if (tooClose) continue;
    kept.push_back(f.m);
  }
  std::stable_sort(kept.begin(), kept.end(),
                   [](const AutoMirror& a, const AutoMirror& b) {
                     if (a.cy != b.cy) return a.cy < b.cy;
                     return a.cx < b.cx;
                   });
  for (std::size_t i = 0; i < kept.size(); ++i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "A%02zu", i + 1);
    kept[i].id = buf;
  }
  return kept;
}

std::vector<Target> BoardVision::DetectTargets(
    const imgproc::Mat8& frame, const imgproc::Mat8& hsv, const int board[4],
    double scale) {
  const int bx = board[0], by = board[1], bw = board[2], bh = board[3];
  const HsvImage ch = SplitHsv(hsv);
  const auto n = ch.h.size();
  std::vector<std::uint8_t> yellowM(n), cyanM(n);
  for (std::size_t i = 0; i < n; ++i) {
    yellowM[i] = col::Pass(col::kMirrorYellow, ch.h[i], ch.s[i], ch.v[i]) ? 255 : 0;
    cyanM[i] = col::Pass(col::kTargetCyanCore, ch.h[i], ch.s[i], ch.v[i]) ? 255 : 0;
  }
  const imgproc::Mat8 gray = imgproc::CvtBgrToGray(frame);
  const imgproc::Mat8 blurred = imgproc::GaussianBlur3x3(gray);
  imgproc::Mat8 restricted(blurred.rows, blurred.cols, 1);
  for (int y = by; y < by + bh; ++y) {
    std::memcpy(restricted.Ptr(y) + bx, blurred.Ptr(y) + bx, bw);
  }
  std::vector<imgproc::Circle> circles;
  imgproc::HoughCircles(restricted, 1.2, std::max(30, PyRound(42 * scale)), 120,
                        std::max(18, PyRound(26 * scale)),
                        std::max(10, PyRound(20 * scale)),
                        std::max(16, PyRound(36 * scale)), false, &circles);
  if (getenv("VISION_DUMP_CIRCLES")) {
    if (std::FILE* f = std::fopen("target_circles_cpp.txt", "w")) {
      for (const auto& c : circles) {
        std::fprintf(f, "%g %g %g\n", c.x, c.y, c.r);
      }
      std::fclose(f);
    }
  }

  struct Candidate { Target t; };
  std::vector<Candidate> candidates;
  for (const auto& c : circles) {
    const int cx = PyRound(c.x), cy = PyRound(c.y);
    const int radius = PyRound(c.r);
    if (!(bx + 8 <= cx && cx <= bx + bw - 8 && by + 8 <= cy && cy <= by + bh - 8)) {
      continue;
    }
    const int pad = radius + 2;
    const int x0 = std::max(0, cx - pad);
    const int x1 = std::min(frame.cols, cx + pad + 1);
    const int y0 = std::max(0, cy - pad);
    const int y1 = std::min(frame.rows, cy + pad + 1);
    std::size_t ringCount = 0;
    std::size_t ringYellow = 0;
    std::size_t coreCount = 0;
    std::size_t coreRed = 0;
    std::size_t cyanCount = 0;
    std::size_t coreCyan = 0;
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        const double dx = x - cx, dy = y - cy;
        const double dist = std::hypot(dx, dy);
        const std::size_t idx = static_cast<std::size_t>(y) * hsv.cols + x;
        if (dist >= radius * 0.75 && dist <= radius * 1.4) {
          ++ringCount;
          if (yellowM[idx]) ++ringYellow;
        }
        if (dist <= radius * 0.6) {
          ++coreCount;
          // firewall discriminator: red LEDs inside the glass. Geometry alone
          // cannot tell a firewall from a packet (same package, same circular
          // edge); this single red-negative is not a cyan confirmation.
          if (col::Pass(col::kFirewallRed, ch.h[idx], ch.s[idx], ch.v[idx]))
            ++coreRed;
        }
        if (dist <= radius * 0.72) {
          ++cyanCount;
          if (cyanM[idx]) ++coreCyan;
        }
      }
    }
    const double ringYellowFrac = ringCount
        ? static_cast<double>(ringYellow) / ringCount : 0.0;
    const double redFrac = coreCount
        ? static_cast<double>(coreRed) / coreCount : 0.0;
    // Acceptance: radial edge on every ray (geometry, primary evidence), the
    // yellow-ring negative against mirror circles, the red-negative against
    // firewalls, and the cyan-core fraction as corroboration (never the sole
    // basis).
    const double edge = TargetRadialEdgeMetric(frame, cx, cy, radius);
    const double coreCyanFrac = coreCount
        ? static_cast<double>(coreCyan) / coreCount : 0.0;
    const bool colorClean = ringYellowFrac <= 0.15 && redFrac <= 0.3;
    double fcx = cx, fcy = cy, fr = static_cast<double>(radius);
    double cyanFrac = coreCyanFrac;
    double conf = edge;
    bool accept = colorClean && edge >= kTargetEdgeMin &&
                  coreCyanFrac >= kCoreCyanMin;
    if (!accept && colorClean) {
      // Occluded special case: only the bottom wedge is visible (tooltip over
      // the top-left targets).  The contiguous strong rays must span >= 60%
      // of the circle and cover the bottom; the cyan fraction is then counted
      // over just the visible wedge, at full strength.
      int runStart = 0, runLen = 0;
      if (OccludedBottomGeometry(frame, cx, cy, radius, &runStart, &runLen,
                                 &fcx, &fcy, &fr)) {
        const double a0 = TargetRayAngle(runStart) - M_PI / kTargetRays;
        const double a1 = a0 + runLen * (2.0 * M_PI / kTargetRays);
        std::size_t wedgeCount = 0, wedgeCyan = 0;
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            const double dx = x - cx, dy = y - cy;
            if (std::hypot(dx, dy) > radius * 0.6) continue;
            double ang = std::atan2(dy, dx);
            while (ang < a0) ang += 2.0 * M_PI;
            while (ang >= a0 + 2.0 * M_PI) ang -= 2.0 * M_PI;
            if (ang > a1) continue;
            ++wedgeCount;
            const std::size_t idx = static_cast<std::size_t>(y) * hsv.cols + x;
            if (cyanM[idx]) ++wedgeCyan;
          }
        }
        cyanFrac = wedgeCount
            ? static_cast<double>(wedgeCyan) / wedgeCount : 0.0;
        double steps[kTargetRays], tbest[kTargetRays];
        TargetRaySteps(frame, cx, cy, radius, steps, tbest);
        double worst = 255.0;
        for (int i = 0; i < runLen; ++i) {
          worst = std::min(worst, steps[(runStart + i) % kTargetRays]);
        }
        accept = cyanFrac >= kCoreCyanMin;
        conf = worst;
      }
    }
    if (!accept) continue;
    Target t;
    t.cx = fcx;
    t.cy = fcy;
    t.radius = fr;
    t.rect[0] = fcx - fr;
    t.rect[1] = fcy - fr;
    t.rect[2] = 2.0 * fr;
    t.rect[3] = 2.0 * fr;
    t.confidence = PyRoundN(std::min(1.0, conf / 40.0), 4);
    t.alive = true;
    t.core_cyan = PyRoundN(cyanFrac, 4);
    t.ring_yellow = PyRoundN(ringYellowFrac, 4);
    candidates.push_back({t});
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) {
                     return a.t.confidence > b.t.confidence;
                   });
  std::vector<Target> targets;
  for (auto& cand : candidates) {
    bool tooClose = false;
    for (const auto& kept : targets) {
      if (std::hypot(cand.t.cx - kept.cx, cand.t.cy - kept.cy) <
          (cand.t.radius + kept.radius) * 1.05) {
        tooClose = true;
        break;
      }
    }
    if (tooClose) continue;
    targets.push_back(cand.t);
  }
  std::stable_sort(targets.begin(), targets.end(),
                   [](const Target& a, const Target& b) {
                     if (a.cy != b.cy) return a.cy < b.cy;
                     return a.cx < b.cx;
                   });
  for (std::size_t i = 0; i < targets.size(); ++i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "T%02zu", i + 1);
    targets[i].id = buf;
  }
  return targets;
}

BoardVision::MirrorLayer BoardVision::DetectMirrorTargetLayer(
    const imgproc::Mat8& frame, const imgproc::Mat8& hsv, const int board[4],
    double scale) {
  MirrorLayer layer;
  layer.mirrors = DetectMirrors(frame, hsv, board, scale, 0.25);
  for (auto& m : layer.mirrors) m.hidden = false;
  const std::vector<Mirror> recovered =
      DetectLaserOccludedMirrors(frame, hsv, board, layer.mirrors, scale);
  layer.mirrors.insert(layer.mirrors.end(), recovered.begin(), recovered.end());
  std::stable_sort(layer.mirrors.begin(), layer.mirrors.end(),
                   [](const Mirror& a, const Mirror& b) {
                     if (a.cy != b.cy) return a.cy < b.cy;
                     return a.cx < b.cx;
                   });
  for (std::size_t i = 0; i < layer.mirrors.size(); ++i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "M%02zu", i + 1);
    layer.mirrors[i].id = buf;
  }
  layer.auto_mirrors = DetectAutoMirrors(frame, hsv, board, scale);
  // a mount proposal landing on an octagon duplicates it: drop the mount
  std::vector<Mirror> deduped;
  for (const auto& m : layer.mirrors) {
    bool duplicate = false;
    for (const auto& a : layer.auto_mirrors) {
      if (std::hypot(m.cx - a.cx, m.cy - a.cy) <= (m.radius + a.radius) * 1.05) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) deduped.push_back(m);
  }
  layer.mirrors = std::move(deduped);
  // authored mirrors share one physical size: force every mirror to the
  // frame median. Per-mirror Hough fits are not trusted (the laser beam on
  // the source mirror biases its fit, which put overlay labels inside the
  // mirror rim), so no single measured radius survives — only the median.
  if (!layer.mirrors.empty()) {
    std::vector<double> radii;
    for (const auto& m : layer.mirrors) radii.push_back(m.radius);
    const double medianRadius = Median(radii);
    for (auto& m : layer.mirrors) {
      m.radius = medianRadius;
    }
  }
  layer.targets = DetectTargets(frame, hsv, board, scale);
  std::vector<Target> keptTargets;
  for (const auto& t : layer.targets) {
    bool tooClose = false;
    for (const auto& m : layer.mirrors) {
      if (std::hypot(t.cx - m.cx, t.cy - m.cy) <= (t.radius + m.radius) * 1.05) {
        tooClose = true;
        break;
      }
    }
    if (!tooClose) {
      for (const auto& a : layer.auto_mirrors) {
        if (std::hypot(t.cx - a.cx, t.cy - a.cy) <= (t.radius + a.radius) * 1.05) {
          tooClose = true;
          break;
        }
      }
    }
    if (!tooClose) keptTargets.push_back(t);
  }
  layer.targets = std::move(keptTargets);
  return layer;
}

std::vector<Mirror> BoardVision::DetectLaserOccludedMirrors(
    const imgproc::Mat8& frame, const imgproc::Mat8& hsv, const int board[4],
    const std::vector<Mirror>& known, double scale) {
  const HsvImage ch = SplitHsv(hsv);
  const auto n = ch.h.size();
  bool anyLaser = false;
  std::vector<std::uint8_t> laserM(n);
  for (std::size_t i = 0; i < n; ++i) {
    laserM[i] = col::Pass(col::kLaserBeam, ch.h[i], ch.s[i], ch.v[i]) ? 255 : 0;
    if (laserM[i]) anyLaser = true;
  }
  if (!anyLaser) return {};
  imgproc::Mat8 suppressed = frame;  // copy
  for (std::size_t i = 0; i < n; ++i) {
    if (!laserM[i]) continue;
    suppressed.a[3 * i + 0] = 18;
    suppressed.a[3 * i + 1] = 28;
    suppressed.a[3 * i + 2] = 55;
  }
  // HSV after suppression differs only at laser pixels (constant colour)
  imgproc::Mat8 probe(1, 1, 3);
  probe.a[0] = 18; probe.a[1] = 28; probe.a[2] = 55;
  const imgproc::Mat8 probeHsv = imgproc::CvtBgrToHsv(probe);
  imgproc::Mat8 suppressedHsv = hsv;  // copy
  for (std::size_t i = 0; i < n; ++i) {
    if (!laserM[i]) continue;
    suppressedHsv.a[3 * i + 0] = probeHsv.a[0];
    suppressedHsv.a[3 * i + 1] = probeHsv.a[1];
    suppressedHsv.a[3 * i + 2] = probeHsv.a[2];
  }
  std::vector<Mirror> found =
      DetectMirrors(suppressed, suppressedHsv, board, scale, 0.05);
  const double exclusion = std::max(18.0, 45.0 * scale);
  std::vector<Mirror> recovered;
  for (auto& cand : found) {
    bool close = false;
    for (const auto& item : known) {
      if (std::hypot(cand.cx - item.cx, cand.cy - item.cy) < exclusion) {
        close = true;
        break;
      }
    }
    if (close) continue;
    cand.hidden = true;
    recovered.push_back(cand);
  }
  return recovered;
}

GateResult BoardVision::DetectLiveScene(const imgproc::Mat8& frame,
                                        const int board_rect[4],
                                        SceneModel* out,
                                        imgproc::Mat8* annotated) {
  const int bx = board_rect[0], by = board_rect[1];
  const int bw = board_rect[2], bh = board_rect[3];
  GateResult g;
  g.gate = "ENTITIES";
  if (bw <= 0 || bh <= 0 || by + bh > frame.rows || bx + bw > frame.cols) {
    g.status = "NO";
    g.reason = "BOARD_ROI_EMPTY";
    return g;
  }
  const double scale = geom::ProcessingScale(frame.rows);
  const int margin = PyRound(72 * scale);
  const int x0 = std::max(0, bx - margin);
  const int y0 = std::max(0, by - margin);
  const int x1 = std::min(frame.cols, bx + bw + margin);
  const int y1 = std::min(frame.rows, by + bh + margin);
  imgproc::Mat8 sub(y1 - y0, x1 - x0, 3);
  for (int y = 0; y < y1 - y0; ++y) {
    std::memcpy(sub.Ptr(y), frame.Ptr(y0 + y) + static_cast<std::size_t>(x0) * 3,
                static_cast<std::size_t>(x1 - x0) * 3);
  }
  const imgproc::Mat8 hsv = imgproc::CvtBgrToHsv(sub);
  const int subBoard[4] = {bx - x0, by - y0, bw, bh};
  MirrorLayer layer = DetectMirrorTargetLayer(sub, hsv, subBoard, scale);
  for (auto& m : layer.mirrors) {
    m.cx += x0; m.cy += y0;
    m.anchor_x += x0; m.anchor_y += y0;
  }
  for (auto& a : layer.auto_mirrors) {
    a.cx += x0; a.cy += y0;
    for (auto& pt : a.polygon) {
      pt[0] += x0;
      pt[1] += y0;
    }
  }
  for (auto& t : layer.targets) {
    t.cx += x0; t.cy += y0;
    t.rect[0] += x0;
    t.rect[1] += y0;
  }
  out->processing_w = frame.cols;
  out->processing_h = frame.rows;
  out->beam_half_width = PyRoundN(15.0 * frame.rows / geom::kRefHeight, 3);
  std::copy(board_rect, board_rect + 4, out->playfield);
  out->mirrors = std::move(layer.mirrors);
  out->auto_mirrors = std::move(layer.auto_mirrors);
  out->targets = std::move(layer.targets);

  g.status = "YES";
  g.reason = "LIVE_SCENE_READY";
  std::string reasons;
  const std::size_t mirrorCount = out->mirrors.size() + out->auto_mirrors.size();
  if (!(1 <= mirrorCount && mirrorCount <= 16)) {
    reasons = "MIRROR_COUNT_" + std::to_string(out->mirrors.size());
  }
  if (out->targets.empty()) {
    if (!reasons.empty()) reasons += ";";
    reasons += "NO_TARGETS";
  }
  if (!reasons.empty()) {
    g.status = "NO";
    g.reason = reasons;
    g.confidence = 0.0;
  } else {
    double confidence = 0.9;
    for (const auto& m : out->mirrors) {
      confidence = std::min(confidence, m.confidence);
    }
    g.confidence = confidence;
  }
  (void)annotated;
  return g;
}

namespace {

// Per-ray largest 1-px grey step within [0.8r, 1.2r], plus the radius it was
// found at.  Shared by the full-circle gate and the occluded-bottom fallback.
void TargetRaySteps(const imgproc::Mat8& frame, double cx, double cy,
                    double radius, double* steps, double* tbest) {
  const int pad = static_cast<int>(radius * 1.25) + 3;
  const int fcx = PyRound(cx), fcy = PyRound(cy);
  const int x0 = std::max(0, fcx - pad);
  const int y0 = std::max(0, fcy - pad);
  const int x1 = std::min(frame.cols, fcx + pad + 1);
  const int y1 = std::min(frame.rows, fcy + pad + 1);
  for (int k = 0; k < kTargetRays; ++k) {
    steps[k] = 0.0;
    tbest[k] = 0.8 * radius;
  }
  if (x1 <= x0 || y1 <= y0) return;
  imgproc::Mat8 sub(y1 - y0, x1 - x0, 3);
  for (int y = 0; y < y1 - y0; ++y) {
    std::memcpy(sub.Ptr(y), frame.Ptr(y0 + y) + static_cast<std::size_t>(x0) * 3,
                static_cast<std::size_t>(x1 - x0) * 3);
  }
  const imgproc::Mat8 gray = imgproc::CvtBgrToGray(sub);
  const double lcx = cx - x0, lcy = cy - y0;
  auto greyAt = [&](int px, int py) {
    return static_cast<int>(gray.Ptr(py)[px]);
  };
  for (int k = 0; k < kTargetRays; ++k) {
    const double ang = k * (M_PI / (kTargetRays / 2)) + M_PI / kTargetRays;
    const double ca = std::cos(ang), sa = std::sin(ang);
    double best = 0.0, bestT = 0.8 * radius;
    for (double t = 0.8 * radius; t <= 1.2 * radius; t += 1.0) {
      const int ax = std::clamp(PyRound(lcx + t * ca), 0, gray.cols - 1);
      const int ay = std::clamp(PyRound(lcy + t * sa), 0, gray.rows - 1);
      const int bx = std::clamp(PyRound(lcx + (t + 1.0) * ca), 0, gray.cols - 1);
      const int by = std::clamp(PyRound(lcy + (t + 1.0) * sa), 0, gray.rows - 1);
      const double step = std::abs(greyAt(bx, by) - greyAt(ax, ay));
      if (step > best) {
        best = step;
        bestT = t;
      }
    }
    steps[k] = best;
    tbest[k] = bestT;
  }
}

double TargetRayAngle(int k) {
  return k * (M_PI / (kTargetRays / 2)) + M_PI / kTargetRays;
}

// Longest contiguous (cyclic) run of strong rays; returns its length and
// start index.  A fully visible target gives kTargetRays.
int TargetStrongRun(const double* steps, int* start) {
  int best = 0, bestStart = 0;
  int run = 0, runStart = 0;
  for (int i = 0; i < 2 * kTargetRays; ++i) {
    if (steps[i % kTargetRays] >= kTargetEdgeMin) {
      if (run == 0) runStart = i;
      ++run;
      if (run > best && run <= kTargetRays) {
        best = run;
        bestStart = runStart % kTargetRays;
      }
    } else {
      run = 0;
    }
  }
  *start = bestStart;
  return best;
}

bool TargetRunCoversBottom(int start, int len) {
  // rays 3 and 4 bracket straight down (image y grows downward)
  for (int d = 3; d <= 4; ++d) {
    const int pos = (d - start + kTargetRays) % kTargetRays;
    if (pos < len) return true;
  }
  return false;
}

// Kasa algebraic circle fit; needs a well-spread arc, which the visible
// bottom wedge (>= 60% of the rays) provides.
bool FitCircle(const std::vector<std::pair<double, double>>& pts, double* cx,
               double* cy, double* r) {
  const std::size_t n = pts.size();
  if (n < 5) return false;
  double sx = 0, sy = 0, sz = 0, sxx = 0, sxy = 0, sxz = 0, syy = 0, syz = 0;
  for (const auto& p : pts) {
    const double z = p.first * p.first + p.second * p.second;
    sx += p.first; sy += p.second; sz += z;
    sxx += p.first * p.first; sxy += p.first * p.second; sxz += p.first * z;
    syy += p.second * p.second; syz += p.second * z;
  }
  const double dn = static_cast<double>(n);
  double m[3][4] = {
      {sxx, sxy, sx, sxz},
      {sxy, syy, sy, syz},
      {sx, sy, dn, sz},
  };
  for (int c = 0; c < 3; ++c) {
    int piv = c;
    for (int rr = c + 1; rr < 3; ++rr) {
      if (std::abs(m[rr][c]) > std::abs(m[piv][c])) piv = rr;
    }
    if (std::abs(m[piv][c]) < 1e-9) return false;
    if (piv != c) std::swap(m[piv], m[c]);
    for (int rr = 0; rr < 3; ++rr) {
      if (rr == c) continue;
      const double f = m[rr][c] / m[c][c];
      for (int cc = c; cc < 4; ++cc) m[rr][cc] -= f * m[c][cc];
    }
  }
  const double a = m[0][3] / m[0][0];
  const double b = m[1][3] / m[1][1];
  const double c0 = m[2][3] / m[2][2];
  const double rr = c0 + a * a / 4.0 + b * b / 4.0;
  if (rr <= 0.0) return false;
  *cx = a / 2.0;
  *cy = b / 2.0;
  *r = std::sqrt(rr);
  return true;
}

// Occluded-target geometry: the longest strong-ray run must be a bottom wedge
// of at least kMinStrongRun rays and shorter than the full circle.  The
// centre/radius are then re-fit from the boundary points of exactly those
// rays, so a seed centre biased by the partial Hough arc is corrected.
bool OccludedBottomGeometry(const imgproc::Mat8& frame, double cx, double cy,
                            double radius, int* runStart, int* runLen,
                            double* fitCx, double* fitCy, double* fitR) {
  double steps[kTargetRays], tbest[kTargetRays];
  TargetRaySteps(frame, cx, cy, radius, steps, tbest);
  int start = 0;
  const int len = TargetStrongRun(steps, &start);
  *runStart = start;
  *runLen = len;
  if (len < kMinStrongRun || len >= kTargetRays) return false;
  if (!TargetRunCoversBottom(start, len)) return false;
  std::vector<std::pair<double, double>> pts;
  for (int i = 0; i < len; ++i) {
    const int k = (start + i) % kTargetRays;
    const double ang = TargetRayAngle(k);
    pts.emplace_back(cx + tbest[k] * std::cos(ang),
                     cy + tbest[k] * std::sin(ang));
  }
  double fcx = cx, fcy = cy, fr = radius;
  if (FitCircle(pts, &fcx, &fcy, &fr) &&
      fr >= 0.5 * radius && fr <= 1.8 * radius &&
      std::hypot(fcx - cx, fcy - cy) <= 0.6 * radius) {
    *fitCx = fcx;
    *fitCy = fcy;
    *fitR = fr;
  } else {
    *fitCx = cx;
    *fitCy = cy;
    *fitR = radius;
  }
  return true;
}

}  // namespace

bool BoardVision::TargetPresent(const imgproc::Mat8& frame,
                                const Target& expected, double* metricOut) {
  if (frame.a.empty()) return false;
  const double metric = TargetRadialEdgeMetric(
      frame, expected.cx, expected.cy, std::max(8.0, expected.radius));
  if (metricOut) *metricOut = PyRoundN(metric, 3);
  if (metric >= kTargetEdgeMin) return true;
  int start = 0, len = 0;
  double fcx, fcy, fr;
  return OccludedBottomGeometry(frame, expected.cx, expected.cy,
                                std::max(8.0, expected.radius), &start, &len,
                                &fcx, &fcy, &fr);
}

double BoardVision::TargetRadialEdgeMetric(const imgproc::Mat8& frame,
                                           double cx, double cy,
                                           double radius) {
  double steps[kTargetRays], tbest[kTargetRays];
  TargetRaySteps(frame, cx, cy, radius, steps, tbest);
  double worst = 255.0;
  for (int k = 0; k < kTargetRays; ++k) worst = std::min(worst, steps[k]);
  return worst;
}

}  // namespace vision
