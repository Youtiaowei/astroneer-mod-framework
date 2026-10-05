#pragma once
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <array>
#include <string>
#include <vector>

namespace astro {

struct QREntry {
  const char *id;
  double rect[4]; // x, y, w, h in pixel coords
};

struct CardConfig {
  double size[2]; // width, height in pixels
  std::vector<QREntry> entries;
};

struct CardStepResult {
  double flip;
  double tiltX;
  double tiltY;
  const char *face; // "turning", "back", "front"
  bool needsFrame;
};

// Pure interaction state, ported from the approved card controller.
// No Unreal calls. Identical algorithm to
// reference/runtime/support_card_state.lua.
class CardState {
public:
  bool open = true;
  bool visible = true;
  bool reducedMotion = false;
  bool back = false;
  double x = 0, y = 0;
  double tiltX = 0, tiltY = 0;
  double angle = 0;
  std::string scanId;
  std::array<double, 4> scanRect;
  CardConfig config;

  explicit CardState(const CardConfig &card = {{1024, 1536}, {}})
      : scanRect{0, 0, 0, 0}, config(card) {
    const double w = card.size[0], h = card.size[1];
    const double scan_w = std::min(0.82, 0.56 * h / w);
    const double scan_h = scan_w * w / h;
    scanRect = {(1 - scan_w) / 2, 1 - (0.18 + (0.56 - scan_h) / 2) - scan_h,
                scan_w, scan_h};
  }

  static double clamp_val(double value, double low, double high) {
    if (!std::isfinite(value))
      value = 0;
    return std::min(high, std::max(low, value));
  }

  void pointer(double px, double py) {
    if (!open || !visible || back)
      return;
    x = clamp_val(px, -1, 1);
    y = clamp_val(py, -1, 1);
  }

  bool flip() {
    if (!open)
      return false;
    back = !back;
    x = 0;
    y = 0;
    return true;
  }

  std::string click(double cx, double cy) {
    return click_with_size(cx, cy, config);
  }

  // Click with card size for QR hit testing
  std::string click_with_size(double cx, double cy, const CardConfig &card) {
    if (!open || !visible || !std::isfinite(cx) || !std::isfinite(cy) ||
        cx < 0 || cx > 1 || cy < 0 || cy > 1)
      return "";
    if (!scanId.empty()) {
      if (inside(cx, cy, scanRect)) {
        scanId.clear();
        return "exit_scan";
      }
      return "";
    }
    if (back && angle == M_PI) {
      for (const auto &entry : card.entries) {
        if (inside(
                cx * card.size[0], cy * card.size[1],
                {entry.rect[0], entry.rect[1], entry.rect[2], entry.rect[3]})) {
          scanId = entry.id;
          return "scan";
        }
      }
    }
    flip();
    return "flip";
  }

  CardStepResult step(double seconds) {
    const double dt = clamp_val(seconds, 0, 0.05);
    const double target = back ? M_PI : 0.0;
    const double tx = (back || reducedMotion) ? 0.0 : -y * 0.18;
    const double ty = (back || reducedMotion) ? 0.0 : x * 0.25;
    const bool allowed = open && visible;
    if (allowed) {
      if (reducedMotion) {
        angle = target;
        tiltX = tx;
        tiltY = ty;
      } else {
        angle = approach(angle, target, dt, 10);
        tiltX = approach(tiltX, tx, dt, 14);
        tiltY = approach(tiltY, ty, dt, 14);
      }
    }
    const bool settled = (angle == target) && (tiltX == tx) && (tiltY == ty);
    const char *face =
        (angle != target) ? "turning" : (back ? "back" : "front");
    return CardStepResult{angle, tiltX, tiltY, face, allowed && !settled};
  }

private:
  static double approach(double value, double target, double dt, double rate) {
    double next_val = value + (target - value) * (1 - std::exp(-rate * dt));
    return std::abs(next_val - target) < 0.0001 ? target : next_val;
  }

  static bool inside(double px, double py, const std::array<double, 4> &r) {
    return px >= r[0] && px <= r[0] + r[2] && py >= r[1] && py <= r[1] + r[3];
  }

  static bool inside(double px, double py, double r[4]) {
    return px >= r[0] && px <= r[0] + r[2] && py >= r[1] && py <= r[1] + r[3];
  }
};

} // namespace astro
