#pragma once

// Click-through layered overlay (port of live_overlay.LiveMirrorOverlay):
// WS_EX_LAYERED | TRANSPARENT | TOOLWINDOW | NOACTIVATE + colorkey #000000 +
// WM_NCHITTEST -> HTTRANSPARENT. Missing any of these either eats input or
// flashes black (handoff pitfall list).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

namespace overlay {

// Scale of overlay text metrics vs the 1080p reference (28px Consolas,
// 16px/char, 30px line). Uncapped and linear in client height (= 1 over the
// capture downscale factor), so text stays proportional to entity geometry
// on any client size.
double FontScale(int client_height);

struct Label {
  double x = 0, y = 0;       // client-space pixels
  double radius = 0;
  double angle_deg = -1;     // < 0: no angle arc
  std::string text;
  COLORREF color = RGB(128, 255, 128);
  // explicit text anchor (top-left). When unset the text is drawn centred
  // above the entity, the legacy placement.
  bool text_at_pos = false;
  double text_x = 0, text_y = 0;
};

class MirrorOverlay {
 public:
  ~MirrorOverlay();
  bool Create();
  // Move/resize the overlay to sit exactly over the game client rectangle.
  void Position(const RECT& screenRect);
  void Render(const std::vector<Label>& labels, const std::string& hud);
  void Clear();
  void Hide();
  void Show();
  void Destroy();

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam);
  void Paint();

  HWND hwnd_ = nullptr;
  std::vector<Label> labels_;
  std::string hud_;
  int width_ = 0, height_ = 0;
  double font_scale_ = 1.0;
};

}  // namespace overlay
