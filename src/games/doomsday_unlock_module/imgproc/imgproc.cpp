#include "imgproc/imgproc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <numeric>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace imgproc {
namespace {

inline int Rint(float v) { return static_cast<int>(std::rint(v)); }
inline int Rint(double v) { return static_cast<int>(std::rint(v)); }

// Fixed-point HSV tables (shift 12), matching cv2's 8U BGR2HSV path.
constexpr int kHsvShift = 12;
struct HsvTables {
  std::int32_t sdiv[256] = {0};
  std::int64_t hdiv[256] = {0};
  HsvTables() {
    for (int i = 1; i < 256; ++i) {
      sdiv[i] = static_cast<std::int32_t>(
          std::lround((255 << kHsvShift) / (1.0 * i)));
      hdiv[i] = static_cast<std::int64_t>(
          std::lround((180 << kHsvShift) / (6.0 * i)));
    }
  }
};
const HsvTables& HsvTable() {
  static const HsvTables tables;
  return tables;
}

inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// sobel 3x3 with replicate ("edge") padding; int16 arithmetic like numpy.
Mat16 Sobel3x3(const Mat8& img, bool xDir) {
  const int h = img.rows, w = img.cols;
  Mat16 out(h, w);
  // padded p has (h+2, w+2); p[y+1][x+1] = img[y][x], border replicates
  auto pAt = [&](int py, int px) -> int {
    const int y = Clamp(py - 1, 0, h - 1);
    const int x = Clamp(px - 1, 0, w - 1);
    return img.Ptr(y)[x];
  };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (xDir) {
        // output row y sums img rows y-1, y, y+1 (pAt(py,px) = img[py-1][px-1])
        const int r0 = pAt(y, x + 2) - pAt(y, x);
        const int r1 = pAt(y + 1, x + 2) - pAt(y + 1, x);
        const int r2 = pAt(y + 2, x + 2) - pAt(y + 2, x);
        out.Ptr(y)[x] = static_cast<std::int16_t>(r0 + 2 * r1 + r2);
      } else {
        const int c0 = pAt(y, x) + 2 * pAt(y, x + 1) + pAt(y, x + 2);
        const int c2 = pAt(y + 2, x) + 2 * pAt(y + 2, x + 1) + pAt(y + 2, x + 2);
        out.Ptr(y)[x] = static_cast<std::int16_t>(c2 - c0);
      }
    }
  }
  return out;
}

// 8-connected closure of strong over weak pixels. BFS from the strong set
// gives exactly the fixed point of the iterative dilation sweep; BFS depth
// equals the iteration count (8-neighbour Chebyshev growth), so maxIters
// truncates identically to the numpy reference loop.
Mat8 Hysteresis(const std::vector<std::uint8_t>& strong,
                const std::vector<std::uint8_t>& weak, int h, int w,
                int maxIters) {
  Mat8 out(h, w, 1);
  // edge pixels: strong seeds, then weak pixels reached by the flood
  std::vector<std::uint8_t> edges(strong);          // 1 = accepted edge
  std::vector<std::uint8_t> seen(strong);           // 1 = enqueued
  std::vector<int> frontier;
  frontier.reserve(h * w / 4);
  for (int i = 0; i < h * w; ++i) {
    if (edges[i]) frontier.push_back(i);
  }
  auto visit = [&](int y, int x, std::vector<int>& next) {
    if (y < 0 || y >= h || x < 0 || x >= w) return;
    const int i = y * w + x;
    if (seen[i] || !weak[i]) return;
    seen[i] = 1;
    edges[i] = 1;
    next.push_back(i);
  };
  int depth = 0;
  while (!frontier.empty() && (maxIters < 0 || depth < maxIters)) {
    std::vector<int> next;
    next.reserve(frontier.size());
    for (const int i : frontier) {
      const int y = i / w, x = i % w;
      visit(y - 1, x - 1, next);
      visit(y - 1, x, next);
      visit(y - 1, x + 1, next);
      visit(y, x - 1, next);
      visit(y, x + 1, next);
      visit(y + 1, x - 1, next);
      visit(y + 1, x, next);
      visit(y + 1, x + 1, next);
    }
    frontier.swap(next);
    ++depth;
  }
  for (int i = 0; i < h * w; ++i) out.a[i] = edges[i] ? 255 : 0;
  return out;
}

}  // namespace

Mat8 CvtBgraToBgr(const Mat8& img) {
  Mat8 out(img.rows, img.cols, 3);
  for (int y = 0; y < img.rows; ++y) {
    const std::uint8_t* src = img.Ptr(y);
    std::uint8_t* dst = out.Ptr(y);
    for (int x = 0; x < img.cols; ++x) {
      dst[3 * x + 0] = src[4 * x + 0];
      dst[3 * x + 1] = src[4 * x + 1];
      dst[3 * x + 2] = src[4 * x + 2];
    }
  }
  return out;
}

Mat8 CvtBgrToGray(const Mat8& img) {
  Mat8 out(img.rows, img.cols, 1);
  for (int y = 0; y < img.rows; ++y) {
    const std::uint8_t* src = img.Ptr(y);
    std::uint8_t* dst = out.Ptr(y);
    for (int x = 0; x < img.cols; ++x) {
      const float b = src[3 * x + 0];
      const float g = src[3 * x + 1];
      const float r = src[3 * x + 2];
      dst[x] = static_cast<std::uint8_t>(Rint(b * 0.114f + g * 0.587f + r * 0.299f));
    }
  }
  return out;
}

Mat8 CvtBgrToHsv(const Mat8& img) {
  const HsvTables& t = HsvTable();
  const int h = img.rows, w = img.cols;
  Mat8 out(h, w, 3);
  const std::int32_t half = 1 << (kHsvShift - 1);
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* src = img.Ptr(y);
    std::uint8_t* dst = out.Ptr(y);
    for (int x = 0; x < w; ++x) {
      const int b = src[3 * x + 0];
      const int g = src[3 * x + 1];
      const int r = src[3 * x + 2];
      const int v = std::max(std::max(b, g), r);
      const int vmin = std::min(std::min(b, g), r);
      const int diff = v - vmin;
      const bool vr = v == r;
      const bool vg = v == g;
      const int s = static_cast<int>(
          (static_cast<std::int64_t>(diff) * t.sdiv[v] + half) >> kHsvShift);
      int hpre;
      if (vr) {
        hpre = g - b;
      } else if (vg) {
        hpre = b - r + 2 * diff;
      } else {
        hpre = r - g + 4 * diff;
      }
      int hh = static_cast<int>(
          (static_cast<std::int64_t>(hpre) * t.hdiv[diff] + half) >> kHsvShift);
      if (hh < 0) hh += 180;
      dst[3 * x + 0] = static_cast<std::uint8_t>(hh);
      dst[3 * x + 1] = static_cast<std::uint8_t>(s);
      dst[3 * x + 2] = static_cast<std::uint8_t>(v);
    }
  }
  return out;
}

Kernel StructuringElementEllipse(int ksize) {
  Kernel elem(static_cast<std::size_t>(ksize) * ksize, 0);
  const int r0 = ksize / 2, c0 = ksize / 2;
  if (ksize <= 1) {
    if (ksize == 1) elem[0] = 1;
    return elem;
  }
  const double invR2 = r0 ? 1.0 / (r0 * r0) : 1.0;
  for (int i = 0; i < ksize; ++i) {
    const int dy = i - r0;
    if (std::abs(dy) <= r0) {
      const int dx = static_cast<int>(std::lround(
          c0 * std::sqrt(std::max(0.0, (r0 * r0 - dy * dy) * invR2))));
      for (int x = c0 - dx; x <= c0 + dx; ++x) elem[i * ksize + x] = 1;
    }
  }
  return elem;
}

namespace {

Mat8 Morph(const Mat8& mask, const Kernel& kernel, bool isDilate, int iterations) {
  const int kh = static_cast<int>(std::sqrt(static_cast<double>(kernel.size())));
  const int kw = kh;
  std::vector<std::pair<int, int>> taps;
  for (int ty = 0; ty < kh; ++ty)
    for (int tx = 0; tx < kw; ++tx)
      if (kernel[ty * kw + tx] > 0) taps.emplace_back(ty, tx);
  Mat8 work = mask;
  const int nIter = std::max(1, iterations);
  for (int it = 0; it < nIter; ++it) {
    const int oh = work.rows + kh - 1, ow = work.cols + kw - 1;
    const std::uint8_t padVal = isDilate ? 0 : 255;
    std::vector<std::uint8_t> padded(static_cast<std::size_t>(oh) * ow, padVal);
    const int oy = kh / 2, ox = kw / 2;
    for (int y = 0; y < work.rows; ++y) {
      std::memcpy(padded.data() + static_cast<std::size_t>(oy + y) * ow + ox,
                  work.Ptr(y), work.cols);
    }
    Mat8 acc(work.rows, work.cols, 1);
    const int wh = work.rows, ww = work.cols;
    // init acc to padVal and fold every tap
    std::fill(acc.a.begin(), acc.a.end(), padVal);
    for (auto [ty, tx] : taps) {
      const std::uint8_t* base = padded.data() + static_cast<std::size_t>(ty) * ow + tx;
      for (int y = 0; y < wh; ++y) {
        const std::uint8_t* win = base + static_cast<std::size_t>(y) * ow;
        std::uint8_t* accRow = acc.Ptr(y);
        if (isDilate) {
          for (int x = 0; x < ww; ++x) accRow[x] = std::max(accRow[x], win[x]);
        } else {
          for (int x = 0; x < ww; ++x) accRow[x] = std::min(accRow[x], win[x]);
        }
      }
    }
    work = std::move(acc);
  }
  return work;
}

}  // namespace

Mat8 Dilate(const Mat8& mask, const Kernel& kernel, int iterations) {
  return Morph(mask, kernel, true, iterations);
}

Mat8 Erode(const Mat8& mask, const Kernel& kernel, int iterations) {
  return Morph(mask, kernel, false, iterations);
}

Mat8 GaussianBlur3x3(const Mat8& img) {
  const int h = img.rows, w = img.cols, cn = img.cn;
  // separable [1 2 1]/4 with reflect-101 border on each axis
  auto reflectIndex = [](int i, int n) {
    if (n == 1) return 0;
    int period = 2 * n - 2;
    int m = i % period;
    if (m < 0) m += period;
    return m < n ? m : period - m;
  };
  Mat8 out(h, w, cn);
  for (int y = 0; y < h; ++y) {
    const int ym = reflectIndex(y - 1, h), yp = reflectIndex(y + 1, h);
    for (int x = 0; x < w; ++x) {
      const int xm = reflectIndex(x - 1, w), xp = reflectIndex(x + 1, w);
      for (int c = 0; c < cn; ++c) {
        const int r0 = img.Ptr(ym)[xm * cn + c] + 2 * img.Ptr(ym)[x * cn + c] +
                       img.Ptr(ym)[xp * cn + c];
        const int r1 = img.Ptr(y)[xm * cn + c] + 2 * img.Ptr(y)[x * cn + c] +
                       img.Ptr(y)[xp * cn + c];
        const int r2 = img.Ptr(yp)[xm * cn + c] + 2 * img.Ptr(yp)[x * cn + c] +
                       img.Ptr(yp)[xp * cn + c];
        const int total = r0 + 2 * r1 + r2;
        out.Ptr(y)[x * cn + c] = static_cast<std::uint8_t>((total + 8) >> 4);
      }
    }
  }
  return out;
}

Mat8 Canny(const Mat8& gray, int low, int high, int maxClosureIters) {
  const int h = gray.rows, w = gray.cols;
  if (h < 3 || w < 3) return Mat8(h, w, 1);
  const Mat16 dx = Sobel3x3(gray, true);
  const Mat16 dy = Sobel3x3(gray, false);

  // mag with 1px zero border, matching the numpy mp array
  Mat16 mp(h + 2, w + 2);
  std::vector<std::int32_t> ax(h * w), ay15(h * w);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int sx = dx.Ptr(y)[x], sy = dy.Ptr(y)[x];
      const int mag = std::abs(sx) + std::abs(sy);
      mp.Ptr(y + 1)[x + 1] = static_cast<std::int16_t>(mag);
      ax[y * w + x] = std::abs(sx);
      ay15[y * w + x] = std::abs(sy) << 15;
    }
  }
  auto mpAt = [&](int y, int x) -> int { return mp.Ptr(Clamp(y, 0, h + 1))[Clamp(x, 0, w + 1)]; };

  std::vector<std::uint8_t> strong(h * w, 0), weak(h * w, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int center = mpAt(y + 1, x + 1);
      const int left = mpAt(y + 1, x);
      const int right = mpAt(y + 1, x + 2);
      const int up = mpAt(y, x + 1);
      const int down = mpAt(y + 2, x + 1);
      const std::int32_t a = ax[y * w + x];
      const std::int32_t y15 = ay15[y * w + x];
      const std::int32_t tg22x = a * 13573;
      const bool horiz = y15 < tg22x;
      const bool vert = !horiz && y15 > tg22x + (a << 16);
      const int sx = dx.Ptr(y)[x], sy = dy.Ptr(y)[x];
      const int s = ((sx < 0) ^ (sy < 0)) ? -1 : 1;
      const int upL = mpAt(y, x), upR = mpAt(y, x + 2);
      const int dnL = mpAt(y + 2, x), dnR = mpAt(y + 2, x + 2);
      const int diagP = s > 0 ? upL : upR;
      const int diagN = s > 0 ? dnR : dnL;
      const bool keep = center > low;
      const bool nms = (keep && horiz && center > left && center >= right) ||
                       (keep && vert && center > up && center >= down) ||
                       (keep && !horiz && !vert && center > diagP && center > diagN);
      if (nms) {
        if (center > high) strong[y * w + x] = 1;
        else weak[y * w + x] = 1;
      }
    }
  }
  return Hysteresis(strong, weak, h, w, maxClosureIters);
}

bool HoughLinesP(const Mat8& edges, double rho, double theta, int threshold,
                 int minLineLength, int maxLineGap, std::vector<Segment>* out,
                 std::FILE* trace) {
  const int h = edges.rows, w = edges.cols;
  std::vector<int> px, py;
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* row = edges.Ptr(y);
    for (int x = 0; x < w; ++x) {
      if (row[x]) { px.push_back(x); py.push_back(y); }
    }
  }
  const int n = static_cast<int>(px.size());
  if (n == 0 || out == nullptr) return false;

  const int numangle = std::max(1, static_cast<int>(std::floor(M_PI / theta + 0.5)));
  const int numrho = static_cast<int>(std::lround(((w + h) * 2 + 1) / rho));
  const int rhoOff = numrho / 2;
  std::vector<double> cosT(numangle), sinT(numangle);
  for (int i = 0; i < numangle; ++i) {
    const double angle = i * theta;
    cosT[i] = std::cos(angle) / rho;
    sinT[i] = std::sin(angle) / rho;
  }
  // R[n, k]: rho bin of pixel k at angle n (rint half-even, like numpy rint)
  Mat32 R(numangle, n);
  std::vector<std::int32_t> acc(static_cast<std::size_t>(numangle) * numrho, 0);
  for (int a = 0; a < numangle; ++a) {
    std::int32_t* rRow = R.Ptr(a);
    for (int k = 0; k < n; ++k) {
      const double v = std::rint(cosT[a] * px[k] + sinT[a] * py[k]);
      rRow[k] = static_cast<std::int32_t>(v) + rhoOff;
      ++acc[static_cast<std::size_t>(a) * numrho + rRow[k]];
    }
  }

  std::vector<std::uint8_t> alive(n, 1);
  out->clear();
  if (getenv("HOUGH_DUMP_ACC")) {
    if (std::FILE* accDump = std::fopen("hough_acc_cpp.bin", "wb")) {
      std::fwrite(acc.data(), sizeof(std::int32_t), acc.size(), accDump);
      std::fclose(accDump);
    }
  }
  if (getenv("HOUGH_DUMP_R")) {
    if (std::FILE* rDump = std::fopen("hough_r_cpp.bin", "wb")) {
      const int rows[2] = {0, numangle / 2};
      for (int a : rows) {
        std::fwrite(R.Ptr(a), sizeof(std::int32_t), n, rDump);
      }
      std::fclose(rDump);
    }
  }
  int iter = 0;
  // Lazy per-row argmax: consumption only decrements a few bins per row, so
  // a row's cached max stays valid unless its max bin itself is decremented.
  // Selection = first row (row-major, like numpy argmax on the flat array)
  // holding the global max.
  std::vector<std::int32_t> rowMaxVal(numangle, 0);
  std::vector<int> rowMaxBin(numangle, 0);
  std::vector<std::uint8_t> rowDirty(numangle, 1);
  auto recomputeRow = [&](int a) {
    const std::int32_t* row = acc.data() + static_cast<std::size_t>(a) * numrho;
    std::int32_t best = row[0];
    int bestBin = 0;
    for (int b = 1; b < numrho; ++b) {
      if (row[b] > best) { best = row[b]; bestBin = b; }
    }
    rowMaxVal[a] = best;
    rowMaxBin[a] = bestBin;
    rowDirty[a] = 0;
  };
  auto decBin = [&](int a, int bin) {
    --acc[static_cast<std::size_t>(a) * numrho + bin];
    if (bin == rowMaxBin[a]) rowDirty[a] = 1;
  };
  auto zeroBin = [&](int a, int bin) {
    acc[static_cast<std::size_t>(a) * numrho + bin] = 0;
    if (bin == rowMaxBin[a]) rowDirty[a] = 1;
  };
  for (;;) {
    for (int a = 0; a < numangle; ++a) {
      if (rowDirty[a]) recomputeRow(a);
    }
    int a = 0;
    std::int32_t bestVal = rowMaxVal[0];
    for (int ai = 1; ai < numangle; ++ai) {
      if (rowMaxVal[ai] > bestVal) { bestVal = rowMaxVal[ai]; a = ai; }
    }
    const int r = rowMaxBin[a];
    if (acc[static_cast<std::size_t>(a) * numrho + r] < threshold) {
      if (trace) {
        std::fprintf(trace, "STOP iter=%d n=%d r=%d votes=%d (<%d)\n", iter, a,
                     r, acc[static_cast<std::size_t>(a) * numrho + r], threshold);
      }
      break;
    }
    std::vector<int> cand;
    const std::int32_t* rRow = R.Ptr(a);
    for (int k = 0; k < n; ++k) {
      if (alive[k] && std::abs(rRow[k] - r) <= 1) cand.push_back(k);
    }
    if (trace) {
      std::fprintf(trace, "iter=%d n=%d r=%d votes=%d cand=%zu\n", iter, a, r,
                   acc[static_cast<std::size_t>(a) * numrho + r], cand.size());
    }
    if (cand.empty()) {
      zeroBin(a, r);
      continue;
    }
    const bool useX = std::abs(cosT[a]) >= std::abs(sinT[a]);
    std::vector<double> proj(cand.size());
    for (std::size_t i = 0; i < cand.size(); ++i) {
      proj[i] = useX ? px[cand[i]] : py[cand[i]];
    }
    std::vector<int> order(cand.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int i, int j) { return proj[i] < proj[j]; });
    {
      // permute into NEW vectors: an in-place write would clobber entries
      // still needed by later indices (Python's cand = cand[order])
      std::vector<int> sortedCand(cand.size());
      std::vector<double> sortedProj(cand.size());
      for (std::size_t i = 0; i < cand.size(); ++i) {
        sortedCand[i] = cand[order[i]];
        sortedProj[i] = proj[order[i]];
      }
      cand = std::move(sortedCand);
      proj = std::move(sortedProj);
    }
    bool consumed = false;
    std::size_t s0 = 0;
    for (std::size_t i = 1; i <= cand.size(); ++i) {
      const bool isBreak = i == cand.size() ||
          (proj[i] - proj[i - 1]) > maxLineGap;
      if (!isBreak) continue;
      const std::size_t s1 = i;
      const std::size_t runLen = s1 - s0;
      if (runLen >= 2) {
        const int first = cand[s0], last = cand[s1 - 1];
        const int dxLen = std::abs(px[last] - px[first]);
        const int dyLen = std::abs(py[last] - py[first]);
        if (dxLen >= minLineLength || dyLen >= minLineLength) {
          if (trace) {
            std::fprintf(trace, "  seg (%d,%d)-(%d,%d) run=%zu\n", px[first],
                         py[first], px[last], py[last], runLen);
          }
          out->push_back({px[first], py[first], px[last], py[last]});
          for (std::size_t j = s0; j < s1; ++j) alive[cand[j]] = 0;
          consumed = true;
          // consume votes once per (angle, pixel rho bin)
          for (int a2 = 0; a2 < numangle; ++a2) {
            const std::int32_t* rr = R.Ptr(a2);
            for (std::size_t j = s0; j < s1; ++j) decBin(a2, rr[cand[j]]);
          }
        }
      }
      s0 = s1;
    }
    if (!consumed) {
      if (trace) std::fprintf(trace, "  cleared r-1..r+1\n");
      for (int rr = std::max(0, r - 1); rr <= r + 1 && rr < numrho; ++rr) {
        zeroBin(a, rr);
      }
    }
    ++iter;
    if (getenv("HOUGH_DUMP_ALIVE_N")) {
      static const int dumpAt = atoi(getenv("HOUGH_DUMP_ALIVE_N"));
      if (iter == dumpAt) {
        if (std::FILE* aDump = std::fopen("hough_alive_cpp.bin", "wb")) {
          for (int k = 0; k < n; ++k) {
            std::uint8_t b = alive[k] ? 1 : 0;
            std::fwrite(&b, 1, 1, aDump);
          }
          std::fclose(aDump);
        }
      }
    }
    if (getenv("HOUGH_DUMP_ACC")) {
      static const int dumpAt = getenv("HOUGH_DUMP_ACC_N")
          ? atoi(getenv("HOUGH_DUMP_ACC_N")) : 1;
      if (iter == dumpAt) {
        if (std::FILE* accDump = std::fopen("hough_acc_cpp.bin", "wb")) {
          std::fwrite(acc.data(), sizeof(std::int32_t), acc.size(), accDump);
          std::fclose(accDump);
        }
      }
    }
  }
  return !out->empty();
}

bool HoughCircles(const Mat8& image, double dp, double minDist, int param1,
                  int param2, int minRadius, int maxRadius, bool binary,
                  std::vector<Circle>* out) {
  if (out == nullptr) return false;
  out->clear();
  const int h = image.rows, w = image.cols;
  std::vector<int> xs, ys;
  if (binary) {
    // cv2.Canny of a 0/255 mask marks both sides of every boundary step:
    // take the two-pixel ring.
    Kernel k3(9, 1);
    Mat8 m255(h, w, 1);
    for (std::size_t i = 0; i < image.a.size(); ++i) m255.a[i] = image.a[i] > 0 ? 255 : 0;
    const Mat8 inner1Full = Erode(m255, k3);
    Mat8 inner1m(h, w, 1);
    for (std::size_t i = 0; i < inner1m.a.size(); ++i) inner1m.a[i] = inner1Full.a[i] > 0 ? 255 : 0;
    const Mat8 inner2Full = Erode(inner1m, k3);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const bool mask = image.a[y * w + x] > 0;
        const bool i1 = inner1Full.a[y * w + x] > 0;
        const bool i2 = inner2Full.a[y * w + x] > 0;
        if ((mask && !i1) || (i1 && !i2)) { xs.push_back(x); ys.push_back(y); }
      }
    }
  } else {
    const Mat8 edges = Canny(image, std::max(1, param1 / 2), param1, 60);
    for (int y = 0; y < h; ++y) {
      const std::uint8_t* row = edges.Ptr(y);
      for (int x = 0; x < w; ++x) {
        if (row[x]) { xs.push_back(x); ys.push_back(y); }
      }
    }
  }
  if (xs.empty()) return false;

  // keep only pixels with sufficient gradient magnitude
  const Mat16 sobX = Sobel3x3(image, true);
  const Mat16 sobY = Sobel3x3(image, false);
  std::vector<int> exs, eys;
  std::vector<double> gx, gy, gm;
  exs.reserve(xs.size());
  eys.reserve(xs.size());
  for (std::size_t i = 0; i < xs.size(); ++i) {
    const double ddx = sobX.Ptr(ys[i])[xs[i]];
    const double ddy = sobY.Ptr(ys[i])[xs[i]];
    const double mm = std::hypot(ddx, ddy);
    if (mm >= 1.0) {
      exs.push_back(xs[i]); eys.push_back(ys[i]);
      gx.push_back(ddx); gy.push_back(ddy); gm.push_back(mm);
    }
  }
  if (exs.empty()) return false;

  const double idp = 1.0 / std::max(dp, 1.0);
  const int acols = static_cast<int>(std::ceil(w * idp));
  const int arows = static_cast<int>(std::ceil(h * idp));
  const int astep = acols + 2;
  std::vector<std::int32_t> acc(static_cast<std::size_t>(arows + 2) * astep, 0);
  for (int sign = 1; sign >= -1; sign -= 2) {
    for (int radius = minRadius; radius <= maxRadius; ++radius) {
      for (std::size_t k = 0; k < exs.size(); ++k) {
        const double nx = gx[k] / gm[k] * idp;
        const double ny = gy[k] / gm[k] * idp;
        const double pxv = exs[k] * idp;
        const double pyv = eys[k] * idp;
        const std::int64_t cx = Rint(pxv + sign * nx * radius);
        const std::int64_t cy = Rint(pyv + sign * ny * radius);
        if (cx >= 0 && cx < acols && cy >= 0 && cy < arows) {
          ++acc[static_cast<std::size_t>((cy + 1) * astep + (cx + 1))];
        }
      }
    }
  }

  // 3x3 NMS peaks above param2
  std::vector<std::pair<int, int>> peaks;  // (cy, cx) accumulator coords
  std::vector<int> peakVals;
  for (int cy = 1; cy <= arows; ++cy) {
    for (int cx = 1; cx <= acols; ++cx) {
      const std::size_t o = static_cast<std::size_t>(cy) * astep + cx;
      const int core = acc[o];
      if (core > param2 && core >= acc[o - 1] && core >= acc[o + 1] &&
          core >= acc[o - astep] && core > acc[o + astep]) {
        peaks.emplace_back(cy, cx);
        peakVals.push_back(core);
      }
    }
  }
  if (peaks.empty()) return false;
  std::vector<int> order(peaks.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int i, int j) {
    return peakVals[i] > peakVals[j];
  });
  const double minDist2 = minDist * minDist;
  std::vector<std::pair<double, double>> centres;
  for (int oi : order) {
    const double cx = (peaks[oi].second - 1 + 0.5) * dp;
    const double cy = (peaks[oi].first - 1 + 0.5) * dp;
    bool ok = true;
    for (const auto& c : centres) {
      if ((cx - c.first) * (cx - c.first) + (cy - c.second) * (cy - c.second) <
          minDist2) {
        ok = false;
        break;
      }
    }
    if (ok) centres.emplace_back(cx, cy);
  }
  if (centres.empty()) return false;

  const double r2Lo = static_cast<double>(minRadius) * minRadius;
  const double r2Hi = static_cast<double>(maxRadius) * maxRadius;
  const double dr = std::max(dp, 1.0);
  const int nBinsPerDr = 10;
  const int nBins =
      std::max(1, static_cast<int>(std::lround((maxRadius - minRadius) / dr * nBinsPerDr)));
  for (const auto& [cx, cy] : centres) {
    std::vector<double> dist;
    std::vector<int> binIdxAll;  // bin index per supported pixel
    std::vector<int> pixIdx;
    for (std::size_t k = 0; k < exs.size(); ++k) {
      const double d2 = (exs[k] - cx) * (exs[k] - cx) + (eys[k] - cy) * (eys[k] - cy);
      if (d2 >= r2Lo && d2 <= r2Hi) {
        dist.push_back(std::sqrt(d2));
        pixIdx.push_back(static_cast<int>(k));
      }
    }
    if (dist.empty()) continue;
    std::vector<int> binIdx(dist.size());
    std::vector<int> bins(nBins, 0);
    for (std::size_t i = 0; i < dist.size(); ++i) {
      int b = static_cast<int>((dist[i] - minRadius) / dr * nBinsPerDr);
      b = Clamp(b, 0, nBins - 1);
      binIdx[i] = b;
      ++bins[b];
    }
    int upbin = 0, maxCount = -1;
    for (int i = 0; i < nBins; ++i) {
      int window = 0;
      for (int j = std::max(0, i - nBinsPerDr + 1); j <= i; ++j) window += bins[j];
      if (window > maxCount) { maxCount = window; upbin = i; }
    }
    if (maxCount <= param2) continue;
    double sumR = 0.0;
    int nSup = 0;
    for (std::size_t i = 0; i < binIdx.size(); ++i) {
      if (binIdx[i] > upbin - nBinsPerDr && binIdx[i] <= upbin) {
        sumR += dist[i];
        ++nSup;
      }
    }
    if (nSup == 0) continue;
    double fx = cx, fy = cy, fr = sumR / nSup;
    // two Kasa circle fits (normal equations; documented deviation from SVD)
    for (int iter = 0; iter < 2; ++iter) {
      std::vector<double> pxs, pys;
      for (std::size_t k = 0; k < exs.size(); ++k) {
        const double d2 = (exs[k] - cx) * (exs[k] - cx) + (eys[k] - cy) * (eys[k] - cy);
        if (d2 < r2Lo || d2 > r2Hi) continue;
        const double selD = std::hypot(exs[k] - fx, eys[k] - fy);
        if (std::abs(selD - fr) <= std::max(1.5, 0.06 * fr)) {
          pxs.push_back(exs[k]);
          pys.push_back(eys[k]);
        }
      }
      if (pxs.size() < 6) break;
      // Kasa: minimise sum (x^2+y^2 + A x + B y + C)^2
      double Sxx = 0, Sxy = 0, Sx = 0, Syy = 0, Sy = 0, S1 = 0;
      double Sxz = 0, Syz = 0, Sz = 0;
      for (std::size_t i = 0; i < pxs.size(); ++i) {
        const double X = pxs[i], Y = pys[i];
        const double Z = X * X + Y * Y;
        Sxx += X * X; Sxy += X * Y; Sx += X;
        Syy += Y * Y; Sy += Y; S1 += 1;
        Sxz += X * Z; Syz += Y * Z; Sz += Z;
      }
      const double M[3][4] = {{Sxx, Sxy, Sx, Sxz}, {Sxy, Syy, Sy, Syz}, {Sx, Sy, S1, Sz}};
      double sol[3];
      {
        double A[3][4];
        std::memcpy(A, M, sizeof(A));
        bool singular = false;
        for (int col = 0; col < 3; ++col) {
          int piv = col;
          for (int r = col + 1; r < 3; ++r) {
            if (std::abs(A[r][col]) > std::abs(A[piv][col])) piv = r;
          }
          if (std::abs(A[piv][col]) < 1e-12) { singular = true; break; }
          std::swap(A[col], A[piv]);
          for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double f = A[r][col] / A[col][col];
            for (int c2 = col; c2 < 4; ++c2) A[r][c2] -= f * A[col][c2];
          }
        }
        if (singular) break;
        for (int i = 0; i < 3; ++i) sol[i] = A[i][3] / A[i][i];
      }
      const double nfx = -sol[0] / 2.0, nfy = -sol[1] / 2.0;
      const double inner = nfx * nfx + nfy * nfy - sol[2];
      const double nr = std::sqrt(std::max(inner, 0.0));
      if (!(minRadius - 2 <= nr && nr <= maxRadius + 2)) break;
      if (std::hypot(nfx - fx, nfy - fy) > 0.5 * fr) break;
      fx = nfx; fy = nfy; fr = nr;
    }
    out->push_back({static_cast<float>(fx), static_cast<float>(fy),
                    static_cast<float>(fr)});
  }
  return !out->empty();
}

bool ConnectedComponentsWithStats(const Mat8& mask, CcResult* result) {
  if (result == nullptr) return false;
  const int h = mask.rows, w = mask.cols;
  result->labels = Mat32(h, w);
  std::vector<int> parent(1, 0);
  auto find = [&parent](int a) {
    int root = a;
    while (parent[root] != root) root = parent[root];
    while (parent[a] != root) {
      const int next = parent[a];
      parent[a] = root;
      a = next;
    }
    return root;
  };
  struct Run { int y, sx, ex, label; };
  std::vector<Run> runRows;
  struct Prev { int sx, ex, label; };
  std::vector<Prev> prev;
  int nextLabel = 1;
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* row = mask.Ptr(y);
    bool any = false;
    for (int x = 0; x < w; ++x) { if (row[x]) { any = true; break; } }
    if (!any) { prev.clear(); continue; }
    std::vector<Prev> cur;
    int x = 0;
    while (x < w) {
      if (!row[x]) { ++x; continue; }
      const int sx = x;
      while (x < w && row[x]) ++x;
      const int ex = x;  // exclusive
      int label = 0;
      for (const auto& p : prev) {
        if (p.sx < ex + 1 && sx < p.ex + 1) {  // 8-connectivity
          if (label == 0) {
            label = p.label;
          } else if (label != p.label) {
            const int ra = find(label), rb = find(p.label);
            if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
          }
        }
      }
      if (label == 0) {
        label = nextLabel;
        parent.push_back(nextLabel);
        ++nextLabel;
      }
      cur.push_back({sx, ex, label});
      runRows.push_back({y, sx, ex, label});
    }
    prev = std::move(cur);
  }

  if (runRows.empty()) {
    result->count = 0;
    result->labels = Mat32(h, w);
    result->stats.assign(1, {0, 0, 0, 0, 0});
    result->centroids.assign(1, {0.0, 0.0});
    return true;
  }

  std::map<int, int> ids;
  std::vector<Run> resolved(runRows.size());
  for (std::size_t i = 0; i < runRows.size(); ++i) {
    const int root = find(runRows[i].label);
    auto it = ids.find(root);
    int newId;
    if (it == ids.end()) {
      newId = static_cast<int>(ids.size()) + 1;
      ids.emplace(root, newId);
    } else {
      newId = it->second;
    }
    resolved[i] = {runRows[i].y, runRows[i].sx, runRows[i].ex, newId};
  }
  const int count = static_cast<int>(ids.size()) + 1;  // + background

  for (const auto& r : resolved) {
    std::int32_t* lrow = result->labels.Ptr(r.y);
    for (int x = r.sx; x < r.ex; ++x) lrow[x] = r.label;
  }

  result->stats.assign(count, {0, 0, w, h, 0});
  result->centroids.assign(count, {0.0, 0.0});
  std::vector<double> area(count, 0.0), sumX(count, 0.0), sumY(count, 0.0);
  std::vector<std::int32_t> x0(count, 1 << 30), x1(count, 0);
  std::vector<std::int32_t> y0(count, 1 << 30), y1(count, 0);
  for (const auto& r : resolved) {
    const int len = r.ex - r.sx;
    const int id = r.label;
    area[id] += len;
    x0[id] = std::min(x0[id], r.sx);
    x1[id] = std::max(x1[id], r.ex);
    y0[id] = std::min(y0[id], r.y);
    y1[id] = std::max(y1[id], r.y);
    sumX[id] += (r.sx + r.ex - 1) * len / 2.0;  // association matches numpy
    sumY[id] += static_cast<double>(r.y) * len;
  }
  std::int64_t totalArea = 0;
  for (int id = 1; id < count; ++id) {
    auto& s = result->stats[id];
    s[kCcLeft] = x0[id];
    s[kCcTop] = y0[id];
    s[kCcWidth] = x1[id] - x0[id];
    s[kCcHeight] = y1[id] - y0[id] + 1;
    s[kCcArea] = static_cast<std::int32_t>(area[id]);
    result->centroids[id][0] = sumX[id] / area[id];
    result->centroids[id][1] = sumY[id] / area[id];
    totalArea += static_cast<std::int64_t>(area[id]);
  }
  // background row: whole-image bbox + remaining pixel count (cv2 semantics)
  auto& bg = result->stats[0];
  bg[kCcLeft] = 0;
  bg[kCcTop] = 0;
  bg[kCcWidth] = w;
  bg[kCcHeight] = h;
  bg[kCcArea] = static_cast<std::int32_t>(static_cast<std::int64_t>(w) * h - totalArea);
  result->count = count;
  return true;
}

}  // namespace imgproc
