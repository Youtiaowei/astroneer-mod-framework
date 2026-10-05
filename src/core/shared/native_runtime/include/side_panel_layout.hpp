#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace astro {

struct LayoutInput {
  double width;
  double height;
  double left_width;
  double left_height;
  double right_width;
  double right_height;
  double content_height = 540;
  double max_height = 540;
};

struct LayoutRect {
  double x, y, width, height;
};

struct LayoutResult {
  const char *mode; // "beside" or "stacked"
  double native_scale;
  LayoutRect left;
  LayoutRect right;
  bool scroll_required;
  LayoutRect mod;
};

// Pure layout in DPI-independent viewport units, never UObject or screen
// pixels. Identical algorithm to reference/runtime/side_panel_layout.lua.
inline LayoutResult solve_layout(const LayoutInput &v) {
  if (!std::isfinite(v.content_height) || v.content_height <= 0)
    throw std::invalid_argument("Invalid content height");
  const double desired = v.content_height;
  const double margin = 47.0, gap = 24.0;
  const double min_height = std::min(160.0, desired);
  const double total = v.left_width + v.right_width;
  double width =
      std::max(300.0, std::min(360.0, v.width - 2 * margin - total - 2 * gap));
  double scale =
      std::min(1.0, (v.width - 2 * margin - 2 * gap - width) / total);
  const char *mode = "beside";
  if (scale < 0.8) {
    mode = "stacked";
    const double vertical_scale = (v.height - 2 * margin - gap - min_height) /
                                  std::max(v.left_height, v.right_height);
    scale =
        std::min({1.0, (v.width - 2 * margin - gap) / total, vertical_scale});
    width = std::min(420.0, v.width - 2 * margin);
  }
  if (!std::isfinite(scale) || scale < .8)
    throw std::runtime_error(
        "Unsupported viewport: native menus would become illegible");
  LayoutRect left{margin, margin, v.left_width * scale, v.left_height * scale};
  LayoutRect right{v.width - margin - v.right_width * scale, margin,
                   v.right_width * scale, v.right_height * scale};
  const double x = (mode[0] == 'b') ? left.x + left.width + gap : margin;
  const double y = (mode[0] == 'b')
                       ? margin
                       : margin + std::max(left.height, right.height) + gap;
  if (!std::isfinite(v.max_height) || v.max_height <= 0)
    throw std::invalid_argument("Invalid panel height limit");
  const double limit = v.max_height;
  const double height = std::min({desired, limit, v.height - margin - y});
  if (height < min_height - 1e-6)
    throw std::runtime_error("Unsupported viewport: not enough panel height");
  return LayoutResult{mode,
                      scale,
                      left,
                      right,
                      desired > height,
                      LayoutRect{x, y, width, height}};
}

} // namespace astro
