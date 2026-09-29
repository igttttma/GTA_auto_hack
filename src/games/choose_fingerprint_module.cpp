#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "games.h"
#include "../capture/game_window.h"
#include "../input/key_input.h"
#ifdef CLI_TEST
#include <olectl.h>
#include <gdiplus.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace gta5::games::choose_fingerprint {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Region {
    Rect rect;
    int pixels = 0;
    double aspect = 1.0;
    double cx = 0.0, cy = 0.0;
};

struct Frame {
    int x = 0, y = 0, w = 0, h = 0;
    int screenW = 0, screenH = 0;
    double toScreenX = 1.0, toScreenY = 1.0;
    std::uint64_t windowGeneration = 0;
    std::vector<uint8_t> bgra;
    std::vector<uint8_t> gray;
};

struct TitleBars {
    Rect timer, target, components, signals;
    bool hasTimer = false, hasTarget = false, hasComponents = false, hasSignals = false;
};

struct RoiInfo {
    bool isMinigame = false;
    Rect panel;
    TitleBars bars;
};

struct InGameGeometry {
    bool valid = false;
    std::uint64_t windowGeneration = 0;
    int frameW = 0;
    int frameH = 0;
    RoiInfo roi;
    Rect target;
    std::vector<Rect> components;
};

static InGameGeometry gDetectedGeometry;

struct BlockInfo {
    int index = 0;
    Rect rect;
    double score = 0.0;
    bool correct = false;
    bool selected = false;
    bool cursor = false;
    int cursorScore = 0;
};

struct OverlayState {
    bool visible = false;
    uint64_t targetHash = 0;
    Rect target;
    int levelMarker = -1;
    Rect levelMarkerLine;
    std::vector<BlockInfo> blocks;
};

struct SolverCache {
    bool valid = false;
    uint64_t targetHash = 0;
    Rect target;
    std::vector<BlockInfo> baseBlocks;
};

enum class AutomationPhase {
    Idle,
    Selecting,
    VerifyDelay,
    Submitting,
    WaitingLevel,
};

struct AutomationState {
    AutomationPhase phase = AutomationPhase::Idle;
    uint64_t plannedHash = 0;
    int plannedLevelMarker = -1;
    int observedLevelMarker = -1;
    int levelChangeFrames = 0;
    std::chrono::steady_clock::time_point verifyAfter{};
    std::chrono::steady_clock::time_point submittedAt{};
    gta5::input::Job inputJob;
};

struct FrameTiming {
    double captureMs = 0.0;
    double gateMs = 0.0;
    double roiMs = 0.0;
    double hashMs = 0.0;
    double answerMs = 0.0;
    double stateMs = 0.0;
    double analyzeMs = 0.0;
    double autoMs = 0.0;
    double publishMs = 0.0;
    double totalMs = 0.0;
    bool cacheHit = false;
    bool minigame = false;
};

static HWND gMainWnd = nullptr;
static HWND gOverlayWnd = nullptr;
static HWND gLogList = nullptr;
static HWND gStatusText = nullptr;
static std::atomic<bool> gRunning{false};
static std::atomic<bool> gStopping{false};
static std::thread gWorker;
static CRITICAL_SECTION gStateLock;
static OverlayState gState;
static int gVirtualX = 0, gVirtualY = 0, gVirtualW = 0, gVirtualH = 0;
static DWORD gUiThreadId = 0;
static constexpr UINT WM_APP_LOG = WM_APP + 1;
static constexpr UINT WM_APP_WORKER_STOPPED = WM_APP + 2;
static constexpr DWORD kFrameDelayMs = 10;

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static std::string timingText(const FrameTiming& t) {
    char buf[256];
    std::snprintf(
        buf,
        sizeof(buf),
        "dt=%.1fms cap=%.1f ana=%.1f gate=%.1f roi=%.1f hash=%.1f ans=%.1f state=%.1f auto=%.1f pub=%.1f",
        t.totalMs,
        t.captureMs,
        t.analyzeMs,
        t.gateMs,
        t.roiMs,
        t.hashMs,
        t.answerMs,
        t.stateMs,
        t.autoMs,
        t.publishMs
    );
    return buf;
}

static std::string correctList(const OverlayState& s) {
    std::string correct;
    for (const auto& b : s.blocks) {
        if (b.correct) {
            if (!correct.empty()) correct += ",";
            correct += std::to_string(b.index);
        }
    }
    return correct;
}

static int oddKernel(int h, int w, double ratio, int minimum = 3) {
    int s = std::max(minimum, (int)std::lround(std::min(h, w) * ratio));
    return (s % 2) ? s : s + 1;
}

static Rect clampRect(Rect r, int w, int h) {
    r.x = std::max(0, r.x);
    r.y = std::max(0, r.y);
    r.w = std::max(0, std::min(r.w, w - r.x));
    r.h = std::max(0, std::min(r.h, h - r.y));
    return r;
}

static Rect scaleRectToScreen(const Frame& f, Rect r) {
    int x1 = f.x + (int)std::lround(r.x * f.toScreenX);
    int y1 = f.y + (int)std::lround(r.y * f.toScreenY);
    int x2 = f.x + (int)std::lround((r.x + r.w) * f.toScreenX);
    int y2 = f.y + (int)std::lround((r.y + r.h) * f.toScreenY);
    x1 = std::clamp(x1, f.x, f.x + f.screenW);
    y1 = std::clamp(y1, f.y, f.y + f.screenH);
    x2 = std::clamp(x2, f.x, f.x + f.screenW);
    y2 = std::clamp(y2, f.y, f.y + f.screenH);
    return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

static OverlayState scaleOverlayStateToScreen(const Frame& f, OverlayState s) {
    s.target = scaleRectToScreen(f, s.target);
    if (s.levelMarker >= 0) {
        s.levelMarkerLine = scaleRectToScreen(f, s.levelMarkerLine);
    }
    for (auto& b : s.blocks) {
        b.rect = scaleRectToScreen(f, b.rect);
    }
    return s;
}

static Rect padRect(Rect r, int w, int h, double ratio) {
    int pad = (int)std::lround(std::max(r.w, r.h) * ratio);
    return clampRect({r.x - pad, r.y - pad, r.w + pad * 2, r.h + pad * 2}, w, h);
}

static int scaledPx(int frameW, int frameH, int px1080) {
    (void)frameW;
    return std::max(1, (int)std::lround(px1080 * (frameH / 1080.0)));
}

static int scaledPx(const Frame& f, int px1080) {
    return scaledPx(f.w, f.h, px1080);
}

static int scaledArea(const Frame& f, int area1080) {
    const double scale = f.h / 1080.0;
    return std::max(1, (int)std::lround(area1080 * scale * scale));
}

static std::wstring widenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

static void postLog(const std::string& s) {
    if (!gMainWnd) return;
    PostMessageW(gMainWnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(widenAscii(s)));
}

static std::string rectText(Rect r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%d,%d %dx%d)", r.x, r.y, r.w, r.h);
    return buf;
}

static int gradientAt(const Frame& f, int x, int y) {
    x = std::clamp(x, 1, f.w - 2);
    y = std::clamp(y, 1, f.h - 2);
    const int dx = std::abs((int)f.gray[y * f.w + x + 1] - (int)f.gray[y * f.w + x - 1]);
    const int dy = std::abs((int)f.gray[(y + 1) * f.w + x] - (int)f.gray[(y - 1) * f.w + x]);
    return dx + dy;
}

static bool isFlashingStyleWhite(uint8_t red, uint8_t green, uint8_t blue) {
    return red > 175 && green > 175 && blue > 175
        && std::abs((int)red - green) < 55
        && std::abs((int)red - blue) < 55;
}

static std::vector<uint8_t> uiWhiteMask(const Frame& f) {
    std::vector<uint8_t> mask(static_cast<size_t>(f.w) * f.h, 0);
    if (f.bgra.size() != mask.size() * 4) return mask;
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = isFlashingStyleWhite(f.bgra[i * 4 + 2], f.bgra[i * 4 + 1], f.bgra[i * 4]) ? 1 : 0;
    }
    return mask;
}

static std::vector<uint8_t> dilate(const std::vector<uint8_t>& src, int w, int h, int k) {
    int r = k / 2;
    std::vector<uint8_t> tmp(w * h), dst(w * h);
    for (int y = 0; y < h; ++y) {
        int sum = 0;
        for (int x = -r; x <= r; ++x) if (0 <= x && x < w) sum += src[y * w + x];
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = sum > 0;
            int oldx = x - r;
            int newx = x + r + 1;
            if (0 <= oldx && oldx < w) sum -= src[y * w + oldx];
            if (0 <= newx && newx < w) sum += src[y * w + newx];
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -r; y <= r; ++y) if (0 <= y && y < h) sum += tmp[y * w + x];
        for (int y = 0; y < h; ++y) {
            dst[y * w + x] = sum > 0;
            int oldy = y - r;
            int newy = y + r + 1;
            if (0 <= oldy && oldy < h) sum -= tmp[oldy * w + x];
            if (0 <= newy && newy < h) sum += tmp[newy * w + x];
        }
    }
    return dst;
}

static std::vector<uint8_t> erode(const std::vector<uint8_t>& src, int w, int h, int k) {
    int r = k / 2;
    std::vector<uint8_t> tmp(w * h), dst(w * h);
    for (int y = 0; y < h; ++y) {
        int sum = 0, cnt = 0;
        for (int x = -r; x <= r; ++x) if (0 <= x && x < w) { sum += src[y * w + x]; cnt++; }
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = sum == cnt;
            int oldx = x - r;
            int newx = x + r + 1;
            if (0 <= oldx && oldx < w) { sum -= src[y * w + oldx]; cnt--; }
            if (0 <= newx && newx < w) { sum += src[y * w + newx]; cnt++; }
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0, cnt = 0;
        for (int y = -r; y <= r; ++y) if (0 <= y && y < h) { sum += tmp[y * w + x]; cnt++; }
        for (int y = 0; y < h; ++y) {
            dst[y * w + x] = sum == cnt;
            int oldy = y - r;
            int newy = y + r + 1;
            if (0 <= oldy && oldy < h) { sum -= tmp[oldy * w + x]; cnt--; }
            if (0 <= newy && newy < h) { sum += tmp[newy * w + x]; cnt++; }
        }
    }
    return dst;
}

static std::vector<uint8_t> closeMask(std::vector<uint8_t> m, int w, int h, int k, int iters = 1) {
    for (int i = 0; i < iters; ++i) {
        m = dilate(m, w, h, k);
        m = erode(m, w, h, k);
    }
    return m;
}

static std::vector<uint8_t> openMask(std::vector<uint8_t> m, int w, int h, int k, int iters = 1) {
    for (int i = 0; i < iters; ++i) {
        m = erode(m, w, h, k);
        m = dilate(m, w, h, k);
    }
    return m;
}

static void bridgeHorizontalGaps(std::vector<uint8_t>& mask, int w, int h, int maxGap) {
    for (int y = 0; y < h; ++y) {
        int previous = -1;
        for (int x = 0; x < w; ++x) {
            if (!mask[y * w + x]) continue;
            if (previous >= 0 && x - previous - 1 <= maxGap) {
                std::fill(mask.begin() + y * w + previous + 1, mask.begin() + y * w + x, 1);
            }
            previous = x;
        }
    }
}

static std::vector<Region> connectedRegions(const std::vector<uint8_t>& mask, int w, int h, int minPixels) {
    std::vector<uint8_t> work = mask;
    std::vector<Region> regions;
    std::vector<int> q;
    q.reserve(4096);

    for (int start = 0; start < w * h; ++start) {
        if (!work[start]) continue;
        int sx = start % w, sy = start / w;
        int minx = sx, maxx = sx, miny = sy, maxy = sy, pixels = 0;
        q.clear();
        q.push_back(start);
        work[start] = 0;

        for (size_t qi = 0; qi < q.size(); ++qi) {
            int p = q[qi], x = p % w, y = p / w;
            pixels++;
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);

            for (int dy = -1; dy <= 1; ++dy) {
                int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx;
                    if ((dx == 0 && dy == 0) || nx < 0 || nx >= w) continue;
                    int np = ny * w + nx;
                    if (work[np]) {
                        work[np] = 0;
                        q.push_back(np);
                    }
                }
            }
        }

        if (pixels >= minPixels) {
            Rect r{minx, miny, maxx - minx + 1, maxy - miny + 1};
            regions.push_back({r, pixels, (double)r.w / std::max(1, r.h), r.x + r.w / 2.0, r.y + r.h / 2.0});
        }
    }
    return regions;
}

static bool captureScreen(Frame& out) {
    gta5::capture::GameFrame captured;
    if (!gta5::capture::CaptureGameFrame(captured)) return false;
    gVirtualX = captured.screenX;
    gVirtualY = captured.screenY;
    gVirtualW = captured.screenW;
    gVirtualH = captured.screenH;
    out.x = captured.screenX; out.y = captured.screenY;
    out.w = captured.width; out.h = captured.height;
    out.screenW = captured.screenW; out.screenH = captured.screenH;
    out.toScreenX = captured.toScreenX;
    out.toScreenY = captured.toScreenY;
    out.windowGeneration = captured.windowGeneration;
    const uint8_t* px = reinterpret_cast<const uint8_t*>(captured.bgra.data());
    out.bgra.assign(px, px + static_cast<size_t>(out.w) * out.h * 4);
    out.gray.resize(static_cast<size_t>(out.w) * out.h);
    for (int i = 0; i < out.w * out.h; ++i) {
        uint8_t b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
        out.gray[i] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
    }
    return true;
}

static bool barMatches(const Region& b, Rect panel, double xmin, double xmax, double ymin, double ymax, double wmin, double wmax) {
    double rx = (b.rect.x - panel.x) / (double)panel.w;
    double ry = (b.rect.y - panel.y) / (double)panel.h;
    double rw = b.rect.w / (double)panel.w;
    return xmin <= rx && rx <= xmax && ymin <= ry && ry <= ymax && wmin <= rw && rw <= wmax;
}

static bool isTitleBarCandidate(const Frame& f, const Region& rg) {
    Rect r = rg.rect;
    double asp = r.w / (double)std::max(1, r.h);
    return r.w > scaledPx(f, 269)
        && r.h >= scaledPx(f, 7)
        && r.h <= scaledPx(f, 48)
        && asp > 5.0
        && rg.cy > f.h * 0.04
        && rg.cy < f.h * 0.86;
}

static bool extractTitleStripFromTallRegion(const Frame& f, const std::vector<uint8_t>& bright, const Region& rg, Region& out) {
    Rect r = rg.rect;
    if (!(r.w > scaledPx(f, 269) && r.h > scaledPx(f, 48) && r.h < scaledPx(f, 140))) return false;
    if (!(rg.cy > f.h * 0.04 && rg.cy < f.h * 0.86)) return false;

    const int rowOn = std::max(4, (int)std::lround(r.w * 0.012));
    const int maxGap = scaledPx(f, 4);
    const int maxSearchH = std::min(r.h, scaledPx(f, 62));
    int start = -1, end = -1, gap = 0;

    for (int yy = 0; yy < maxSearchH; ++yy) {
        int y = r.y + yy;
        int count = 0;
        const uint8_t* row = bright.data() + y * f.w + r.x;
        for (int x = 0; x < r.w; ++x) count += row[x] ? 1 : 0;

        if (count >= rowOn) {
            if (start < 0) start = yy;
            end = yy;
            gap = 0;
        } else if (start >= 0 && ++gap > maxGap) {
            break;
        }
    }

    if (start < 0 || end < start) return false;
    int y0 = r.y + start;
    int y1 = r.y + end;
    int minx = r.x + r.w, maxx = r.x - 1, pixels = 0;
    for (int y = y0; y <= y1; ++y) {
        const uint8_t* row = bright.data() + y * f.w;
        for (int x = r.x; x < r.x + r.w; ++x) {
            if (!row[x]) continue;
            minx = std::min(minx, x);
            maxx = std::max(maxx, x);
            ++pixels;
        }
    }
    if (maxx < minx) return false;

    Rect strip{minx, y0, maxx - minx + 1, y1 - y0 + 1};
    Region candidate{strip, pixels, strip.w / (double)std::max(1, strip.h), strip.x + strip.w / 2.0, strip.y + strip.h / 2.0};
    if (!isTitleBarCandidate(f, candidate)) return false;
    out = candidate;
    return true;
}

static std::vector<Region> findTitleBarsByRuns(const Frame& f, const std::vector<uint8_t>& bright) {
    const int minRun = scaledPx(f, 269);
    const int barHeight = scaledPx(f, 24);
    std::vector<Region> candidates;
    for (int y = scaledPx(f, 40); y < (int)std::lround(f.h * 0.86); y += 2) {
        int start = -1;
        for (int x = 0; x <= f.w; ++x) {
            const bool on = x < f.w && bright[y * f.w + x];
            if (on && start < 0) {
                start = x;
            } else if (!on && start >= 0) {
                if (x - start >= minRun) {
                    Rect rect = clampRect({start, y - scaledPx(f, 5), x - start, barHeight}, f.w, f.h);
                    Region candidate{rect, rect.w * rect.h, rect.w / (double)std::max(1, rect.h),
                                     rect.x + rect.w / 2.0, rect.y + rect.h / 2.0};
                    if (isTitleBarCandidate(f, candidate)) candidates.push_back(candidate);
                }
                start = -1;
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Region& a, const Region& b) {
        return a.rect.w > b.rect.w;
    });
    std::vector<Region> bars;
    for (const auto& candidate : candidates) {
        bool duplicate = false;
        for (const auto& kept : bars) {
            const int overlap = std::min(candidate.rect.x + candidate.rect.w, kept.rect.x + kept.rect.w)
                - std::max(candidate.rect.x, kept.rect.x);
            if (std::abs(candidate.cy - kept.cy) <= scaledPx(f, 24)
                && overlap > std::min(candidate.rect.w, kept.rect.w) * 0.45) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) bars.push_back(candidate);
    }
    return bars;
}

static RoiInfo detectMinigame(const Frame& f, std::string* diag = nullptr) {
    RoiInfo info;
    // Keep this gate aligned with flashing_module, whose white-bar detector is
    // known to work on users' HDR desktops. Geometry below remains specific to
    // the fingerprint layout.
    auto bright = uiWhiteMask(f);
    bridgeHorizontalGaps(bright, f.w, f.h, scaledPx(f, 18));
    int k = oddKernel(f.h, f.w, 0.0028);
    bright = closeMask(std::move(bright), f.w, f.h, k);
    auto regs = connectedRegions(bright, f.w, f.h, scaledArea(f, 1659));

    std::vector<Region> bars = findTitleBarsByRuns(f, bright);
    for (const auto& rg : regs) {
        if (isTitleBarCandidate(f, rg)) {
            bars.push_back(rg);
            continue;
        }
        Region strip;
        if (extractTitleStripFromTallRegion(f, bright, rg, strip)) {
            bars.push_back(strip);
        }
    }

    auto right = [](Rect r) { return r.x + r.w; };
    auto bottom = [](Rect r) { return r.y + r.h; };
    auto closeEnough = [](double a, double b, double limit) { return std::abs(a - b) <= limit; };

    int bestScore = -1000000000;
    TitleBars bestBars;
    Rect bestPanel{};
    for (const auto& target : bars) {
        if (!(target.rect.w > scaledPx(f, 461) && target.rect.w < scaledPx(f, 1056))) continue;
        if (!(target.cy > f.h * 0.05 && target.cy < f.h * 0.20)) continue;

        for (const auto& components : bars) {
            if (&components == &target) continue;
            const double componentWidthRatio = components.rect.w / (double)target.rect.w;
            if (!(target.cx - components.cx > target.rect.w * 0.70 &&
                  componentWidthRatio > 0.45 && componentWidthRatio < 1.05)) continue;
            if (!(components.cy > target.cy + f.h * 0.07 && components.cy < target.cy + f.h * 0.23)) continue;

            for (const auto& signals : bars) {
                if (&signals == &target || &signals == &components) continue;
                if (!(signals.cy > components.cy + f.h * 0.35 && signals.cy < f.h * 0.84)) continue;
                const double pairedWidth = (target.rect.w + signals.rect.w) * 0.5;
                if (!closeEnough(signals.cx, target.cx, pairedWidth * 0.12)) continue;
                if (!closeEnough(signals.rect.w, target.rect.w, pairedWidth * 0.15)) continue;
                if (!closeEnough(signals.rect.x, target.rect.x, pairedWidth * 0.12)) continue;
                if (!closeEnough(right(signals.rect), right(target.rect), pairedWidth * 0.12)) continue;
                const double rightPaneLeft = std::min(target.rect.x, signals.rect.x);
                if (right(components.rect) > rightPaneLeft + pairedWidth * 0.03) continue;

                int left = components.rect.x - scaledPx(f, 36);
                int top = target.rect.y - scaledPx(f, 14);
                int panelRight = std::max(right(target.rect), right(signals.rect)) + scaledPx(f, 36);
                int panelBottom = bottom(signals.rect) + scaledPx(f, 170);
                Rect panel = clampRect({left, top, panelRight - left, panelBottom - top}, f.w, f.h);
                double panelAsp = panel.w / (double)std::max(1, panel.h);
                if (!(panel.w > scaledPx(f, 864) && panel.h > f.h * 0.45 &&
                      panelAsp > 1.0 && panelAsp < 2.0)) continue;

                int score = target.pixels + components.pixels + signals.pixels;
                score -= (int)std::lround(std::abs(signals.cx - target.cx) * 2.0);
                score -= (int)std::lround(
                    std::abs((target.cx - components.cx) - scaledPx(f, 595)) * 1.5);
                if (score > bestScore) {
                    bestScore = score;
                    bestBars.target = target.rect;
                    bestBars.components = components.rect;
                    bestBars.signals = signals.rect;
                    bestBars.hasTarget = bestBars.hasComponents = bestBars.hasSignals = true;
                    bestPanel = panel;
                }
            }
        }
    }

    if (bestScore > 0) {
        info.bars = bestBars;
        info.panel = bestPanel;
    } else if (diag) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "title layout failed bars=%zu", bars.size());
        *diag = buf;
        return info;
    };

    info.isMinigame = info.bars.hasTarget && info.bars.hasComponents && info.bars.hasSignals;
    if (diag) {
        char buf[256];
        std::snprintf(
            buf,
            sizeof(buf),
            "panel=%s title_bars=%zu timer=%d target=%d components=%d signals=%d",
            rectText(info.panel).c_str(),
            bars.size(),
            info.bars.hasTimer ? 1 : 0,
            info.bars.hasTarget ? 1 : 0,
            info.bars.hasComponents ? 1 : 0,
            info.bars.hasSignals ? 1 : 0
        );
        *diag = buf;
    }
    return info;
}

static bool validateMinigameGeometry(const Frame& f, const InGameGeometry& geometry) {
    if (!geometry.valid || geometry.windowGeneration != f.windowGeneration ||
        geometry.frameW != f.w || geometry.frameH != f.h) return false;

    auto hasWhiteUi = [&](Rect rect) {
        rect = clampRect(rect, f.w, f.h);
        if (rect.w <= 0 || rect.h <= 0) return false;
        const int step = std::max(1, std::min(rect.w, rect.h) / 12);
        int white = 0;
        int samples = 0;
        for (int y = rect.y; y < rect.y + rect.h; y += step) {
            for (int x = rect.x; x < rect.x + rect.w; x += step) {
                const size_t pixel = (static_cast<size_t>(y) * f.w + x) * 4;
                white += isFlashingStyleWhite(f.bgra[pixel + 2], f.bgra[pixel + 1], f.bgra[pixel]) ? 1 : 0;
                ++samples;
            }
        }
        return samples > 0 && white * 100 >= samples * 2;
    };

    return hasWhiteUi(geometry.roi.bars.target) &&
           hasWhiteUi(geometry.roi.bars.components) &&
           hasWhiteUi(geometry.roi.bars.signals);
}

static std::vector<int> edgeLinePeaks(const std::vector<int>& projection, int base, int minDist, double thresholdRatio) {
    if (projection.empty()) return {};

    std::vector<int> smooth(projection.size(), 0);
    for (int i = 0; i < (int)projection.size(); ++i) {
        int sum = 0, count = 0;
        for (int d = -2; d <= 2; ++d) {
            int j = i + d;
            if (0 <= j && j < (int)projection.size()) {
                sum += projection[j];
                ++count;
            }
        }
        smooth[i] = sum / std::max(1, count);
    }

    int maxVal = *std::max_element(smooth.begin(), smooth.end());
    int threshold = (int)std::lround(maxVal * thresholdRatio);
    std::vector<std::pair<int, int>> candidates;
    for (int i = 1; i + 1 < (int)smooth.size(); ++i) {
        if (smooth[i] < threshold) continue;
        if (smooth[i] >= smooth[i - 1] && smooth[i] >= smooth[i + 1]) {
            candidates.push_back({smooth[i], i});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](auto a, auto b){ return a.first > b.first; });

    std::vector<int> chosen;
    for (auto [score, idx] : candidates) {
        bool isNear = false;
        for (int c : chosen) {
            if (std::abs(c - idx) < minDist) {
                isNear = true;
                break;
            }
        }
        if (!isNear) chosen.push_back(idx);
    }
    std::sort(chosen.begin(), chosen.end());
    for (int& v : chosen) v += base;
    return chosen;
}

static std::vector<std::pair<int, int>> selectGridBorderPairs(
        const std::vector<int>& peaks,
        int pairCount,
        int minSide,
        int maxSide,
        double expectedSide,
        double expectedGap) {
    std::vector<std::pair<int, int>> best;
    std::vector<std::pair<int, int>> current;
    double bestCost = 1e100;
    const int minGap = std::max(1, (int)std::lround(expectedGap * 0.25));
    const int maxGap = std::max(minGap, (int)std::lround(expectedGap * 2.5));

    std::function<void(int, double)> search = [&](int firstIndex, double cost) {
        if ((int)current.size() == pairCount) {
            if (cost < bestCost) {
                bestCost = cost;
                best = current;
            }
            return;
        }

        const int pairsLeft = pairCount - (int)current.size();
        if ((int)peaks.size() - firstIndex < pairsLeft * 2) return;

        for (int leftIndex = firstIndex; leftIndex + 1 < (int)peaks.size(); ++leftIndex) {
            if (!current.empty()) {
                const int gap = peaks[leftIndex] - current.back().second;
                if (gap < minGap) continue;
                if (gap > maxGap) break;
            }
            for (int rightIndex = leftIndex + 1; rightIndex < (int)peaks.size(); ++rightIndex) {
                const int side = peaks[rightIndex] - peaks[leftIndex];
                if (side < minSide) continue;
                if (side > maxSide) break;

                double nextCost = cost + std::pow((side - expectedSide) / expectedSide, 2.0);
                if (!current.empty()) {
                    const int gap = peaks[leftIndex] - current.back().second;
                    nextCost += std::pow((gap - expectedGap) / expectedGap, 2.0);
                }
                if (nextCost >= bestCost) continue;

                current.push_back({peaks[leftIndex], peaks[rightIndex]});
                search(rightIndex + 1, nextCost);
                current.pop_back();
            }
        }
    };

    search(0, 0.0);
    return best;
}

static bool detectComponentBoxesByBorder(const Frame& f, const RoiInfo& roi, std::vector<Rect>& components, std::string* diag = nullptr) {
    Rect cb = roi.bars.components;
    Rect p = roi.panel;
    int left = cb.x + (int)std::lround(cb.w * 0.08);
    int right = cb.x + (int)std::lround(cb.w * 0.86);
    int top = cb.y + cb.h + (int)std::lround(cb.h * 0.25);
    int bottom = p.y + (int)std::lround(p.h * 0.84);
    Rect search = clampRect({left, top, right - left, bottom - top}, f.w, f.h);
    if (search.w <= 0 || search.h <= 0) {
        if (diag) *diag = "border search empty";
        return false;
    }

    std::vector<int> vertical(search.w, 0), horizontal(search.h, 0);
    for (int y = search.y; y < search.y + search.h; ++y) {
        int row = y * f.w;
        for (int x = search.x + 1; x < search.x + search.w; ++x) {
            vertical[x - search.x] += std::abs((int)f.gray[row + x] - (int)f.gray[row + x - 1]);
        }
    }
    for (int y = search.y + 1; y < search.y + search.h; ++y) {
        int row = y * f.w;
        int prev = (y - 1) * f.w;
        for (int x = search.x; x < search.x + search.w; ++x) {
            horizontal[y - search.y] += std::abs((int)f.gray[row + x] - (int)f.gray[prev + x]);
        }
    }

    int minDist = scaledPx(f, 10);
    auto xs = edgeLinePeaks(vertical, search.x, minDist, 0.45);
    auto ys = edgeLinePeaks(horizontal, search.y, minDist, 0.18);

    int minSide = std::max(scaledPx(f, 45), (int)std::lround(cb.w * 0.16));
    int maxSide = std::max(minSide + 1, (int)std::lround(cb.w * 0.36));
    auto colPairs = selectGridBorderPairs(xs, 2, minSide, maxSide, cb.w * 0.255, cb.w * 0.05);

    if (colPairs.size() != 2) {
        if (diag) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "border columns failed xs=%zu pairs=%zu search=%s", xs.size(), colPairs.size(), rectText(search).c_str());
            *diag = buf;
        }
        return false;
    }

    double avgW = 0.0;
    for (auto [a, b] : colPairs) avgW += b - a;
    avgW /= colPairs.size();

    auto rowPairs = selectGridBorderPairs(
        ys,
        4,
        (int)std::lround(avgW * 0.72),
        (int)std::lround(avgW * 1.35),
        avgW,
        avgW * 0.20);

    if (rowPairs.size() != 4) {
        if (diag) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "border rows failed ys=%zu pairs=%zu search=%s", ys.size(), rowPairs.size(), rectText(search).c_str());
            *diag = buf;
        }
        return false;
    }

    components.clear();
    for (auto [y0, y1] : rowPairs) {
        for (auto [x0, x1] : colPairs) {
            components.push_back(clampRect({x0, y0, x1 - x0 + 1, y1 - y0 + 1}, f.w, f.h));
        }
    }

    if (diag) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "border boxes=8 search=%s side=%.1f", rectText(search).c_str(), avgW);
        *diag = buf;
    }
    return components.size() == 8;
}

static bool detectRois(const Frame& f, const RoiInfo& roi, Rect& target, std::vector<Rect>& components, std::string* diag = nullptr) {
    Rect tb = roi.bars.target, sb = roi.bars.signals;

    int targetTop = tb.y + tb.h + (int)std::lround(tb.h * 0.35);
    int targetBottom = sb.y - (int)std::lround(tb.h * 0.55);
    int targetLeft = tb.x + (int)std::lround(tb.w * 0.12);
    int targetRight = tb.x + (int)std::lround(tb.w * 0.72);
    target = clampRect({targetLeft, targetTop, targetRight - targetLeft, targetBottom - targetTop}, f.w, f.h);

    std::string compDiag;
    if (!detectComponentBoxesByBorder(f, roi, components, &compDiag)) {
        if (diag) *diag = "component ROI failed: " + compDiag;
        return false;
    }
    if (target.w <= 0 || target.h <= 0 || components.size() != 8) {
        if (diag) *diag = "component ROI failed: " + compDiag;
        return false;
    }

    if (diag) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "target=%s components=8 source=border %s",
                      rectText(target).c_str(),
                      compDiag.c_str());
        *diag = buf;
    }
    return true;
}

static int detectLevelMarker(const Frame& f, Rect signalsBar, std::string* diag = nullptr,
                             Rect* markerLine = nullptr) {
    if (markerLine) *markerLine = {};
    signalsBar = clampRect(signalsBar, f.w, f.h);
    if (signalsBar.w <= 0 || signalsBar.h <= 0 || f.bgra.size() != (size_t)f.w * f.h * 4) {
        if (diag) *diag = "level marker unavailable";
        return -1;
    }

    const int yStart = std::min(f.h, signalsBar.y + signalsBar.h);
    const int yEnd = std::min(f.h, signalsBar.y + signalsBar.h * 2);
    const int minRun = std::max(scaledPx(f, 42), (int)std::lround(signalsBar.w * 0.12));
    const int maxRun = std::max(minRun + 1, (int)std::lround(signalsBar.w * 0.27));
    const int maxGap = scaledPx(f, 3);
    int bestLeft = -1, bestRight = -1, bestY = -1;
    double bestScore = -1e100;

    auto linePixel = [&](int x, int y) {
        const size_t p = (static_cast<size_t>(y) * f.w + x) * 4;
        const int blue = f.bgra[p];
        const int green = f.bgra[p + 1];
        const int red = f.bgra[p + 2];
        const int luma = (77 * red + 150 * green + 29 * blue) >> 8;
        const int high = std::max({red, green, blue});
        const int low = std::min({red, green, blue});
        return luma >= 135 && high - low <= 120;
    };

    for (int y = yStart; y < yEnd; ++y) {
        int runStart = -1, lastOn = -1;
        for (int x = signalsBar.x; x <= signalsBar.x + signalsBar.w; ++x) {
            const bool on = x < signalsBar.x + signalsBar.w && linePixel(x, y);
            if (on) {
                if (runStart < 0) runStart = x;
                lastOn = x;
            }
            const bool gapEnded = runStart >= 0 && (!on && x - lastOn > maxGap);
            const bool rowEnded = x == signalsBar.x + signalsBar.w;
            if (!gapEnded && !rowEnded) continue;

            const int runWidth = lastOn - runStart + 1;
            if (runWidth >= minRun && runWidth <= maxRun) {
                const double yProgress = (y - yStart) / (double)std::max(1, yEnd - yStart - 1);
                const double widthScore = runWidth / (double)std::max(1, maxRun);
                const double score = yProgress * 3.0 + widthScore;
                if (score <= bestScore) {
                    runStart = -1;
                    lastOn = -1;
                    continue;
                }
                bestLeft = runStart;
                bestRight = lastOn;
                bestY = y;
                bestScore = score;
            }
            runStart = -1;
            lastOn = -1;
        }
    }

    if (bestLeft < 0) {
        if (diag) *diag = "level marker line not found";
        return -1;
    }

    const double center = (bestLeft + bestRight) * 0.5;
    const double firstCenter = signalsBar.x + signalsBar.w * 0.20;
    const double spacing = signalsBar.w * 0.195;
    const int marker = (int)std::lround((center - firstCenter) / spacing);
    if (marker < 0 || marker >= 4) {
        if (diag) *diag = "level marker outside slots";
        return -1;
    }

    if (diag) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "level marker=%d line=(%d..%d y=%d)",
                      marker + 1, bestLeft, bestRight, bestY);
        *diag = buf;
    }
    if (markerLine) {
        *markerLine = {bestLeft, bestY, bestRight - bestLeft + 1, 1};
    }
    return marker;
}

struct GrayPatch {
    int w = 0, h = 0;
    std::vector<uint8_t> pixels;
};

static GrayPatch downsampleGray(const Frame& f, Rect r, int divisor) {
    r = clampRect(r, f.w, f.h);
    GrayPatch out;
    out.w = std::max(1, r.w / divisor);
    out.h = std::max(1, r.h / divisor);
    out.pixels.resize(static_cast<size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        const int sy0 = r.y + y * r.h / out.h;
        const int sy1 = r.y + (y + 1) * r.h / out.h;
        for (int x = 0; x < out.w; ++x) {
            const int sx0 = r.x + x * r.w / out.w;
            const int sx1 = r.x + (x + 1) * r.w / out.w;
            int sum = 0, count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    sum += f.gray[sy * f.w + sx];
                    ++count;
                }
            }
            out.pixels[y * out.w + x] = static_cast<uint8_t>(sum / std::max(1, count));
        }
    }
    return out;
}

static GrayPatch resizeGrayBilinear(const GrayPatch& src, int w, int h) {
    GrayPatch out;
    out.w = std::max(1, w);
    out.h = std::max(1, h);
    out.pixels.resize(static_cast<size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        const double sourceY = std::clamp((y + 0.5) * src.h / out.h - 0.5,
                                          0.0, (double)(src.h - 1));
        const int y0 = (int)std::floor(sourceY);
        const int y1 = std::min(src.h - 1, y0 + 1);
        const double fy = sourceY - y0;
        for (int x = 0; x < out.w; ++x) {
            const double sourceX = std::clamp((x + 0.5) * src.w / out.w - 0.5,
                                              0.0, (double)(src.w - 1));
            const int x0 = (int)std::floor(sourceX);
            const int x1 = std::min(src.w - 1, x0 + 1);
            const double fx = sourceX - x0;
            const double top = src.pixels[y0 * src.w + x0] * (1.0 - fx)
                + src.pixels[y0 * src.w + x1] * fx;
            const double bottom = src.pixels[y1 * src.w + x0] * (1.0 - fx)
                + src.pixels[y1 * src.w + x1] * fx;
            out.pixels[y * out.w + x] = (uint8_t)std::clamp(
                (int)std::lround(top * (1.0 - fy) + bottom * fy), 0, 255);
        }
    }
    return out;
}

static double matchGrayZncc(const GrayPatch& target, const GrayPatch& component) {
    constexpr double kInvalidScore = -2.0;
    if (component.w >= target.w || component.h >= target.h) return kInvalidScore;

    constexpr int sampleStep = 1;
    int64_t componentSum = 0, componentSquared = 0;
    int samples = 0;
    for (int y = 0; y < component.h; y += sampleStep) {
        for (int x = 0; x < component.w; x += sampleStep) {
            const int value = component.pixels[y * component.w + x];
            componentSum += value;
            componentSquared += value * value;
            ++samples;
        }
    }
    const double componentVariance = componentSquared
        - (double)componentSum * componentSum / std::max(1, samples);
    if (componentVariance <= 1e-6) return kInvalidScore;

    double best = kInvalidScore;
    for (int y0 = 0; y0 <= target.h - component.h; ++y0) {
        for (int x0 = 0; x0 <= target.w - component.w; ++x0) {
            int64_t targetSum = 0, targetSquared = 0, cross = 0;
            for (int y = 0; y < component.h; y += sampleStep) {
                const uint8_t* targetRow = target.pixels.data() + (y0 + y) * target.w + x0;
                const uint8_t* componentRow = component.pixels.data() + y * component.w;
                for (int x = 0; x < component.w; x += sampleStep) {
                    const int tv = targetRow[x];
                    const int cv = componentRow[x];
                    targetSum += tv;
                    targetSquared += tv * tv;
                    cross += tv * cv;
                }
            }
            const double targetVariance = targetSquared
                - (double)targetSum * targetSum / samples;
            if (targetVariance <= 1e-6) continue;
            const double covariance = cross
                - (double)targetSum * componentSum / samples;
            best = std::max(best, covariance / std::sqrt(targetVariance * componentVariance));
        }
    }
    return best;
}

static std::vector<double> answerScores(const Frame& f, Rect target, const std::vector<Rect>& components) {
    int targetInset = std::max(1, (int)std::lround(std::min(target.w, target.h) * 0.02));
    target = clampRect(
        {target.x + targetInset, target.y + targetInset,
         target.w - targetInset * 2, target.h - targetInset * 2},
        f.w, f.h);
    constexpr int down = 4;
    const GrayPatch targetPatch = downsampleGray(f, target, down);
    std::vector<GrayPatch> componentPatches;
    componentPatches.reserve(components.size());
    for (Rect cr : components) {
        int inset = std::max(1, (int)std::lround(std::min(cr.w, cr.h) * 0.05));
        cr = clampRect({cr.x + inset, cr.y + inset, cr.w - inset * 2, cr.h - inset * 2}, f.w, f.h);
        componentPatches.push_back(downsampleGray(f, cr, down));
    }

    // All eight candidates are rendered from the same source sheet, so the
    // four matching fragments must share one scale. Allowing every candidate
    // to choose its own scale lets a decoy imitate unrelated target ridges.
    const double scales[] = {1.0, 1.15, 1.3, 1.45};
    std::vector<double> scores(componentPatches.size(), -2.0);
    double bestScaleScore = -1e100;
    for (double scale : scales) {
        std::vector<double> scaleScores;
        scaleScores.reserve(componentPatches.size());
        for (const auto& component : componentPatches) {
            const int sw = std::max(2, (int)std::lround(component.w * scale));
            const int sh = std::max(2, (int)std::lround(component.h * scale));
            const GrayPatch scaled = resizeGrayBilinear(component, sw, sh);
            scaleScores.push_back(matchGrayZncc(targetPatch, scaled));
        }

        auto ranked = scaleScores;
        std::sort(ranked.begin(), ranked.end(), std::greater<double>());
        double scaleScore = std::accumulate(
            ranked.begin(), ranked.begin() + std::min<size_t>(4, ranked.size()), 0.0);
        if (scaleScore > bestScaleScore) {
            bestScaleScore = scaleScore;
            scores = std::move(scaleScores);
        }
    }
    return scores;
}

static uint64_t targetFingerprintHash(const Frame& f, Rect target) {
    int inset = std::max(1, (int)std::lround(std::min(target.w, target.h) * 0.02));
    target = clampRect(
        {target.x + inset, target.y + inset,
         target.w - inset * 2, target.h - inset * 2},
        f.w, f.h);
    // 8x8 difference hash. Pairwise ordering is stable under monotonic gamma
    // and exposure changes, unlike the old fixed gray bands.
    uint64_t hash = 0;
    for (int row = 0; row < 8; ++row) {
        int values[9]{};
        for (int col = 0; col < 9; ++col) {
            int energy = 0;
            for (int sy = 0; sy < 4; ++sy) {
                const int y = target.y + std::min(
                    target.h - 1,
                    (row * 4 + sy) * target.h / 32);
                for (int sx = 0; sx < 4; ++sx) {
                    const int x = target.x + std::min(
                        target.w - 1,
                        (col * 4 + sx) * target.w / 36);
                    energy += gradientAt(f, x, y);
                }
            }
            values[col] = energy;
        }
        for (int col = 0; col < 8; ++col) {
            hash <<= 1;
            hash |= values[col] > values[col + 1] ? 1ull : 0ull;
        }
    }
    return hash;
}

static int hashDistance(uint64_t a, uint64_t b) {
    uint64_t bits = a ^ b;
    int count = 0;
    while (bits) {
        bits &= bits - 1;
        ++count;
    }
    return count;
}

static bool sameTargetFingerprint(uint64_t a, uint64_t b) {
    return hashDistance(a, b) <= 6;
}

static int edgeWeightedGray(const Frame& f, Rect r) {
    r = clampRect(r, f.w, f.h);
    int64_t weightedGray = 0;
    int64_t totalWeight = 0;
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            const int weight = gradientAt(f, x, y);
            weightedGray += static_cast<int64_t>(f.gray[y * f.w + x]) * weight;
            totalWeight += weight;
        }
    }
    return totalWeight > 0 ? static_cast<int>(weightedGray / totalWeight) : 0;
}

static void markStates(const Frame& f, std::vector<BlockInfo>& blocks) {
    struct RankedState { int score = 0; int block = 0; };
    std::vector<RankedState> selectedScores;
    selectedScores.reserve(blocks.size());

    int bestCursor = -1, bestScore = -1, secondScore = -1;
    std::vector<int> cursorScores;
    for (auto& b : blocks) {
        const int inset = std::max(1, (int)std::lround(std::min(b.rect.w, b.rect.h) * 0.08));
        Rect selectedRect = clampRect(
            {b.rect.x + inset, b.rect.y + inset, b.rect.w - inset * 2, b.rect.h - inset * 2},
            f.w, f.h);
        selectedScores.push_back({edgeWeightedGray(f, selectedRect), b.index});
        b.selected = false;

        Rect cursorRect = padRect(b.rect, f.w, f.h, 0.12);
        int len = std::max(scaledPx(f, 10), (int)std::lround(std::min(cursorRect.w, cursorRect.h) * 0.22));
        int band = std::max(scaledPx(f, 3), (int)std::lround(std::min(cursorRect.w, cursorRect.h) * 0.045));
        auto at = [&](int x, int y){ return gradientAt(f, cursorRect.x + x, cursorRect.y + y); };
        int score = 0;
        for (int y = 0; y < band; ++y) for (int x = 0; x < len; ++x) score += at(x, y) + at(cursorRect.w - 1 - x, y);
        for (int y = 0; y < len; ++y) for (int x = 0; x < band; ++x) score += at(x, y) + at(cursorRect.w - 1 - x, y);
        for (int y = 0; y < band; ++y) for (int x = 0; x < len; ++x) score += at(x, cursorRect.h - 1 - y) + at(cursorRect.w - 1 - x, cursorRect.h - 1 - y);
        for (int y = 0; y < len; ++y) for (int x = 0; x < band; ++x) score += at(x, cursorRect.h - 1 - y) + at(cursorRect.w - 1 - x, cursorRect.h - 1 - y);
        cursorScores.push_back(score);
        b.cursorScore = score;
        if (score > bestScore) {
            secondScore = bestScore;
            bestScore = score;
            bestCursor = b.index;
        } else if (score > secondScore) {
            secondScore = score;
        }
    }

    // Selected ridges are brighter than unselected ridges, but their absolute
    // value depends on HDR tone mapping. Split the eight relative scores only
    // when two compact groups are clearly separated; otherwise the round is
    // treated as its normal all-unselected initial state.
    std::sort(selectedScores.begin(), selectedScores.end(), [](const auto& a, const auto& b) {
        return a.score < b.score;
    });
    double bestSeparation = 0.0;
    int bestSplit = -1;
    for (int split = 1; split < (int)selectedScores.size(); ++split) {
        double lowMean = 0.0, highMean = 0.0;
        for (int i = 0; i < split; ++i) lowMean += selectedScores[i].score;
        for (int i = split; i < (int)selectedScores.size(); ++i) highMean += selectedScores[i].score;
        lowMean /= split;
        highMean /= selectedScores.size() - split;
        double lowVar = 0.0, highVar = 0.0;
        for (int i = 0; i < split; ++i) lowVar += std::pow(selectedScores[i].score - lowMean, 2.0);
        for (int i = split; i < (int)selectedScores.size(); ++i) highVar += std::pow(selectedScores[i].score - highMean, 2.0);
        lowVar /= split;
        highVar /= selectedScores.size() - split;
        const double separation = std::pow(highMean - lowMean, 2.0) / (lowVar + highVar + 1.0);
        if (separation > bestSeparation) {
            bestSeparation = separation;
            bestSplit = split;
        }
    }
    if (bestSplit > 0 && bestSeparation >= 9.0) {
        for (int i = bestSplit; i < (int)selectedScores.size(); ++i) {
            for (auto& b : blocks) {
                if (b.index == selectedScores[i].block) b.selected = true;
            }
        }
    }

    std::sort(cursorScores.begin(), cursorScores.end());
    const int medianCursor = cursorScores.empty() ? 0 : cursorScores[cursorScores.size() / 2];
    const bool cursorSeparated = bestCursor > 0 && bestScore > 0
        && static_cast<int64_t>(bestScore) * 100 >= static_cast<int64_t>(std::max(0, secondScore)) * 108
        && static_cast<int64_t>(bestScore) * 100 >= static_cast<int64_t>(medianCursor) * 115;
    if (cursorSeparated) {
        for (auto& b : blocks) b.cursor = b.index == bestCursor;
    }
}

static void resetAutomation(AutomationState& aut) {
    gta5::input::CancelAll();
    aut = {};
}

static bool allBlocksCorrectlySelected(const OverlayState& state) {
    if (!state.visible || state.blocks.size() != 8) return false;
    for (const auto& b : state.blocks) {
        if (b.selected != b.correct) return false;
    }
    return true;
}

static int cursorIndex(const OverlayState& state) {
    for (const auto& b : state.blocks) {
        if (b.cursor) return b.index;
    }
    return -1;
}

static void appendCursorMoves(int& cur, int target, std::vector<gta5::input::Key>& keys) {
    int curRow = (cur - 1) / 2;
    int curCol = (cur - 1) % 2;
    int targetRow = (target - 1) / 2;
    int targetCol = (target - 1) % 2;

    while (curCol < targetCol) {
        keys.push_back({0x4D, true});
        ++curCol;
    }
    while (curCol > targetCol) {
        keys.push_back({0x4B, true});
        --curCol;
    }
    while (curRow < targetRow) {
        keys.push_back({0x50, true});
        ++curRow;
    }
    while (curRow > targetRow) {
        keys.push_back({0x48, true});
        --curRow;
    }
    cur = target;
}

static void planAndRunAutomation(const OverlayState& state, AutomationState& aut,
                                 std::string* diag = nullptr, int levelMarker = -1) {
    if (!state.visible) {
        resetAutomation(aut);
        if (diag) *diag = "auto reset";
        return;
    }

    if (aut.plannedHash != 0 && !sameTargetFingerprint(aut.plannedHash, state.targetHash)) {
        resetAutomation(aut);
    }

    if (aut.phase == AutomationPhase::Selecting && aut.inputJob) {
        if (aut.inputJob.Pending()) {
            if (diag) *diag = "selection input queued";
            return;
        }
        if (!aut.inputJob.Succeeded()) {
            resetAutomation(aut);
            if (diag) *diag = "selection input canceled or failed";
            return;
        }
        aut.inputJob = {};
        aut.phase = AutomationPhase::VerifyDelay;
        aut.verifyAfter = Clock::now() + std::chrono::milliseconds(120);
        if (diag) *diag = "selection input executed; waiting to verify";
        return;
    }

    if (aut.phase == AutomationPhase::VerifyDelay && Clock::now() < aut.verifyAfter) {
        if (diag) *diag = "waiting to verify selection";
        return;
    }
    if (aut.phase == AutomationPhase::Submitting ||
        aut.phase == AutomationPhase::WaitingLevel) {
        return;
    }

    if (allBlocksCorrectlySelected(state)) {
        std::vector<gta5::input::Command> commands{
            {{0x0F, false}, gta5::input::Action::LongPress},
        };
        aut.inputJob = gta5::input::QueueSequence(commands);
        aut.phase = AutomationPhase::Submitting;
        aut.plannedHash = state.targetHash;
        aut.plannedLevelMarker = levelMarker;
        if (diag) *diag = "selection verified; tab queued";
        return;
    }

    int cur = cursorIndex(state);
    if (cur < 1 || state.blocks.size() != 8) {
        if (diag) *diag = "auto waiting cursor";
        return;
    }

    std::vector<bool> selected(9, false);
    std::vector<bool> correct(9, false);
    for (const auto& b : state.blocks) {
        if (b.index >= 1 && b.index <= 8) {
            selected[b.index] = b.selected;
            correct[b.index] = b.correct;
        }
    }

    int toggles = 0;
    std::vector<gta5::input::Key> keys;
    for (int target = 1; target <= 8; ++target) {
        if (selected[target] == correct[target]) continue;
        appendCursorMoves(cur, target, keys);
        keys.push_back({0x1C, false});
        selected[target] = !selected[target];
        ++toggles;
    }

    std::vector<gta5::input::Command> commands;
    commands.reserve(keys.size());
    for (const auto& key : keys) commands.push_back({key, gta5::input::Action::Tap});
    aut.inputJob = gta5::input::QueueSequence(commands);
    aut.phase = AutomationPhase::Selecting;
    aut.plannedHash = state.targetHash;
    aut.plannedLevelMarker = levelMarker;

    if (diag) {
        *diag = "selection plan queued toggles=" + std::to_string(toggles);
    }
}

static OverlayState analyzeFrame(const Frame& f, SolverCache& cache, std::string* diag = nullptr,
                                 FrameTiming* timing = nullptr, InGameGeometry* geometry = nullptr) {
    auto analyzeStart = Clock::now();
    OverlayState os;
    std::string minigameDiag;
    auto phaseStart = Clock::now();
    const bool geometryHit = geometry && validateMinigameGeometry(f, *geometry);
    RoiInfo roi = geometryHit ? geometry->roi : detectMinigame(f, &minigameDiag);
    if (timing) {
        timing->gateMs = msSince(phaseStart);
        timing->minigame = roi.isMinigame;
    }
    if (!roi.isMinigame) {
        if (geometry) *geometry = {};
        if (diag) *diag = "not in minigame: " + minigameDiag;
        if (timing) timing->analyzeMs = msSince(analyzeStart);
        return os;
    }

    Rect target = geometryHit ? geometry->target : Rect{};
    std::vector<Rect> comps = geometryHit ? geometry->components : std::vector<Rect>{};
    std::string roiDiag;
    phaseStart = Clock::now();
    if (!geometryHit && !detectRois(f, roi, target, comps, &roiDiag)) {
        if (diag) *diag = "minigame found, ROI failed: " + roiDiag;
        if (timing) {
            timing->roiMs = msSince(phaseStart);
            timing->analyzeMs = msSince(analyzeStart);
        }
        return os;
    }
    if (geometry && !geometryHit) {
        geometry->valid = true;
        geometry->windowGeneration = f.windowGeneration;
        geometry->frameW = f.w;
        geometry->frameH = f.h;
        geometry->roi = roi;
        geometry->target = target;
        geometry->components = comps;
    }
    if (timing) timing->roiMs = msSince(phaseStart);

    phaseStart = Clock::now();
    uint64_t targetHash = targetFingerprintHash(f, target);
    if (timing) timing->hashMs = msSince(phaseStart);
    bool cacheHit = cache.valid
        && sameTargetFingerprint(cache.targetHash, targetHash)
        && cache.baseBlocks.size() == comps.size()
        && cache.target.x == target.x
        && cache.target.y == target.y
        && cache.target.w == target.w
        && cache.target.h == target.h;
    if (timing) timing->cacheHit = cacheHit;

    if (!cacheHit) {
        phaseStart = Clock::now();
        auto scores = answerScores(f, target, comps);
        if (timing) timing->answerMs = msSince(phaseStart);
        const int validScores = (int)std::count_if(scores.begin(), scores.end(), [](double score) {
            return score > -1.5;
        });
        if (validScores < 4) {
            if (diag) *diag = "fingerprint features insufficient valid=" + std::to_string(validScores);
            if (timing) timing->analyzeMs = msSince(analyzeStart);
            return os;
        }
        std::vector<int> order(scores.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b){ return scores[a] > scores[b]; });

        cache = {};
        cache.valid = true;
        cache.targetHash = targetHash;
        cache.target = target;
        for (int i = 0; i < (int)comps.size(); ++i) {
            BlockInfo b;
            b.index = i + 1;
            b.rect = comps[i];
            b.score = scores[i];
            b.correct = std::find(order.begin(), order.begin() + std::min(4, (int)order.size()), i) != order.begin() + std::min(4, (int)order.size());
            cache.baseBlocks.push_back(b);
        }
    }

    os.visible = true;
    os.targetHash = targetHash;
    os.target = target;
    os.blocks = cache.baseBlocks;
    os.levelMarker = detectLevelMarker(f, roi.bars.signals, nullptr, &os.levelMarkerLine);
    phaseStart = Clock::now();
    markStates(f, os.blocks);
    if (timing) timing->stateMs = msSince(phaseStart);
    if (diag) {
        std::string correct;
        for (const auto& b : os.blocks) {
            if (b.correct) {
                if (!correct.empty()) correct += ",";
                correct += std::to_string(b.index);
            }
        }
        *diag = std::string("overlay updated: ") + (cacheHit ? "cache-hit " : "cache-miss ")
            + roiDiag + " correct=" + correct;
    }
    if (timing) timing->analyzeMs = msSince(analyzeStart);
    return os;
}

static void publishState(const OverlayState& s) {
    EnterCriticalSection(&gStateLock);
    gState = s;
    LeaveCriticalSection(&gStateLock);
    if (gOverlayWnd) {
        if (GetCurrentThreadId() == gUiThreadId) {
            RedrawWindow(gOverlayWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        } else {
            InvalidateRect(gOverlayWnd, nullptr, FALSE);
        }
    }
}

#ifdef CLI_TEST
static bool loadPngFrame(const wchar_t* path, Frame& out) {
    Gdiplus::Bitmap bitmap(path);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;

    int w = (int)bitmap.GetWidth();
    int h = (int)bitmap.GetHeight();
    Gdiplus::Rect lockRect(0, 0, w, h);
    Gdiplus::BitmapData data{};
    Gdiplus::Status st = bitmap.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data);
    if (st != Gdiplus::Ok) return false;

    out.x = 0;
    out.y = 0;
    out.w = w;
    out.h = h;
    out.bgra.assign(w * h * 4, 0);
    out.gray.assign(w * h, 0);

    for (int y = 0; y < h; ++y) {
        uint8_t* row = (uint8_t*)data.Scan0 + y * data.Stride;
        for (int x = 0; x < w; ++x) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            int p = y * w + x;
            out.bgra[p * 4 + 0] = b;
            out.bgra[p * 4 + 1] = g;
            out.bgra[p * 4 + 2] = r;
            out.bgra[p * 4 + 3] = 255;
            out.gray[p] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
        }
    }

    bitmap.UnlockBits(&data);
    return true;
}

static int runCli(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: gta_fingerprint_cli.exe image.png\n");
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        fprintf(stderr, "GDI+ startup failed\n");
        return 1;
    }

    Frame f;
    if (!loadPngFrame(argv[1], f)) {
        fwprintf(stderr, L"failed to load image: %ls\n", argv[1]);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 1;
    }

    printf("image: %dx%d\n", f.w, f.h);

    bool runTwice = argc >= 3 && std::wstring(argv[2]) == L"--twice";
    if (runTwice) {
        SolverCache cache;
        std::string d1, d2;
        analyzeFrame(f, cache, &d1);
        analyzeFrame(f, cache, &d2);
        printf("cache_test_1: %s\n", d1.c_str());
        printf("cache_test_2: %s\n", d2.c_str());
    }

    std::string gateDiag;
    RoiInfo roi = detectMinigame(f, &gateDiag);
    printf("gate: %s\n", roi.isMinigame ? "true" : "false");
    printf("gate_diag: %s\n", gateDiag.c_str());
    if (!roi.isMinigame) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 2;
    }

    Rect target;
    std::vector<Rect> comps;
    std::string roiDiag;
    bool roiOk = detectRois(f, roi, target, comps, &roiDiag);
    printf("roi: %s\n", roiOk ? "true" : "false");
    printf("roi_diag: %s\n", roiDiag.c_str());
    if (!roiOk) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 3;
    }

    std::string markerDiag;
    const int levelMarker = detectLevelMarker(f, roi.bars.signals, &markerDiag);
    printf("level_marker: %d\n", levelMarker >= 0 ? levelMarker + 1 : -1);
    printf("level_marker_diag: %s\n", markerDiag.c_str());

    auto scores = answerScores(f, target, comps);
    std::vector<int> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b){ return scores[a] > scores[b]; });

    std::vector<BlockInfo> blocks;
    for (int i = 0; i < (int)comps.size(); ++i) {
        BlockInfo b;
        b.index = i + 1;
        b.rect = comps[i];
        b.score = scores[i];
        b.correct = std::find(order.begin(), order.begin() + std::min(4, (int)order.size()), i) != order.begin() + std::min(4, (int)order.size());
        blocks.push_back(b);
    }
    markStates(f, blocks);

    printf("target: %s\n", rectText(target).c_str());
    printf("blocks:\n");
    for (const auto& b : blocks) {
        printf(
            "  #%d rect=%s score=%.3f correct=%d cursor=%d selected=%d cursor_score=%d\n",
            b.index,
            rectText(b.rect).c_str(),
            b.score,
            b.correct ? 1 : 0,
            b.cursor ? 1 : 0,
            b.selected ? 1 : 0,
            b.cursorScore
        );
    }

    printf("correct:");
    for (const auto& b : blocks) if (b.correct) printf(" %d", b.index);
    printf("\n");
    printf("cursor:");
    bool anyCursor = false;
    for (const auto& b : blocks) if (b.cursor) { printf(" %d", b.index); anyCursor = true; }
    if (!anyCursor) printf(" none");
    printf("\nselected:");
    bool anySelected = false;
    for (const auto& b : blocks) if (b.selected) { printf(" %d", b.index); anySelected = true; }
    if (!anySelected) printf(" none");
    printf("\n");

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
#endif

static void workerLoop() {
    postLog("worker started");
    int frameNo = 0;
    bool lastMinigame = false;
    bool captureFailed = false;
    SolverCache cache;
    AutomationState automation;
    while (gRunning.load()) {
        auto loopStart = Clock::now();
        FrameTiming timing;
        Frame f;
        auto phaseStart = Clock::now();
        if (captureScreen(f)) {
            timing.captureMs = msSince(phaseStart);
            if (frameNo == 0) {
                postLog("capture " + std::to_string(f.w) + "x" + std::to_string(f.h) + " origin=(0,0)");
            }
            if (captureFailed) {
                postLog("capture recovered");
                captureFailed = false;
            }
            OverlayState state = analyzeFrame(f, cache, nullptr, &timing);
            std::string autoDiag;
            phaseStart = Clock::now();
            publishState(scaleOverlayStateToScreen(f, state));
            timing.publishMs = msSince(phaseStart);
            phaseStart = Clock::now();
            planAndRunAutomation(state, automation, &autoDiag);
            timing.autoMs = msSince(phaseStart);
            timing.totalMs = msSince(loopStart);

            if (timing.minigame && !lastMinigame) {
                postLog("minigame detected");
            } else if (!timing.minigame && lastMinigame) {
                postLog("minigame lost");
            }

            if (state.visible && !timing.cacheHit) {
                postLog("answer ready: correct=" + correctList(state)
                    + " target=" + rectText(state.target)
                    + " | " + timingText(timing));
            }

            if (autoDiag.find("executed") != std::string::npos) {
                postLog(autoDiag);
            }
            lastMinigame = timing.minigame;
        } else {
            if (!captureFailed) {
                postLog("screen capture failed");
                captureFailed = true;
            }
        }
        frameNo++;
        Sleep(kFrameDelayMs);
    }
    resetAutomation(automation);
    publishState({});
    postLog("worker stopped");
}

static void drawOverlay(HDC hdc) {
    OverlayState s;
    EnterCriticalSection(&gStateLock);
    s = gState;
    LeaveCriticalSection(&gStateLock);

    RECT rc{0, 0, gVirtualW, gVirtualH};
    HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, clearBrush);
    DeleteObject(clearBrush);

    if (!s.visible) return;
    const int savedDc = SaveDC(hdc);
    int refBlock = s.blocks.empty() ? std::max(1, s.target.w / 3) : std::max(1, s.blocks.front().rect.w);
    int blockPen = std::max(1, (int)std::lround(refBlock / 60.0));
    int targetPen = blockPen;
    int dot = std::max(4, (int)std::lround(refBlock * 0.10));
    int dotMargin = std::max(2, (int)std::lround(refBlock * 0.05));
    HPEN green = CreatePen(PS_SOLID, blockPen, RGB(0, 255, 0));
    HPEN red = CreatePen(PS_SOLID, blockPen, RGB(255, 50, 50));
    HPEN magenta = CreatePen(PS_SOLID, targetPen, RGB(255, 0, 255));
    HPEN markerPen = CreatePen(PS_SOLID, blockPen, RGB(0, 255, 0));
    HBRUSH hollow = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    HBRUSH yellow = CreateSolidBrush(RGB(255, 230, 0));
    HBRUSH pink = CreateSolidBrush(RGB(255, 70, 190));
    HBRUSH markerFill = CreateSolidBrush(RGB(0, 255, 0));
    SelectObject(hdc, hollow);
    SetBkMode(hdc, TRANSPARENT);

    SelectObject(hdc, magenta);
    Rectangle(
        hdc,
        s.target.x - gVirtualX,
        s.target.y - gVirtualY,
        s.target.x + s.target.w - gVirtualX,
        s.target.y + s.target.h - gVirtualY);

    for (const auto& b : s.blocks) {
        int x = b.rect.x - gVirtualX, y = b.rect.y - gVirtualY;
        SelectObject(hdc, b.correct ? green : red);
        Rectangle(hdc, x, y, x + b.rect.w, y + b.rect.h);
        if (b.cursor) {
            SelectObject(hdc, pink);
            int markerTop = y + (b.rect.h - dot) / 2;
            int markerGap = std::max(dot, dotMargin * 2);
            if (b.selected) markerGap += dot + dotMargin;
            bool leftColumn = b.index % 2 == 1;
            int markerLeft = leftColumn
                ? x - markerGap - dot
                : x + b.rect.w + markerGap;
            Ellipse(hdc, markerLeft, markerTop, markerLeft + dot, markerTop + dot);
            SelectObject(hdc, hollow);
        }
        if (b.selected) {
            SelectObject(hdc, yellow);
            int markerTop = y + (b.rect.h - dot) / 2;
            int markerGap = std::max(dot, dotMargin * 2);
            bool leftColumn = b.index % 2 == 1;
            int markerLeft = leftColumn
                ? x - markerGap - dot
                : x + b.rect.w + markerGap;
            Ellipse(hdc, markerLeft, markerTop, markerLeft + dot, markerTop + dot);
            SelectObject(hdc, hollow);
        }
    }

    if (s.levelMarker >= 0 && s.levelMarkerLine.w > 0) {
        const int centerX = s.levelMarkerLine.x + s.levelMarkerLine.w / 2 - gVirtualX;
        const int markerWidth = std::max(10, (int)std::lround(s.levelMarkerLine.w * 0.16));
        const int markerHeight = std::max(7, (int)std::lround(markerWidth * 0.65));
        const int markerTop = s.levelMarkerLine.y + s.levelMarkerLine.h - gVirtualY
            + std::max(4, (int)std::lround(markerHeight * 0.75));
        POINT triangle[3]{
            {centerX - markerWidth / 2, markerTop},
            {centerX + markerWidth / 2, markerTop},
            {centerX, markerTop + markerHeight},
        };
        SelectObject(hdc, markerPen);
        SelectObject(hdc, markerFill);
        Polygon(hdc, triangle, 3);
        SelectObject(hdc, hollow);
    }

    if (savedDc) RestoreDC(hdc, savedDc);
    DeleteObject(green); DeleteObject(red); DeleteObject(magenta); DeleteObject(markerPen);
    DeleteObject(yellow); DeleteObject(pink); DeleteObject(markerFill);
}

static LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            drawOverlay(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void updateButton(HWND hwnd) {
    SetDlgItemTextW(hwnd, 1001, gStopping.load() ? L"Stopping" : (gRunning.load() ? L"Stop" : L"Start"));
    HWND btn = GetDlgItem(hwnd, 1001);
    if (btn) EnableWindow(btn, !gStopping.load());
}

static void startWorker(HWND hwnd) {
    if (gStopping.load()) return;
    if (gRunning.exchange(true)) return;
    postLog("start requested");
    if (gOverlayWnd) {
        SetWindowPos(gOverlayWnd, HWND_TOPMOST, gVirtualX, gVirtualY, gVirtualW, gVirtualH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        publishState({});
    }
    gWorker = std::thread(workerLoop);
    updateButton(hwnd);
}

static void stopWorker(HWND hwnd) {
    if (!gRunning.exchange(false)) return;
    gStopping.store(true);
    postLog("stop requested");
    publishState({});
    updateButton(hwnd);
    if (gWorker.joinable()) {
        std::thread oldWorker = std::move(gWorker);
        std::thread joiner([worker = std::move(oldWorker)]() mutable {
            if (worker.joinable()) worker.join();
            gStopping.store(false);
            if (gMainWnd) PostMessageW(gMainWnd, WM_APP_WORKER_STOPPED, 0, 0);
        });
        joiner.detach();
    } else {
        gStopping.store(false);
        updateButton(hwnd);
    }
}

static LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            CreateWindowW(L"BUTTON", L"Start", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 16, 16, 120, 36, hwnd, (HMENU)1001, GetModuleHandle(nullptr), nullptr);
            gStatusText = CreateWindowW(L"STATIC", L"Idle", WS_VISIBLE | WS_CHILD, 150, 24, 430, 24, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
            gLogList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                       16, 64, 560, 260, hwnd, (HMENU)1002, GetModuleHandle(nullptr), nullptr);
            postLog("app ready");
            return 0;
        case WM_APP_LOG: {
            std::unique_ptr<std::wstring> text((std::wstring*)lp);
            if (gLogList) {
                int index = (int)SendMessageW(gLogList, LB_ADDSTRING, 0, (LPARAM)text->c_str());
                SendMessageW(gLogList, LB_SETTOPINDEX, std::max(0, index - 14), 0);
                int count = (int)SendMessageW(gLogList, LB_GETCOUNT, 0, 0);
                while (count > 200) {
                    SendMessageW(gLogList, LB_DELETESTRING, 0, 0);
                    count--;
                }
            }
            if (gStatusText) SetWindowTextW(gStatusText, text->c_str());
            std::ofstream log("debug.log", std::ios::app);
            std::string narrow;
            narrow.reserve(text->size());
            for (wchar_t ch : *text) narrow.push_back(static_cast<char>(ch));
            log << narrow << "\n";
            return 0;
        }
        case WM_APP_WORKER_STOPPED:
            publishState({});
            updateButton(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == 1001) {
                if (gRunning.load()) stopWorker(hwnd); else startWorker(hwnd);
            }
            return 0;
        case WM_DESTROY:
            gRunning.store(false);
            if (gWorker.joinable()) gWorker.join();
            while (gStopping.load()) Sleep(10);
            publishState({});
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}


bool DetectInGame(const gta5::capture::GameFrame& captured) {
  Frame f;
  gVirtualX = captured.screenX;
  gVirtualY = captured.screenY;
  gVirtualW = captured.screenW;
  gVirtualH = captured.screenH;
  f.x = captured.screenX; f.y = captured.screenY;
  f.w = captured.width; f.h = captured.height;
  f.screenW = captured.screenW; f.screenH = captured.screenH;
  f.toScreenX = captured.toScreenX;
  f.toScreenY = captured.toScreenY;
  f.windowGeneration = captured.windowGeneration;
  const uint8_t* px = reinterpret_cast<const uint8_t*>(captured.bgra.data());
  f.bgra.assign(px, px + static_cast<size_t>(f.w) * f.h * 4);
  f.gray.resize(static_cast<size_t>(f.w) * f.h);
  for (int i = 0; i < f.w * f.h; ++i) {
    const uint8_t b = px[i * 4], g = px[i * 4 + 1], r = px[i * 4 + 2];
    f.gray[i] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
  }
  std::string diag;
  RoiInfo roi = detectMinigame(f, &diag);
  Rect target;
  std::vector<Rect> components;
  if (!roi.isMinigame || !detectRois(f, roi, target, components, &diag)) {
    gDetectedGeometry = {};
    return false;
  }
  gDetectedGeometry.valid = true;
  gDetectedGeometry.windowGeneration = f.windowGeneration;
  gDetectedGeometry.frameW = f.w;
  gDetectedGeometry.frameH = f.h;
  gDetectedGeometry.roi = roi;
  gDetectedGeometry.target = target;
  gDetectedGeometry.components = std::move(components);
  return true;
}
void ResetInGameCache() { gDetectedGeometry = {}; }
HWND OverlayWindow() { return gOverlayWnd; }
void SetOverlayWindow(HWND hwnd) { gOverlayWnd = hwnd; }
void SetUiThread() { gUiThreadId = GetCurrentThreadId(); }
void InitStateLock() { InitializeCriticalSection(&gStateLock); }
void DeleteStateLock() { DeleteCriticalSection(&gStateLock); }
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return overlayProc(hwnd, msg, wp, lp); }
void ClearOverlay() { publishState({}); if (gOverlayWnd) ShowWindow(gOverlayWnd, SW_HIDE); }
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status) {
  constexpr int kAutomationMissToleranceFrames = 3;
  constexpr int kOverlayMissToleranceFrames = 8;
  std::wstring lastStatus;
  bool overlayShown = false;
  bool retainedUsable = false;
  OverlayState retainedState;
  int overlayX = 0, overlayY = 0, overlayW = 0, overlayH = 0;
  auto setStatus = [&](const std::wstring& text) {
    if (text != lastStatus) {
      lastStatus = text;
      status(text);
    }
  };
  auto syncOverlay = [&] {
    if (!gOverlayWnd) return;
    if (overlayEnabled()) {
      const bool moved = overlayX != gVirtualX || overlayY != gVirtualY
          || overlayW != gVirtualW || overlayH != gVirtualH;
      if (!overlayShown || moved) {
        UINT flags = SWP_NOACTIVATE;
        if (!overlayShown) flags |= SWP_SHOWWINDOW;
        SetWindowPos(gOverlayWnd, HWND_TOPMOST, gVirtualX, gVirtualY, gVirtualW, gVirtualH, flags);
        overlayX = gVirtualX; overlayY = gVirtualY;
        overlayW = gVirtualW; overlayH = gVirtualH;
        if (!overlayShown && retainedUsable) publishState(retainedState);
        overlayShown = true;
      }
    } else if (overlayShown) {
      publishState({});
      ShowWindow(gOverlayWnd, SW_HIDE);
      overlayShown = false;
    }
  };
  SolverCache cache;
  InGameGeometry geometry = gDetectedGeometry;
  AutomationState automation;
  gRunning.store(true);
  RECT game{};
  if (!gta5::capture::GetGameClientRect(game)) {
    gRunning.store(false);
    ResetInGameCache();
    return false;
  }
  gVirtualX = game.left; gVirtualY = game.top;
  gVirtualW = game.right - game.left; gVirtualH = game.bottom - game.top;
  syncOverlay();
  bool completedAnyLevel = false;
  int lostFrames = 0;
  int invalidFrames = 0;
  setStatus(L"fingerprint: locating");
  while (!stopRequested()) {
    syncOverlay();
    Frame frame; FrameTiming timing;
    if (!captureScreen(frame)) {
      geometry = {};
      cache = {};
      ResetInGameCache();
      Sleep(30);
      continue;
    }
    if (geometry.valid && geometry.windowGeneration != frame.windowGeneration) {
      geometry = {};
      cache = {};
      ResetInGameCache();
    }
    const bool waitingForLevel =
        automation.phase == AutomationPhase::Submitting ||
        automation.phase == AutomationPhase::WaitingLevel;
    if (waitingForLevel && automation.plannedLevelMarker >= 0) {
      int levelMarker = -1;
      if (!validateMinigameGeometry(frame, geometry)) {
        RoiInfo relocatedRoi = detectMinigame(frame);
        Rect relocatedTarget;
        std::vector<Rect> relocatedComponents;
        if (relocatedRoi.isMinigame &&
            detectRois(frame, relocatedRoi, relocatedTarget, relocatedComponents)) {
          InGameGeometry relocated;
          relocated.valid = true;
          relocated.windowGeneration = frame.windowGeneration;
          relocated.frameW = frame.w;
          relocated.frameH = frame.h;
          relocated.roi = relocatedRoi;
          relocated.target = relocatedTarget;
          relocated.components = std::move(relocatedComponents);
          geometry = std::move(relocated);
          cache = {};
        } else {
          geometry = {};
          cache = {};
        }
      }
      Rect markerLine;
      if (geometry.valid) {
        levelMarker = detectLevelMarker(frame, geometry.roi.bars.signals, nullptr, &markerLine);
        if (levelMarker >= 0 && markerLine.w > 0 && retainedUsable) {
          retainedState.levelMarker = levelMarker;
          retainedState.levelMarkerLine = scaleRectToScreen(frame, markerLine);
          if (overlayEnabled()) publishState(retainedState);
        }
      }

      if (levelMarker >= 0 && levelMarker != automation.plannedLevelMarker) {
        if (automation.observedLevelMarker == levelMarker) {
          ++automation.levelChangeFrames;
        } else {
          automation.observedLevelMarker = levelMarker;
          automation.levelChangeFrames = 1;
        }
      } else {
        automation.observedLevelMarker = -1;
        automation.levelChangeFrames = 0;
      }

      if (automation.levelChangeFrames >= 2) {
        completedAnyLevel = true;
        setStatus(L"fingerprint: level complete");
        resetAutomation(automation);
        cache = {};
        invalidFrames = 0;
        retainedUsable = false;
        publishState({});
        Sleep(120);
        continue;
      }

      if (automation.phase == AutomationPhase::Submitting && automation.inputJob) {
        if (automation.inputJob.Pending()) {
          setStatus(L"fingerprint: waiting next level");
          Sleep(kFrameDelayMs);
          continue;
        }
        if (!automation.inputJob.Succeeded()) {
          resetAutomation(automation);
          cache = {};
          setStatus(L"fingerprint: locating");
          continue;
        }
        automation.inputJob = {};
        automation.phase = AutomationPhase::WaitingLevel;
        automation.submittedAt = Clock::now();
      }

      if (automation.phase == AutomationPhase::WaitingLevel &&
          Clock::now() - automation.submittedAt >= std::chrono::milliseconds(3000)) {
        resetAutomation(automation);
        cache = {};
        setStatus(L"fingerprint: locating");
        continue;
      }

      setStatus(L"fingerprint: waiting next level");
      Sleep(kFrameDelayMs);
      continue;
    }

    OverlayState state = analyzeFrame(frame, cache, nullptr, &timing, &geometry);
    if (!timing.minigame) {
      ++invalidFrames;
      if (++lostFrames >= 15) {
        setStatus(L"fingerprint: exited");
        break;
      }
      setStatus(L"fingerprint: confirming exit");
      if (invalidFrames == kAutomationMissToleranceFrames) resetAutomation(automation);
      if (invalidFrames == kOverlayMissToleranceFrames) {
        retainedUsable = false;
        publishState({});
      }
      Sleep(50);
      continue;
    }
    lostFrames = 0;
    if (!state.visible) {
      ++invalidFrames;
      if (invalidFrames == kAutomationMissToleranceFrames) resetAutomation(automation);
      if (invalidFrames == kOverlayMissToleranceFrames) {
        retainedUsable = false;
        publishState({});
      }
      setStatus(L"fingerprint: locating");
      Sleep(kFrameDelayMs);
      continue;
    }
    invalidFrames = 0;
    retainedState = scaleOverlayStateToScreen(frame, state);
    retainedUsable = true;
    if (overlayEnabled()) publishState(retainedState);
    std::string autoDiag;
    setStatus(state.visible ? L"fingerprint: auto input" : L"fingerprint: locating");
    const int levelMarker = state.levelMarker;
    if (levelMarker >= 0) {
      planAndRunAutomation(state, automation, &autoDiag, levelMarker);
    }
    Sleep(kFrameDelayMs);
  }
  gRunning.store(false);
  resetAutomation(automation);
  ClearOverlay();
  ResetInGameCache();
  return completedAnyLevel;
}

}  // namespace gta5::games::choose_fingerprint

#ifdef CLI_TEST
int wmain(int argc, wchar_t** argv) {
    return gta5::games::choose_fingerprint::runCli(argc, argv);
}
#endif
