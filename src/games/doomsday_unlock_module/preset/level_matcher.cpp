#include "preset/level_matcher.h"
#include "doomsday_embedded.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace matcher {

using gtajson::Json;

namespace {

using Node = std::pair<double, double>;

std::pair<double, double> BoardSteps(const int board[4]) {
  return {kStep * board[2] / kRefW, kStep * board[3] / kRefH};
}

std::pair<double, double> BoardFirst(const int board[4]) {
  return {kFirstX * board[2] / kRefW, kFirstY * board[3] / kRefH};
}

std::string NodeKey(const Node& n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f,%.3f", n.first, n.second);
  return buf;
}

}  // namespace

std::pair<double, double> GridCoords(double cx, double cy, const int board[4]) {
  const auto [sx, sy] = BoardSteps(board);
  const auto [fx, fy] = BoardFirst(board);
  return {(cx - board[0] - fx) / sx, (cy - board[1] - fy) / sy};
}

std::pair<double, double> NodeCenter(const Node& node, const int board[4]) {
  const auto [sx, sy] = BoardSteps(board);
  const auto [fx, fy] = BoardFirst(board);
  return {board[0] + fx + node.first * sx, board[1] + fy + node.second * sy};
}

bool CenterToNode(double cx, double cy, const int board[4], double quantum,
                  double tolerance, Node* node) {
  const auto [sx, sy] = BoardSteps(board);
  const auto [fx, fy] = BoardFirst(board);
  const double gx = std::rint((cx - board[0] - fx) / sx / quantum) * quantum;
  const double gy = std::rint((cy - board[1] - fy) / sy / quantum) * quantum;
  const double dx = std::abs((cx - board[0] - fx) - gx * sx) / sx;
  const double dy = std::abs((cy - board[1] - fy) - gy * sy) / sy;
  if (dx > tolerance || dy > tolerance) return false;
  // normalize -0.0 and float dust so nodes compare equal (Python round(g*2)/2)
  const double nx = std::rint(gx * 2) / 2;
  const double ny = std::rint(gy * 2) / 2;
  node->first = nx == static_cast<long long>(nx) ? static_cast<double>(static_cast<long long>(nx)) : nx;
  node->second = ny == static_cast<long long>(ny) ? static_cast<double>(static_cast<long long>(ny)) : ny;
  return true;
}

std::map<int, Signature> LoadSignatures() {
  std::map<int, Signature> signatures;
  std::vector<std::string> paths;
  for (int level = 1; level <= 14; ++level) {
    paths.push_back(std::string("maps/level") + (level < 10 ? "0" : "") +
                    std::to_string(level) + "/layout.json");
  }
  for (const auto& lay : paths) {
    const auto embedded = doomsday_embedded::Find(lay);
    if (embedded.empty()) continue;
    const std::string raw(embedded.data(), embedded.size());
    gtajson::Json doc;
    std::string error;
    if (!gtajson::Json::Parse(raw, &doc, &error)) continue;
    Signature sig;
    sig.level = static_cast<int>(doc.Find("level")->AsNumber());
    const Json& board = *doc.Find("board");
    int b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<int>(board.items[i].AsNumber());
    for (const auto& m : doc.Find("mirrors")->items) {
      Node node;
      const Json& center = *m.Find("center");
      const bool onGrid = CenterToNode(center.items[0].AsNumber(),
                                       center.items[1].AsNumber(), b, 1.0,
                                       kSnapTolerance, &node);
      Signature::NodeInfo info;
      info.id = m.Find("id")->AsString();
      const Json* hidden = m.Find("hidden");
      info.hidden = hidden != nullptr && hidden->AsBool();
      if (onGrid) {
        sig.mirrors[node] = info;
      } else {
        const auto [gx, gy] = GridCoords(center.items[0].AsNumber(),
                                         center.items[1].AsNumber(), b);
        sig.off_grid.push_back({{gx, gy}, info});
      }
    }
    for (const auto& t : doc.Find("targets")->items) {
      Node node;
      const Json& center = *t.Find("center");
      if (!CenterToNode(center.items[0].AsNumber(), center.items[1].AsNumber(),
                        b, 0.5, 0.25, &node)) {
        throw LevelMatchError(lay + ": target " + t.Find("id")->AsString() +
                              " is not on the node grid");
      }
      sig.targets[node] = t.Find("id")->AsString();
    }
    signatures[sig.level] = std::move(sig);
  }
  return signatures;
}

namespace {

struct PairEntry {
  double dist;
  int di;
  int ai;
  bool operator<(const PairEntry& other) const {
    if (dist != other.dist) return dist < other.dist;
    if (di != other.di) return di < other.di;
    return ai < other.ai;
  }
};

// One-to-one nearest binding of detections to authored entities.
std::map<int, int> GreedyBind(
    const std::vector<Node>& detections,
    const std::vector<Node>& authored, double tolerance) {
  std::vector<PairEntry> pairs;
  for (int di = 0; di < static_cast<int>(detections.size()); ++di) {
    for (int ai = 0; ai < static_cast<int>(authored.size()); ++ai) {
      const double dist = std::hypot(detections[di].first - authored[ai].first,
                                     detections[di].second - authored[ai].second);
      if (dist <= tolerance) pairs.push_back({dist, di, ai});
    }
  }
  std::sort(pairs.begin(), pairs.end());
  std::set<int> usedD, usedA;
  std::map<int, int> binding;
  for (const auto& [dist, di, ai] : pairs) {
    if (usedD.count(di) || usedA.count(ai)) continue;
    usedD.insert(di);
    usedA.insert(ai);
    binding[di] = ai;
  }
  return binding;
}

}  // namespace

MatchResult MatchLevel(const std::vector<Node>& detected_mirrors,
                       const std::vector<Node>& detected_targets,
                       const int board[4], const std::map<int, Signature>* signatures_in) {
  const std::map<int, Signature> default_sigs = signatures_in
      ? std::map<int, Signature>()
      : LoadSignatures();
  const std::map<int, Signature>& signatures =
      signatures_in ? *signatures_in : default_sigs;
  std::vector<Node> detM, detT;
  for (const auto& [cx, cy] : detected_mirrors) detM.push_back(GridCoords(cx, cy, board));
  for (const auto& [cx, cy] : detected_targets) detT.push_back(GridCoords(cx, cy, board));
  if (detM.empty()) throw LevelMatchError("no mirror detections to match");

  struct Candidate {
    int level;
    const Signature* sig;
    std::map<int, int> mirror_bind;
    std::map<int, int> target_bind;
    int covered;
  };
  std::vector<Candidate> candidates;
  for (const auto& [level, sig] : signatures) {
    std::vector<Node> authoredM;
    std::vector<Signature::NodeInfo> authoredMInfo;
    for (const auto& [node, info] : sig.mirrors) {
      authoredM.push_back(node);
      authoredMInfo.push_back(info);
    }
    for (const auto& [pos, info] : sig.off_grid) {
      authoredM.push_back(pos);
      authoredMInfo.push_back(info);
    }
    const std::map<int, int> mirrorBind = GreedyBind(detM, authoredM, kMirrorBindTolerance);
    if (mirrorBind.size() < detM.size()) continue;
    std::vector<Node> authoredT;
    std::vector<std::string> authoredTIds;
    for (const auto& [node, tid] : sig.targets) {
      authoredT.push_back(node);
      authoredTIds.push_back(tid);
    }
    const std::map<int, int> targetBind = GreedyBind(detT, authoredT, kTargetBindTolerance);
    if (targetBind.size() < detT.size()) continue;
    std::set<Node> visibleM;
    for (const auto& [node, info] : sig.mirrors) {
      if (!info.hidden) visibleM.insert(node);
    }
    for (const auto& [pos, info] : sig.off_grid) {
      if (!info.hidden) visibleM.insert(pos);
    }
    int covered = 0;
    for (const auto& [di, ai] : mirrorBind) {
      if (visibleM.count(authoredM[ai])) ++covered;
    }
    candidates.push_back({level, &sig, mirrorBind, targetBind, covered});
  }
  if (candidates.empty()) throw LevelMatchError("detections match no known level");
  const Candidate* best = &candidates.front();
  for (const auto& c : candidates) {
    if (c.covered > best->covered) best = &c;
  }
  std::vector<const Candidate*> ties;
  for (const auto& c : candidates) {
    if (c.covered == best->covered) ties.push_back(&c);
  }
  if (ties.size() > 1) {
    std::vector<int> levels;
    for (const auto* t : ties) levels.push_back(t->level);
    std::sort(levels.begin(), levels.end());
    std::string names;
    for (int lv : levels) {
      if (!names.empty()) names += ", ";
      char buf[8];
      std::snprintf(buf, sizeof(buf), "L%02d", lv);
      names += buf;
    }
    throw LevelMatchError("ambiguous level match (" + names + ")");
  }

  MatchResult out;
  out.level = best->level;
  std::vector<Node> authoredM;
  std::vector<std::string> authoredMIds;
  std::vector<bool> authoredMVisible;
  for (const auto& [node, info] : best->sig->mirrors) {
    authoredM.push_back(node);
    authoredMIds.push_back(info.id);
    authoredMVisible.push_back(!info.hidden);
  }
  for (const auto& [pos, info] : best->sig->off_grid) {
    authoredM.push_back(pos);
    authoredMIds.push_back(info.id);
    authoredMVisible.push_back(!info.hidden);
  }
  std::set<int> boundAuthored;
  for (const auto& [di, ai] : best->mirror_bind) {
    out.mirrors[di] = authoredMIds[ai];
    boundAuthored.insert(ai);
  }
  // visible authored mirrors no detection bound to -> transient miss signal
  for (std::size_t ai = 0; ai < authoredMIds.size(); ++ai) {
    if (authoredMVisible[ai] &&
        !boundAuthored.count(static_cast<int>(ai))) {
      out.missing_mirrors.push_back(authoredMIds[ai]);
    }
  }
  std::vector<Node> authoredT;
  std::vector<std::string> authoredTIds;
  for (const auto& [node, tid] : best->sig->targets) {
    authoredT.push_back(node);
    authoredTIds.push_back(tid);
  }
  for (const auto& [di, ai] : best->target_bind) {
    out.targets[di] = authoredTIds[ai];
  }
  std::set<Node> boundTargets;
  for (const auto& [di, ai] : best->target_bind) boundTargets.insert(authoredT[ai]);
  // Python: sorted(sig["targets"].items()) — sorted by node (x, then y)
  std::vector<std::pair<Node, std::string>> sortedTargets(best->sig->targets.begin(),
                                                          best->sig->targets.end());
  std::sort(sortedTargets.begin(), sortedTargets.end());
  for (const auto& [node, tid] : sortedTargets) {
    if (boundTargets.count(node)) continue;
    const auto [cx, cy] = NodeCenter(node, board);
    char bx[32], by[32];
    std::snprintf(bx, sizeof(bx), "%.2f", cx);
    std::snprintf(by, sizeof(by), "%.2f", cy);
    MatchResult::MissingTarget missing;
    missing.id = tid;
    missing.center_x = std::atof(bx);
    missing.center_y = std::atof(by);
    out.missing_targets.push_back(missing);
  }
  return out;
}

}  // namespace matcher
