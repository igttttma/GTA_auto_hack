#include "games.h"
#include "../capture/game_window.h"
#include "../input/key_input.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gta5::games::fleeca {
namespace {

struct Point { int x, y; };
struct Box { int x, y, w, h, area; };
struct Image {
    int width = 0, height = 0;
    int screen_x = 0, screen_y = 0;
    double to_screen_x = 1.0, to_screen_y = 1.0;
    std::uint64_t window_generation = 0;
    std::vector<uint8_t> bgra;
    const uint8_t* pixel(int x, int y) const { return &bgra[(y * width + x) * 4]; }
};
struct FrameTiming { LONGLONG last_present_qpc = 0; UINT accumulated_frames = 0; };
struct Connectors { Point first, second; Box first_box, second_box; };
struct InGameGeometry {
    bool valid = false;
    std::uint64_t window_generation = 0;
    int width = 0, height = 0;
    Connectors connectors{};
};
InGameGeometry g_detected_geometry;
struct Map {
    int x1, y1, x2, y2, width, height, grid;
    std::vector<uint8_t> blocked;
    Point start, exit;
    int start_direction = 4, exit_direction = 4;
    int minimum_start_run = 0, minimum_exit_run = 0;
    double signal_head_radius = 0.0, connector_long_side = 0.0;
};
struct SearchRegion { int x1 = 0, y1 = 0, x2 = 0, y2 = 0; };
struct Run { int dx, dy; double distance; Point target; SearchRegion search; };

constexpr int REFERENCE_GAME_HEIGHT = 1080;
constexpr int REFERENCE_GRID = 4;
constexpr double SIGNAL_RADIUS_TO_CONNECTOR_LONG = 0.105;
constexpr double WALL_CLEARANCE_TO_CONNECTOR_SHORT = 0.16;
constexpr double SIGNAL_CORE_TO_HEAD_RADIUS = 0.72;
constexpr double SIGNAL_CORE_MINIMUM_FILL_RATIO = 0.78;
constexpr double SIGNAL_CORE_QUADRANT_MINIMUM_FILL_RATIO = 0.64;
constexpr double SIGNAL_OUTER_PROBE_TO_HEAD_RADIUS = 1.30;
constexpr double SIGNAL_MAX_FRAME_TRAVEL_TO_CONNECTOR_LONG = 0.45;
constexpr double ANALYSIS_INTERVAL_SECONDS = 1.0 / 30.0;
constexpr double TURN_MOTION_TO_CONNECTOR_LONG = 0.012;
constexpr double LATENCY_SAMPLE_MAX_SECONDS = 0.500;
constexpr int HEAD_LOST_CONSECUTIVE_FRAMES = 30;
constexpr double WALL_CENTERING_WEIGHT = 8.0;
constexpr double ROUTE_TURN_COST = 20.0;
constexpr WORD KEY_ENTER = 0x0D;
constexpr WORD KEY_LEFT = 0x25;
constexpr WORD KEY_UP = 0x26;
constexpr WORD KEY_RIGHT = 0x27;
constexpr WORD KEY_DOWN = 0x28;

std::function<bool()> g_stop_callback;
std::function<void(const std::wstring&)> g_status;
bool g_completed_round = false;

bool stop_requested() { return g_stop_callback && g_stop_callback(); }
void set_status(const wchar_t* value) { if (g_status) g_status(value); }
void append_log(const std::wstring&) {}
void update_speed_display(int) {}

struct SpeedTracker {
    using Clock = std::chrono::steady_clock;
    Point anchor;
    Clock::time_point anchor_time;
    double smoothed_speed = 0.0;
    bool has_speed = false;
    explicit SpeedTracker(Point initial) : anchor(initial), anchor_time(Clock::now()) {}
    double pixels_per_second() const { return has_speed ? smoothed_speed : 0.0; }
    void begin_segment(Point current) {
        anchor = current;
        anchor_time = Clock::now();
    }
    void observe(Point current, int axis_x, int axis_y) {
        Clock::time_point now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - anchor_time).count();
        if (elapsed < 0.080) return;
        double travelled = std::abs(double(current.x - anchor.x) * axis_x +
                                    double(current.y - anchor.y) * axis_y);
        double measured = travelled / elapsed;
        smoothed_speed = has_speed ? smoothed_speed * 0.65 + measured * 0.35 : measured;
        has_speed = true;
        anchor = current;
        anchor_time = now;
    }
};

struct EndToEndLatencyEstimator {
    std::vector<double> samples;
    double seconds = 0.0;
    bool calibrated = false;

    void reset() {
        samples.clear();
        seconds = 0.0;
        calibrated = false;
    }
    bool observe(double sample_seconds) {
        if (sample_seconds <= 0.0 || sample_seconds > LATENCY_SAMPLE_MAX_SECONDS) return false;
        samples.push_back(sample_seconds);
        constexpr size_t maximum_samples = 7;
        if (samples.size() > maximum_samples) samples.erase(samples.begin());
        std::vector<double> ordered = samples;
        std::sort(ordered.begin(), ordered.end());
        double median = ordered[ordered.size() / 2];
        constexpr double measurement_weight = 0.35;
        seconds = calibrated
            ? seconds * (1.0 - measurement_weight) + median * measurement_weight
            : median;
        calibrated = true;
        return true;
    }
    double prediction_seconds(double fallback_seconds) const {
        return calibrated ? seconds : fallback_seconds;
    }
};

EndToEndLatencyEstimator g_input_latency;

bool capture_client(Image& image, FrameTiming* timing = nullptr) {
    gta5::capture::GameFrame frame;
    if (!gta5::capture::CaptureGameFrame(frame)) return false;
    image.width = frame.width;
    image.height = frame.height;
    image.screen_x = frame.screenX;
    image.screen_y = frame.screenY;
    image.to_screen_x = frame.toScreenX;
    image.to_screen_y = frame.toScreenY;
    image.window_generation = frame.windowGeneration;
    image.bgra.resize(frame.bgra.size() * sizeof(std::uint32_t));
    std::memcpy(image.bgra.data(), frame.bgra.data(), image.bgra.size());
    if (timing) {
        timing->last_present_qpc = frame.captureQpc;
        timing->accumulated_frames = 1;
    }
    return true;
}

double distance(Point a, Point b) {
    return std::hypot(double(a.x - b.x), double(a.y - b.y));
}

void rgb_to_hsv(uint8_t b, uint8_t g, uint8_t r, int& hue, int& sat, int& value) {
    int maximum = std::max({int(r), int(g), int(b)});
    int minimum = std::min({int(r), int(g), int(b)});
    int delta = maximum - minimum;
    value = maximum;
    sat = maximum == 0 ? 0 : (255 * delta) / maximum;
    if (delta == 0) { hue = 0; return; }
    double h = r == maximum ? 60.0 * (double(g - b) / delta)
             : g == maximum ? 60.0 * (2.0 + double(b - r) / delta)
                            : 60.0 * (4.0 + double(r - g) / delta);
    if (h < 0) h += 360.0;
    hue = int(std::lround(h / 2.0)); // OpenCV-compatible 0..179 scale.
}

Point connector_axis_center(const Box& box) { return {box.x + box.w / 2, box.y + box.h / 2}; }

bool has_connector_texture(const Image& image, const Box& box) {
    int dark_gray = 0;
    int light_gray = 0;
    const int pixels = box.w * box.h;
    for (int y = box.y; y < box.y + box.h; ++y) {
        for (int x = box.x; x < box.x + box.w; ++x) {
            const auto* p = image.pixel(x, y);
            int low = std::min({int(p[0]), int(p[1]), int(p[2])});
            int high = std::max({int(p[0]), int(p[1]), int(p[2])});
            if (high - low > 18) continue;
            dark_gray += high >= 55 && high <= 105;
            light_gray += high >= 130 && high <= 190;
        }
    }
    return dark_gray * 5 >= pixels && light_gray * 12 >= pixels;
}

bool is_connector_candidate(const Image& image, const Box& box) {
    const int long_side = std::max(box.w, box.h);
    const int short_side = std::min(box.w, box.h);
    const double long_to_height = double(long_side) / image.height;
    const double short_to_height = double(short_side) / image.height;
    const double aspect = double(long_side) / short_side;
    const double fill = double(box.area) / (box.w * box.h);
    Point center = connector_axis_center(box);
    const bool near_outer_edge = center.x <= image.width * 0.24 ||
                                 center.x >= image.width * 0.76 ||
                                 center.y <= image.height * 0.24 ||
                                 center.y >= image.height * 0.76;
    return long_to_height >= 0.065 && long_to_height <= 0.095 &&
           short_to_height >= 0.040 && short_to_height <= 0.065 &&
           aspect >= 1.30 && aspect <= 1.75 &&
           fill >= 0.85 && fill <= 0.995 && near_outer_edge &&
           has_connector_texture(image, box);
}

Point connector_mouth(const Box& box, const Image& image) {
    Point center = connector_axis_center(box);
    Point board{image.width / 2, image.height / 2};
    int offset = std::max(10, int(std::lround(std::max(box.w, box.h) * 0.14)));
    if (box.h >= box.w) return {center.x + (board.x > center.x ? offset : -offset), center.y};
    return {center.x, center.y + (board.y > center.y ? offset : -offset)};
}

std::optional<Connectors> find_connectors(const Image& image) {
    int total = image.width * image.height;
    std::vector<uint8_t> mask(total), seen(total);
    auto classify_row = [&](int y, int x1, int x2) {
        for (int x = x1; x < x2; ++x) {
            const auto* p = image.pixel(x, y);
            int low = std::min({int(p[0]), int(p[1]), int(p[2])});
            int high = std::max({int(p[0]), int(p[1]), int(p[2])});
            mask[y * image.width + x] = high - low <= 18 && high >= 55 && high <= 190;
        }
    };

    // A connector center must be in the outer 24% and its longest side is at
    // most 9.5% of the image height. Include half that maximum side plus a
    // rounding margin so every valid connector is fully classified, including
    // in narrow or non-1080p windows. Permitted non-gray texture is handled by
    // the fill and texture checks below, not cropped out here.
    const double maximum_half_side = image.height * 0.095 * 0.5;
    const int border_x = std::clamp(
        static_cast<int>(std::ceil(image.width * 0.24 + maximum_half_side)) + 1,
        1, image.width);
    const int border_y = std::clamp(
        static_cast<int>(std::ceil(image.height * 0.24 + maximum_half_side)) + 1,
        1, image.height);
    const int bottom_start = std::max(border_y, image.height - border_y);
    for (int y = 0; y < border_y; ++y) classify_row(y, 0, image.width);
    for (int y = border_y; y < bottom_start; ++y) {
        classify_row(y, 0, border_x);
        classify_row(y, std::max(border_x, image.width - border_x), image.width);
    }
    for (int y = bottom_start; y < image.height; ++y) classify_row(y, 0, image.width);

    std::vector<Box> boxes;
    std::vector<int> todo;
    constexpr int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    constexpr int dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (int start = 0; start < total; ++start) {
        if (!mask[start] || seen[start]) continue;
        todo.clear();
        todo.push_back(start);
        seen[start] = 1;
        int area = 0, x1 = image.width, y1 = image.height, x2 = 0, y2 = 0;
        while (!todo.empty()) {
            int at = todo.back(); todo.pop_back(); int x = at % image.width, y = at / image.width;
            ++area; x1 = std::min(x1, x); y1 = std::min(y1, y); x2 = std::max(x2, x); y2 = std::max(y2, y);
            for (int i = 0; i < 8; ++i) { int nx = x + dx[i], ny = y + dy[i];
                if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height) continue;
                int next = ny * image.width + nx;
                if (mask[next] && !seen[next]) { seen[next] = 1; todo.push_back(next); }
            }
        }
        int width = x2 - x1 + 1, height = y2 - y1 + 1;
        Box box{x1, y1, width, height, area};
        if (area >= total * 0.001 && area <= total * 0.008 &&
            is_connector_candidate(image, box)) boxes.push_back(box);
    }
    double best_score = -1; std::optional<Connectors> best;
    for (size_t a = 0; a < boxes.size(); ++a) for (size_t b = a + 1; b < boxes.size(); ++b) {
        Point first = connector_axis_center(boxes[a]), second = connector_axis_center(boxes[b]);
        double separation = distance(first, second);
        int first_long = std::max(boxes[a].w, boxes[a].h);
        int second_long = std::max(boxes[b].w, boxes[b].h);
        int first_short = std::min(boxes[a].w, boxes[a].h);
        int second_short = std::min(boxes[b].w, boxes[b].h);
        if (separation < std::min(image.width, image.height) * 0.50 ||
            std::min(first_long, second_long) * 100 < std::max(first_long, second_long) * 85 ||
            std::min(first_short, second_short) * 100 < std::max(first_short, second_short) * 85 ||
            std::min(boxes[a].area, boxes[b].area) * 100 <
                std::max(boxes[a].area, boxes[b].area) * 85) continue;
        double score = std::min(boxes[a].area, boxes[b].area) * separation;
        if (score > best_score) { best_score = score; best = {{first, second, boxes[a], boxes[b]}}; }
    }
    return best;
}

bool validate_connectors(const Image& image, const Connectors& connectors) {
    auto valid_box = [&](const Box& box) {
        return box.x >= 0 && box.y >= 0 && box.w > 0 && box.h > 0 &&
               box.x + box.w <= image.width && box.y + box.h <= image.height &&
               is_connector_candidate(image, box);
    };
    return valid_box(connectors.first_box) && valid_box(connectors.second_box);
}

bool is_wall(const uint8_t* p) {
    int hue, sat, value; rgb_to_hsv(p[0], p[1], p[2], hue, sat, value);
    return int(p[1]) - int(p[2]) >= 39 && hue >= 65 && hue <= 95 && sat >= 175 && value >= 45 && value <= 125;
}

std::optional<Map> build_map(const Image& image, const Connectors& connectors) {
    Point first = connector_mouth(connectors.first_box, image);
    Point second = connector_mouth(connectors.second_box, image);
    const int total = image.width * image.height;
    std::vector<uint8_t> wall_mask(total);
    std::vector<int> region_ids(total, -1);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            wall_mask[y * image.width + x] = is_wall(image.pixel(x, y));

    // Board pieces form a few very large connected regions. Text and background
    // glyphs form small islands, so they must not be allowed to stretch the map bounds.
    std::vector<Box> regions;
    constexpr int neighbor_x[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    constexpr int neighbor_y[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (int start = 0; start < total; ++start) {
        if (!wall_mask[start] || region_ids[start] >= 0) continue;
        int region_id = static_cast<int>(regions.size());
        std::queue<int> todo;
        todo.push(start);
        region_ids[start] = region_id;
        int area = 0, left = image.width, top = image.height, right = 0, bottom = 0;
        while (!todo.empty()) {
            int at = todo.front(); todo.pop();
            int x = at % image.width, y = at / image.width;
            ++area;
            left = std::min(left, x); top = std::min(top, y);
            right = std::max(right, x); bottom = std::max(bottom, y);
            for (int i = 0; i < 8; ++i) {
                int nx = x + neighbor_x[i], ny = y + neighbor_y[i];
                if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height) continue;
                int next = ny * image.width + nx;
                if (wall_mask[next] && region_ids[next] < 0) {
                    region_ids[next] = region_id;
                    todo.push(next);
                }
            }
        }
        regions.push_back({left, top, right - left + 1, bottom - top + 1, area});
    }
    if (regions.empty()) return std::nullopt;

    int largest_area = 0;
    for (const Box& region : regions) largest_area = std::max(largest_area, region.area);
    const int minimum_wall_area = std::max(50, largest_area / 100);
    const int minimum_border_area = std::max(100, largest_area / 50);
    for (int i = 0; i < total; ++i) {
        if (wall_mask[i] && regions[region_ids[i]].area < minimum_wall_area) wall_mask[i] = 0;
    }
    int x1 = image.width, y1 = image.height, x2 = 0, y2 = 0;
    for (const Box& region : regions) {
        if (region.area < minimum_border_area) continue;
        x1 = std::min(x1, region.x); y1 = std::min(y1, region.y);
        x2 = std::max(x2, region.x + region.w - 1);
        y2 = std::max(y2, region.y + region.h - 1);
    }
    x1 = std::min({x1, first.x, second.x}); y1 = std::min({y1, first.y, second.y});
    x2 = std::max({x2, first.x, second.x}); y2 = std::max({y2, first.y, second.y});
    if (x2 <= x1 || y2 <= y1) return std::nullopt;
    x2 = std::min(image.width, x2 + 1);
    y2 = std::min(image.height, y2 + 1);
    int grid = std::min(REFERENCE_GRID, std::max(2, static_cast<int>(std::lround(
        double(image.height) * REFERENCE_GRID / REFERENCE_GAME_HEIGHT))));
    int width = (x2 - x1 + grid - 1) / grid;
    int height = (y2 - y1 + grid - 1) / grid;
    double connector_short = (std::min(connectors.first_box.w, connectors.first_box.h) +
                              std::min(connectors.second_box.w, connectors.second_box.h)) / 2.0;
    double connector_long = (std::max(connectors.first_box.w, connectors.first_box.h) +
                             std::max(connectors.second_box.w, connectors.second_box.h)) / 2.0;
    double nominal_head_radius = connector_long * SIGNAL_RADIUS_TO_CONNECTOR_LONG;
    Map map{x1, y1, x2, y2, width, height, grid, std::vector<uint8_t>(width * height),
            first, second, 4, 4, 0, 0, nominal_head_radius, connector_long};
    for (int gy = 0; gy < height; ++gy) for (int gx = 0; gx < width; ++gx) {
        int hits = 0;
        for (int y = y1 + gy * grid; y < std::min(y1 + (gy + 1) * grid, y2); ++y)
            for (int x = x1 + gx * grid; x < std::min(x1 + (gx + 1) * grid, x2); ++x)
                hits += wall_mask[y * image.width + x];
        int minimum_hits = std::max(1, static_cast<int>(std::ceil(grid * grid * 0.20)));
        map.blocked[gy * width + gx] = hits >= minimum_hits;
    }
    for (int x = 0; x < width; ++x) {
        map.blocked[x] = 1;
        map.blocked[(height - 1) * width + x] = 1;
    }
    for (int y = 0; y < height; ++y) {
        map.blocked[y * width] = 1;
        map.blocked[y * width + width - 1] = 1;
    }
    auto clear_or_inflate = [&](bool clear, Point point) {
        int clearance = static_cast<int>(std::ceil(
            connector_short * WALL_CLEARANCE_TO_CONNECTOR_SHORT));
        int cx = (point.x - x1) / grid, cy = (point.y - y1) / grid;
        int radius = (clearance + grid - 1) / grid;
        for (int y = std::max(0, cy - radius); y <= std::min(height - 1, cy + radius); ++y)
            for (int x = std::max(0, cx - radius); x <= std::min(width - 1, cx + radius); ++x)
                map.blocked[y * width + x] = clear ? 0 : 1;
    };
    std::vector<uint8_t> original = map.blocked;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) if (original[y * width + x]) clear_or_inflate(false, {x1 + x * grid, y1 + y * grid});
    clear_or_inflate(true, first); clear_or_inflate(true, second);
    auto inward_direction = [](Point mouth, const Box& box) {
        Point center = connector_axis_center(box);
        int dx = mouth.x - center.x, dy = mouth.y - center.y;
        return std::abs(dx) >= std::abs(dy)
            ? (dx >= 0 ? 0 : 1)
            : (dy >= 0 ? 2 : 3);
    };
    map.start_direction = inward_direction(first, connectors.first_box);
    map.exit_direction = inward_direction(second, connectors.second_box);
    map.minimum_start_run = std::min(connectors.first_box.w, connectors.first_box.h);
    map.minimum_exit_run = std::min(connectors.second_box.w, connectors.second_box.h);
    return map;
}

inline bool is_signal_head_pixel(const uint8_t* p) {
    const int blue = p[0], green = p[1], red = p[2];
    if (green < 185 || green < blue || green < red) return false;
    const int delta = green - std::min(red, blue);
    // Integer form of saturation >= 130 and hue >= 65 on the OpenCV 0..179 scale.
    // With green as the maximum channel, hue cannot exceed the accepted upper bound of 90.
    return 255 * delta >= 130 * green && 60 * (blue - red) >= 9 * delta;
}

inline bool is_result_pixel(const uint8_t* p) {
    const int blue = p[0], green = p[1], red = p[2];
    bool success_green = green >= 150 && green - red >= 55 && green - blue >= 25;
    bool failure_red = red >= 150 && red - green >= 55 && red - blue >= 25;
    return success_green || failure_red;
}

bool detect_result_screen(const Image& image) {
    if (image.height <= 0 || image.width <= 0) return false;
    const int tolerance = std::max(1, int(std::lround(image.height * 0.002)));
    const int search = std::max(2, int(std::lround(image.height * 0.012)));
    const int expected_y = int(std::lround(image.height * 0.450));
    const int spacing = int(std::lround(image.height * 0.072));
    const int minimum_size = std::max(8, int(std::lround(image.height * 0.047)));
    const int maximum_size = std::max(minimum_size, int(std::lround(image.height * 0.057)));

    auto bright = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < image.width && y < image.height &&
               is_result_pixel(image.pixel(x, y));
    };
    auto vertical_edge_score = [&](int x, int y1, int y2) {
        int hits = 0, samples = 0;
        for (int y = y1; y <= y2; y += 2) {
            ++samples;
            bool found = false;
            for (int dx = -tolerance; dx <= tolerance && !found; ++dx) found = bright(x + dx, y);
            hits += found;
        }
        return samples ? double(hits) / samples : 0.0;
    };
    auto horizontal_edge_score = [&](int y, int x1, int x2) {
        int hits = 0, samples = 0;
        for (int x = x1; x <= x2; x += 2) {
            ++samples;
            bool found = false;
            for (int dy = -tolerance; dy <= tolerance && !found; ++dy) found = bright(x, y + dy);
            hits += found;
        }
        return samples ? double(hits) / samples : 0.0;
    };
    auto square_near = [&](int expected_x) {
        for (int size = minimum_size; size <= maximum_size; size += 2) {
            int half = size / 2;
            for (int cy = expected_y - search; cy <= expected_y + search; cy += 2) {
                for (int cx = expected_x - search; cx <= expected_x + search; cx += 2) {
                    int left = cx - half, right = cx + half;
                    int top = cy - half, bottom = cy + half;
                    if (vertical_edge_score(left, top, bottom) >= 0.62 &&
                        vertical_edge_score(right, top, bottom) >= 0.62 &&
                        horizontal_edge_score(top, left, right) >= 0.62 &&
                        horizontal_edge_score(bottom, left, right) >= 0.62) return true;
                }
            }
        }
        return false;
    };

    int center_x = image.width / 2;
    return square_near(center_x - spacing) && square_near(center_x) &&
           square_near(center_x + spacing);
}

std::optional<Point> detect_head(const Image& image, const Map& map, std::optional<Point> previous = {},
                                 double* observed_radius = nullptr,
                                 const SearchRegion* search_region = nullptr) {
    const int radius = std::max(1, static_cast<int>(std::lround(map.signal_head_radius)));
    const int core_radius = std::max(1, static_cast<int>(std::lround(
        map.signal_head_radius * SIGNAL_CORE_TO_HEAD_RADIUS)));
    const int outer_probe = std::max(radius + 1, static_cast<int>(std::lround(
        map.signal_head_radius * SIGNAL_OUTER_PROBE_TO_HEAD_RADIUS)));
    constexpr double pi = 3.14159265358979323846;
    int left = std::max(map.x1 + outer_probe,
                        search_region ? search_region->x1 : map.x1 + outer_probe);
    int top = std::max(map.y1 + outer_probe,
                       search_region ? search_region->y1 : map.y1 + outer_probe);
    int right = std::min(map.x2 - outer_probe - 1,
                         search_region ? search_region->x2 - 1 : map.x2 - outer_probe - 1);
    int bottom = std::min(map.y2 - outer_probe - 1,
                          search_region ? search_region->y2 - 1 : map.y2 - outer_probe - 1);
    if (left > right || top > bottom) return std::nullopt;

    const int mask_left = left - outer_probe;
    const int mask_top = top - outer_probe;
    const int mask_right = right + outer_probe;
    const int mask_bottom = bottom + outer_probe;
    const int mask_width = mask_right - mask_left + 1;
    const int mask_height = mask_bottom - mask_top + 1;
    std::vector<uint8_t> signal_mask(static_cast<size_t>(mask_width) * mask_height);
    std::vector<Point> signal_pixels;
    auto signal_at = [&](int x, int y) {
        return signal_mask[static_cast<size_t>(y - mask_top) * mask_width + x - mask_left] != 0;
    };
    for (int y = mask_top; y <= mask_bottom; ++y) {
        for (int x = mask_left; x <= mask_right; ++x) {
            const bool signal = is_signal_head_pixel(image.pixel(x, y));
            signal_mask[static_cast<size_t>(y - mask_top) * mask_width + x - mask_left] = signal;
            if (signal && x >= left && x <= right && y >= top && y <= bottom)
                signal_pixels.push_back({x, y});
        }
    }

    struct Candidate { Point center{}; double score = -1.0; int head_pixels = 0; };
    Candidate best;
    const int core_radius_squared = core_radius * core_radius;
    const int head_radius_squared = radius * radius;
    const double maximum_travel = map.connector_long_side *
                                  SIGNAL_MAX_FRAME_TRAVEL_TO_CONNECTOR_LONG;
    auto evaluate = [&](int x, int y) {
        if (!signal_at(x, y)) return;
        if (previous && distance({x, y}, *previous) > maximum_travel) return;

        int head_total = 0;
        int head_hits = 0;
        int core_total = 0;
        int core_hits = 0;
        int quadrant_total[4]{};
        int quadrant_hits[4]{};
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int squared_distance = dx * dx + dy * dy;
                if (squared_distance > head_radius_squared) continue;
                const bool signal = signal_at(x + dx, y + dy);
                ++head_total;
                head_hits += signal;
                if (squared_distance > core_radius_squared) continue;
                ++core_total;
                core_hits += signal;
                if (dx == 0 || dy == 0) continue;
                const int quadrant = (dy > 0 ? 2 : 0) + (dx > 0 ? 1 : 0);
                ++quadrant_total[quadrant];
                quadrant_hits[quadrant] += signal;
            }
        }
        const double core_fill = core_total > 0 ? double(core_hits) / core_total : 0.0;
        if (core_fill < SIGNAL_CORE_MINIMUM_FILL_RATIO) return;

        double weakest_quadrant = 1.0;
        for (int quadrant = 0; quadrant < 4; ++quadrant) {
            const double quadrant_fill = quadrant_total[quadrant] > 0
                ? double(quadrant_hits[quadrant]) / quadrant_total[quadrant]
                : 0.0;
            weakest_quadrant = std::min(weakest_quadrant, quadrant_fill);
        }
        if (weakest_quadrant < SIGNAL_CORE_QUADRANT_MINIMUM_FILL_RATIO) return;

        int dark_outer_probes = 0;
        dark_outer_probes += !signal_at(x - outer_probe, y);
        dark_outer_probes += !signal_at(x + outer_probe, y);
        dark_outer_probes += !signal_at(x, y - outer_probe);
        dark_outer_probes += !signal_at(x, y + outer_probe);
        if (dark_outer_probes < 3) return;

        const double head_fill = head_total > 0 ? double(head_hits) / head_total : 0.0;
        const double shape_score = head_fill + core_fill + weakest_quadrant +
                                   dark_outer_probes * 0.05;
        const bool better_shape = shape_score > best.score;
        const bool equal_shape = std::abs(shape_score - best.score) < 1e-9;
        const bool nearer = previous && equal_shape &&
                            distance({x, y}, *previous) < distance(best.center, *previous);
        if (better_shape || nearer) best = {{x, y}, shape_score, head_hits};
    };

    for (Point point : signal_pixels) evaluate(point.x, point.y);

    if (best.score < 0.0) return std::nullopt;
    if (observed_radius) {
        *observed_radius = std::sqrt(best.head_pixels / pi);
    }
    return best.center;
}

int direction(Point a, Point b) { return std::abs(b.x - a.x) >= std::abs(b.y - a.y) ? (b.x >= a.x ? 0 : 1) : (b.y >= a.y ? 2 : 3); }
std::pair<int, int> delta(int d) { static constexpr int dx[] = {1, -1, 0, 0}; static constexpr int dy[] = {0, 0, 1, -1}; return {dx[d], dy[d]}; }
std::optional<std::vector<Run>> plan_path(const Map& map, Point start, int initial_direction) {
    auto cell = [&](Point p) { return Point{std::clamp((p.x - map.x1) / map.grid, 0, map.width - 1), std::clamp((p.y - map.y1) / map.grid, 0, map.height - 1)}; };
    auto point_for_cell = [&](Point p) { return Point{map.x1 + p.x * map.grid + map.grid / 2, map.y1 + p.y * map.grid + map.grid / 2}; };
    Point source = cell(start), target = cell(map.exit), forced_start = start, forced_target = map.exit;
    if (initial_direction < 4 && map.minimum_start_run > 0) {
        auto [forced_dx, forced_dy] = delta(initial_direction);
        int travelled = 0;
        while (travelled < map.minimum_start_run) {
            source.x += forced_dx;
            source.y += forced_dy;
            if (source.x < 0 || source.y < 0 || source.x >= map.width || source.y >= map.height ||
                map.blocked[source.y * map.width + source.x]) return std::nullopt;
            forced_start = point_for_cell(source);
            travelled = (forced_start.x - start.x) * forced_dx + (forced_start.y - start.y) * forced_dy;
        }
    }
    if (map.exit_direction < 4 && map.minimum_exit_run > 0) {
        auto [forced_dx, forced_dy] = delta(map.exit_direction);
        int travelled = 0;
        while (travelled < map.minimum_exit_run) {
            target.x += forced_dx;
            target.y += forced_dy;
            if (target.x < 0 || target.y < 0 || target.x >= map.width || target.y >= map.height ||
                map.blocked[target.y * map.width + target.x]) return std::nullopt;
            forced_target = point_for_cell(target);
            travelled = (forced_target.x - map.exit.x) * forced_dx +
                        (forced_target.y - map.exit.y) * forced_dy;
        }
    }
    // Manhattan distance to the nearest inflated wall. On these orthogonal
    // circuit boards its ridges follow the middle of each traversable channel.
    std::vector<int> wall_distance(map.width * map.height, std::numeric_limits<int>::max());
    std::queue<int> distance_open;
    for (int i = 0; i < map.width * map.height; ++i) {
        if (!map.blocked[i]) continue;
        wall_distance[i] = 0;
        distance_open.push(i);
    }
    while (!distance_open.empty()) {
        int at = distance_open.front();
        distance_open.pop();
        int x = at % map.width, y = at / map.width;
        for (int d = 0; d < 4; ++d) {
            auto [dx, dy] = delta(d);
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= map.width || ny >= map.height) continue;
            int next = ny * map.width + nx;
            if (wall_distance[next] <= wall_distance[at] + 1) continue;
            wall_distance[next] = wall_distance[at] + 1;
            distance_open.push(next);
        }
    }
    struct Node { double score, cost; int x, y, dir; bool operator<(const Node& other) const { return score > other.score; } };
    int states = map.width * map.height * 5; std::vector<double> cost(states, std::numeric_limits<double>::infinity()); std::vector<int> parent(states, -1);
    auto id = [&](int x, int y, int d) { return (y * map.width + x) * 5 + d; };
    std::priority_queue<Node> open; int start_id = id(source.x, source.y, initial_direction); cost[start_id] = 0; open.push({0, 0, source.x, source.y, initial_direction}); int goal = -1;
    while (!open.empty()) { Node now = open.top(); open.pop(); int now_id = id(now.x, now.y, now.dir); if (now.cost != cost[now_id]) continue; if (now.x == target.x && now.y == target.y && (map.exit_direction >= 4 || now.dir == (map.exit_direction ^ 1))) { goal = now_id; break; }
        for (int d = 0; d < 4; ++d) { auto [dx, dy] = delta(d); int nx = now.x + dx, ny = now.y + dy; if (nx < 0 || ny < 0 || nx >= map.width || ny >= map.height || map.blocked[ny * map.width + nx] || (now.dir < 4 && d == (now.dir ^ 1))) continue;
            int clearance = std::max(1, wall_distance[ny * map.width + nx]);
            double wall_cost = WALL_CENTERING_WEIGHT / (double(clearance) * clearance);
            double next = now.cost + 1.0 + wall_cost +
                          (now.dir < 4 && d != now.dir ? ROUTE_TURN_COST : 0.0);
            int next_id = id(nx, ny, d); if (next >= cost[next_id]) continue; cost[next_id] = next; parent[next_id] = now_id; open.push({next + std::abs(nx - target.x) + std::abs(ny - target.y), next, nx, ny, d}); }
    }
    if (goal < 0) return std::nullopt;
    std::vector<Point> points; for (int at = goal; at >= 0; at = parent[at]) { int cell_id = at / 5; points.push_back({map.x1 + (cell_id % map.width) * map.grid + map.grid / 2, map.y1 + (cell_id / map.width) * map.grid + map.grid / 2}); if (at == start_id) break; }
    std::reverse(points.begin(), points.end()); points.front() = forced_start; points.back() = forced_target;
    if (forced_start.x != start.x || forced_start.y != start.y) points.insert(points.begin(), start);
    if (forced_target.x != map.exit.x || forced_target.y != map.exit.y) points.push_back(map.exit);

    auto clear_axis_segment = [&](Point a, Point b) {
        if (a.x != b.x && a.y != b.y) return false;
        Point first_cell = cell(a), last_cell = cell(b);
        int step_x = (last_cell.x > first_cell.x) - (last_cell.x < first_cell.x);
        int step_y = (last_cell.y > first_cell.y) - (last_cell.y < first_cell.y);
        Point at = first_cell;
        while (true) {
            if (map.blocked[at.y * map.width + at.x]) return false;
            if (at.x == last_cell.x && at.y == last_cell.y) return true;
            at.x += step_x;
            at.y += step_y;
        }
    };
    std::vector<Point> simplified;
    for (size_t i = 0; i < points.size();) {
        simplified.push_back(points[i]);
        if (i + 1 == points.size()) break;
        size_t next = i + 1;
        for (size_t candidate = points.size() - 1; candidate > i + 1; --candidate) {
            if (clear_axis_segment(points[i], points[candidate])) {
                next = candidate;
                break;
            }
        }
        i = next;
    }
    points = std::move(simplified);

    std::vector<Run> runs;
    for (size_t i = 1; i < points.size(); ++i) { int d = direction(points[i - 1], points[i]); auto [dx, dy] = delta(d); double segment = distance(points[i - 1], points[i]); if (!runs.empty() && runs.back().dx == dx && runs.back().dy == dy) { runs.back().distance += segment; runs.back().target = points[i]; } else runs.push_back({dx, dy, segment, points[i], {}}); }

    int half_width = std::max(1, static_cast<int>(std::lround(map.connector_long_side)));
    Point region_start = start;
    for (Run& run : runs) {
        if (run.dx != 0) {
            int center_y = (region_start.y + run.target.y) / 2;
            run.search = {std::min(region_start.x, run.target.x), center_y - half_width,
                          std::max(region_start.x, run.target.x) + 1, center_y + half_width + 1};
        } else {
            int center_x = (region_start.x + run.target.x) / 2;
            run.search = {center_x - half_width, std::min(region_start.y, run.target.y),
                          center_x + half_width + 1, std::max(region_start.y, run.target.y) + 1};
        }
        region_start = run.target;
    }
    return runs;
}

std::vector<Run> reverse_route(Point original_start, const std::vector<Run>& runs) {
    std::vector<Point> points;
    points.reserve(runs.size() + 1);
    points.push_back(original_start);
    for (const Run& run : runs) points.push_back(run.target);

    std::vector<Run> reversed;
    reversed.reserve(runs.size());
    for (size_t i = runs.size(); i > 0; --i) {
        const Run& original = runs[i - 1];
        reversed.push_back({-original.dx, -original.dy, original.distance,
                            points[i - 1], original.search});
    }
    return reversed;
}

bool send_key(WORD keycode) {
    gta5::input::Key key = gta5::input::Key::FromVirtualKey(keycode);
    return static_cast<bool>(
        gta5::input::QueueSequence(std::vector<gta5::input::Key>{key}));
}

WORD keycode_for_direction(int d) {
    static constexpr WORD keys[] = {KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP};
    return keys[std::clamp(d, 0, 3)];
}

void wait_precisely(double seconds) {
    if (seconds > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

constexpr int SESSION_INGAME_LOST = 2;
constexpr int SESSION_GEOMETRY_CHANGED = 3;
constexpr int INGAME_LOST_CONSECUTIVE_FRAMES = 3;
constexpr DWORD RESULT_SCREEN_TIMEOUT_MS = 2000;
constexpr DWORD RESULT_ENTER_DELAY_MS = 2000;
constexpr DWORD NEXT_ROUND_TIMEOUT_MS = 2000;

int run_ingame_session(Image& frame, Connectors& connectors, std::uint64_t& geometry_generation) {
    update_speed_display(0);
    struct SessionSpeedReset {
        ~SessionSpeedReset() { update_speed_display(0); }
    } session_speed_reset;
    int missing_ingame_frames = 0;
    LARGE_INTEGER performance_frequency{};
    QueryPerformanceFrequency(&performance_frequency);
    FrameTiming frame_timing{};
    LONGLONG previous_present_qpc = 0;
    double display_frame_seconds = ANALYSIS_INTERVAL_SECONDS;
    bool has_display_frame_seconds = false;
    using AnalysisClock = std::chrono::steady_clock;
    const auto analysis_interval = std::chrono::duration_cast<AnalysisClock::duration>(
        std::chrono::duration<double>(ANALYSIS_INTERVAL_SECONDS));
    auto next_analysis_time = AnalysisClock::now();
    auto wait_for_analysis_tick = [&]() {
        auto now = AnalysisClock::now();
        if (now < next_analysis_time) std::this_thread::sleep_until(next_analysis_time);
        now = AnalysisClock::now();
        next_analysis_time += analysis_interval;
        if (next_analysis_time <= now) next_analysis_time = now + analysis_interval;
    };
    auto capture_ingame_frame = [&]() {
        wait_for_analysis_tick();
        FrameTiming captured_timing{};
        const bool captured = capture_client(frame, &captured_timing);
        if (!captured) {
            g_detected_geometry = {};
            set_status(L"fleeca: capture failed");
            append_log(L"GTA5_Enhanced.exe window capture was lost.");
            return 1;
        }
        if (captured_timing.last_present_qpc > previous_present_qpc &&
            previous_present_qpc > 0 && captured_timing.accumulated_frames > 0 &&
            performance_frequency.QuadPart > 0) {
            double elapsed = double(captured_timing.last_present_qpc - previous_present_qpc) /
                             double(performance_frequency.QuadPart);
            double measured_frame = elapsed / captured_timing.accumulated_frames;
            if (measured_frame >= 0.002 && measured_frame <= 0.100) {
                constexpr double new_measurement_weight = 0.35;
                display_frame_seconds = has_display_frame_seconds
                    ? display_frame_seconds * (1.0 - new_measurement_weight) +
                      measured_frame * new_measurement_weight
                    : measured_frame;
                has_display_frame_seconds = true;
            }
        }
        if (captured_timing.last_present_qpc > 0)
            previous_present_qpc = captured_timing.last_present_qpc;
        frame_timing = captured_timing;
        if (frame.window_generation == geometry_generation && validate_connectors(frame, connectors)) {
            missing_ingame_frames = 0;
            return 0;
        }
        const auto relocated = find_connectors(frame);
        if (relocated) {
            const bool geometry_changed = frame.window_generation != geometry_generation ||
                relocated->first_box.x != connectors.first_box.x ||
                relocated->first_box.y != connectors.first_box.y ||
                relocated->second_box.x != connectors.second_box.x ||
                relocated->second_box.y != connectors.second_box.y;
            connectors = *relocated;
            g_detected_geometry = {};
            geometry_generation = frame.window_generation;
            missing_ingame_frames = 0;
            return geometry_changed ? SESSION_GEOMETRY_CHANGED : 0;
        }
        ++missing_ingame_frames;
        return missing_ingame_frames >= INGAME_LOST_CONSECUTIVE_FRAMES
            ? SESSION_INGAME_LOST : 0;
    };
    set_status(L"fleeca: planning route");
    auto maybe_map = build_map(frame, connectors);
    if (!maybe_map) {
        set_status(L"fleeca: route unavailable");
        return 1;
    }
    Map map = *maybe_map;
    auto runs = plan_path(map, map.start, map.start_direction);
    if (!runs || runs->empty()) {
        set_status(L"fleeca: route unavailable");
        return 1;
    }
    set_status(L"fleeca: starting signal");
    std::optional<Point> head;
    double tracked_head_radius = map.signal_head_radius;
    append_log(L"Starting signal with Enter (hardware scan code).");
    if (!send_key(KEY_ENTER)) return 1;
    DWORD deadline = GetTickCount() + 2000;
    do {
        int frame_status = capture_ingame_frame();
        if (frame_status != 0) return frame_status;
        head = detect_head(frame, map, {}, &tracked_head_radius);
    }
    while (!head && !stop_requested() && GetTickCount() < deadline);
    if (stop_requested()) return 1;
    if (!head) {
        set_status(L"fleeca: signal not detected");
        return 1;
    }

    if (distance(*head, map.exit) < distance(*head, map.start)) {
        runs = reverse_route(map.start, *runs);
        std::swap(map.start, map.exit);
        std::swap(map.start_direction, map.exit_direction);
        std::swap(map.minimum_start_run, map.minimum_exit_run);
        append_log(L"Oriented the fixed route from the endpoint nearest the live signal.");
    }

    std::vector<int> planned_directions;
    planned_directions.reserve(runs->size());
    for (const Run& run : *runs) {
        planned_directions.push_back(
            run.dx > 0 ? 0 : run.dx < 0 ? 1 : run.dy > 0 ? 2 : 3);
    }

    SpeedTracker speed_tracker(*head);
    Point last_observed_head = *head;
    LONGLONG last_observed_qpc = frame_timing.last_present_qpc;
    int pending_turn_direction = 4;
    LONGLONG pending_turn_qpc = 0;
    Point pending_turn_point{};
    double pending_turn_speed = 0.0;
    set_status(L"fleeca: navigating");
    for (size_t i = 0; i < runs->size(); ++i) {
        if (stop_requested()) return 1;
        const Run& run = (*runs)[i];
        const int planned_direction = planned_directions[i];
        int missed_head_frames = 0;
        speed_tracker.begin_segment(*head);
        LARGE_INTEGER direction_requested{};
        QueryPerformanceCounter(&direction_requested);
        if (!send_key(keycode_for_direction(planned_direction))) return 1;
        if (i > 0 && planned_direction != planned_directions[i - 1]) {
            pending_turn_direction = planned_direction;
            pending_turn_qpc = direction_requested.QuadPart;
            pending_turn_point = (*runs)[i - 1].target;
            pending_turn_speed = speed_tracker.pixels_per_second();
        }
        while (true) {
            if (stop_requested()) return 1;
            int frame_status = capture_ingame_frame();
            if (frame_status != 0) return frame_status;
            double measured_radius = tracked_head_radius;
            const std::optional<Point> tracking_anchor = missed_head_frames >= 2
                ? std::optional<Point>{}
                : head;
            auto observed = detect_head(frame, map, tracking_anchor,
                                        &measured_radius, &run.search);
            if (!observed) {
                ++missed_head_frames;
                if (missed_head_frames == HEAD_LOST_CONSECUTIVE_FRAMES)
                    append_log(L"HEAD LOST for 30 captured frames; direction cannot be verified.");
                continue;
            }
            if (missed_head_frames >= HEAD_LOST_CONSECUTIVE_FRAMES)
                append_log(L"HEAD FOUND again.");
            missed_head_frames = 0;
            Point current_head = *observed;
            bool reset_speed_anchor = false;
            if (pending_turn_direction < 4 && pending_turn_qpc > 0 &&
                frame_timing.last_present_qpc > pending_turn_qpc &&
                performance_frequency.QuadPart > 0) {
                auto [turn_x, turn_y] = delta(pending_turn_direction);
                int movement_x = current_head.x - last_observed_head.x;
                int movement_y = current_head.y - last_observed_head.y;
                double forward = double(movement_x * turn_x + movement_y * turn_y);
                double perpendicular = std::abs(double(movement_x * turn_y - movement_y * turn_x));
                double minimum_motion = map.connector_long_side *
                                        TURN_MOTION_TO_CONNECTOR_LONG;
                if (forward >= minimum_motion && forward * 2.0 >= perpendicular * 3.0) {
                    LONGLONG transition_qpc = frame_timing.last_present_qpc;
                    if (pending_turn_speed > 1.0) {
                        double travelled_after_turn =
                            double(current_head.x - pending_turn_point.x) * turn_x +
                            double(current_head.y - pending_turn_point.y) * turn_y;
                        if (travelled_after_turn >= 0.0) {
                            double elapsed_after_turn = travelled_after_turn /
                                                        pending_turn_speed;
                            transition_qpc -= static_cast<LONGLONG>(std::llround(
                                elapsed_after_turn * performance_frequency.QuadPart));
                        }
                    } else {
                        LONGLONG last_old_frame = std::max(pending_turn_qpc,
                                                          last_observed_qpc);
                        transition_qpc = last_old_frame +
                            (frame_timing.last_present_qpc - last_old_frame) / 2;
                    }
                    transition_qpc = std::clamp(transition_qpc, pending_turn_qpc,
                                                frame_timing.last_present_qpc);
                    double sample = double(transition_qpc - pending_turn_qpc) /
                                    double(performance_frequency.QuadPart);
                    g_input_latency.observe(sample);
                    pending_turn_direction = 4;
                    pending_turn_qpc = 0;
                    pending_turn_speed = 0.0;
                    reset_speed_anchor = true;
                } else {
                    double pending_seconds = double(frame_timing.last_present_qpc -
                                                    pending_turn_qpc) /
                                             double(performance_frequency.QuadPart);
                    if (pending_seconds > LATENCY_SAMPLE_MAX_SECONDS) {
                        pending_turn_direction = 4;
                        pending_turn_qpc = 0;
                        pending_turn_speed = 0.0;
                        reset_speed_anchor = true;
                    }
                }
            }
            last_observed_head = current_head;
            last_observed_qpc = frame_timing.last_present_qpc;
            head = current_head;
            if (reset_speed_anchor) {
                speed_tracker.begin_segment(*head);
            } else if (pending_turn_direction >= 4) {
                speed_tracker.observe(*head, run.dx, run.dy);
            }
            tracked_head_radius = tracked_head_radius * 0.75 + measured_radius * 0.25;

            double remaining = (run.target.x - head->x) * run.dx + (run.target.y - head->y) * run.dy;
            if (i + 1 == runs->size()) {
                if (remaining <= 0.0) break;
            } else {
                double speed = speed_tracker.pixels_per_second();
                if (speed > 1.0) {
                    LARGE_INTEGER now_qpc{};
                    QueryPerformanceCounter(&now_qpc);
                    double frame_age = 0.0;
                    if (frame_timing.last_present_qpc > 0 &&
                        performance_frequency.QuadPart > 0 &&
                        now_qpc.QuadPart >= frame_timing.last_present_qpc) {
                        frame_age = double(now_qpc.QuadPart - frame_timing.last_present_qpc) /
                                    double(performance_frequency.QuadPart);
                        if (frame_age > 0.100) frame_age = 0.0;
                    }
                    double eta_from_capture = std::max(0.0, remaining) / speed;
                    double input_latency = g_input_latency.prediction_seconds(
                        display_frame_seconds);
                    double delay_until_key = eta_from_capture - frame_age - input_latency;
                    double until_next_analysis = std::max(0.0,
                        std::chrono::duration<double>(next_analysis_time - AnalysisClock::now()).count());
                    double until_next_decision = until_next_analysis + display_frame_seconds;
                    if (delay_until_key <= until_next_decision) {
                        wait_precisely(std::max(0.0, delay_until_key));
                        break;
                    }
                } else if (remaining <= 0.0) {
                    break;
                }
            }

        }
    }
    set_status(L"fleeca: waiting for result");
    while (!stop_requested()) {
        int frame_status = capture_ingame_frame();
        if (frame_status != 0) return frame_status;
    }
    return 1;
}

int run_live() {
    struct SpeedReset {
        ~SpeedReset() { update_speed_display(0); }
    } speed_reset;
    if (stop_requested()) return 1;

    Image frame;
    if (!capture_client(frame)) {
        append_log(L"Could not find or capture the GTA5_Enhanced.exe window.");
        return 1;
    }
    std::optional<Connectors> connectors;
    std::uint64_t geometry_generation = 0;
    if (g_detected_geometry.valid &&
        g_detected_geometry.window_generation == frame.window_generation &&
        g_detected_geometry.width == frame.width && g_detected_geometry.height == frame.height &&
        validate_connectors(frame, g_detected_geometry.connectors)) {
        connectors = g_detected_geometry.connectors;
        geometry_generation = g_detected_geometry.window_generation;
    } else {
        connectors = find_connectors(frame);
        if (connectors) geometry_generation = frame.window_generation;
    }
    if (!connectors) {
        append_log(L"Minigame not detected in the initial frame.");
        return 1;
    }

    while (!stop_requested()) {
        int session_result = run_ingame_session(frame, *connectors, geometry_generation);
        if (session_result == SESSION_GEOMETRY_CHANGED) {
            set_status(L"fleeca: geometry relocated; rebuilding route");
            continue;
        }
        if (session_result != SESSION_INGAME_LOST) return session_result;

        connectors.reset();

        set_status(L"fleeca: detecting result");
        bool result_detected = detect_result_screen(frame);
        DWORD result_deadline = GetTickCount() + RESULT_SCREEN_TIMEOUT_MS;
        while (!stop_requested() && !result_detected && GetTickCount() < result_deadline) {
            Sleep(20);
            if (!capture_client(frame)) {
                append_log(L"Could not capture the GTA5_Enhanced.exe window while waiting for a result screen.");
                return 1;
            }
            result_detected = detect_result_screen(frame);
        }
        if (stop_requested()) return 1;
        if (!result_detected) {
            set_status(L"fleeca: result not detected");
            append_log(L"No success or failure marker detected within 2 seconds. Stopping.");
            return 1;
        }
        g_completed_round = true;
        set_status(L"fleeca: result detected; waiting for game");
        DWORD enter_deadline = GetTickCount() + RESULT_ENTER_DELAY_MS;
        while (!stop_requested() && GetTickCount() < enter_deadline) Sleep(20);
        if (stop_requested()) return 1;
        set_status(L"fleeca: advancing");
        if (!send_key(KEY_ENTER)) return 1;

        set_status(L"fleeca: waiting for next round");
        DWORD next_round_deadline = GetTickCount() + NEXT_ROUND_TIMEOUT_MS;
        do {
            Sleep(20);
            if (!capture_client(frame)) {
                append_log(L"Could not capture the GTA5_Enhanced.exe window while waiting for the next round.");
                return 1;
            }
            connectors = find_connectors(frame);
            if (connectors) geometry_generation = frame.window_generation;
        } while (!stop_requested() && !connectors && GetTickCount() < next_round_deadline);
        if (stop_requested()) return 1;
        if (!connectors) {
            set_status(L"fleeca: completed");
            return 0;
        }
        set_status(L"fleeca: next round");
    }

    return 1;
}

}  // namespace

bool DetectInGame(const gta5::capture::GameFrame& captured) {
    Image frame;
    frame.width = captured.width;
    frame.height = captured.height;
    frame.screen_x = captured.screenX;
    frame.screen_y = captured.screenY;
    frame.to_screen_x = captured.toScreenX;
    frame.to_screen_y = captured.toScreenY;
    frame.window_generation = captured.windowGeneration;
    frame.bgra.resize(captured.bgra.size() * sizeof(std::uint32_t));
    std::memcpy(frame.bgra.data(), captured.bgra.data(), frame.bgra.size());
    const auto connectors = find_connectors(frame);
    if (!connectors) {
        g_detected_geometry = {};
        return false;
    }
    g_detected_geometry.valid = true;
    g_detected_geometry.window_generation = frame.window_generation;
    g_detected_geometry.width = frame.width;
    g_detected_geometry.height = frame.height;
    g_detected_geometry.connectors = *connectors;
    return true;
}

void ResetInGameCache() { g_detected_geometry = {}; }

bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<void(const std::wstring&)>& status) {
    g_stop_callback = stopRequested;
    g_status = status;
    g_completed_round = false;
    g_input_latency.reset();
    const int result = run_live();
    g_stop_callback = {};
    g_status = {};
    ResetInGameCache();
    return result == 0 || g_completed_round;
}

}  // namespace gta5::games::fleeca
