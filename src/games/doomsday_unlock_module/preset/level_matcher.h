#pragma once

// C++ port of src/level_matcher.py: grid-node level recognition and
// authored-id binding. Signatures load from maps/level*/layout.json.

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/board_grid.h"
#include "json/json.h"

namespace matcher {

// Board-grid constants live in common/board_grid.h (shared with the vision
// snap); re-exported here under matcher:: for existing users.
constexpr double kRefW = grid::kRefW;
constexpr double kRefH = grid::kRefH;
constexpr double kStep = grid::kStep;
constexpr double kFirstX = grid::kFirstX;
constexpr double kFirstY = grid::kFirstY;

struct LevelMatchError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Signature {
  struct NodeInfo {
    std::string id;
    bool hidden = false;
  };
  // mirrors: grid node (in steps, may be half-integers after normalization)
  // -> id/hidden; off_grid mirrors keep their continuous position.
  std::map<std::pair<double, double>, NodeInfo> mirrors;
  std::vector<std::pair<std::pair<double, double>, NodeInfo>> off_grid;
  std::map<std::pair<double, double>, std::string> targets;
  int level = 0;
};

struct MatchResult {
  int level = 0;
  // detection index (in the order given by the caller) -> authored id
  std::map<int, std::string> mirrors;
  std::map<int, std::string> targets;
  struct MissingTarget {
    std::string id;
    double center_x = 0, center_y = 0;  // board-space px, 2-decimal rounded
  };
  std::vector<MissingTarget> missing_targets;
  // Visible (non-hidden) authored mirrors that no detection bound to: a
  // transient detection miss. The level's preset needs every one of these, so
  // callers treat a non-empty list as "scene not settled yet" and re-detect
  // rather than starting an attack that would abort on an unknown mirror id.
  std::vector<std::string> missing_mirrors;
};

constexpr double kSnapTolerance = 0.45;
constexpr double kMirrorBindTolerance = 0.6;
constexpr double kTargetBindTolerance = 0.5;

// Continuous grid-space coordinates (units of one mirror step).
std::pair<double, double> GridCoords(double cx, double cy, const int board[4]);
// Board-space pixels for a grid node.
std::pair<double, double> NodeCenter(const std::pair<double, double>& node,
                                     const int board[4]);
// Snap to a node; returns false when off-grid.
bool CenterToNode(double cx, double cy, const int board[4], double quantum,
                  double tolerance, std::pair<double, double>* node);

std::map<int, Signature> LoadSignatures();

// Identify the level and bind detections (board-space centers) to authored
// ids. Throws LevelMatchError when detection does not identify exactly one
// level.
MatchResult MatchLevel(const std::vector<std::pair<double, double>>& mirrors,
                       const std::vector<std::pair<double, double>>& targets,
                       const int board[4],
                       const std::map<int, Signature>* signatures = nullptr);

}  // namespace matcher
