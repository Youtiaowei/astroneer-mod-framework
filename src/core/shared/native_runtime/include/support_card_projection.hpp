#pragma once
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <optional>

namespace astro {

struct HitResult {
  double u, v;
  const char *face; // "front" or "back"
};

// CPU counterpart of the native UI shader's perspective ray/card intersection.
// Locked to the approved 2x3 card, 32-degree lens, 1.17 fit and width*0.03
// corners. Identical algorithm to
// reference/runtime/support_card_projection.lua.
inline std::optional<HitResult> card_hit(double x, double y, double pitch,
                                         double yaw, double angle,
                                         double aspect = 2.0 / 3.0) {
  const auto finite = [](double v) { return std::isfinite(v); };
  if (!(finite(x) && finite(y) && finite(pitch) && finite(yaw) &&
        finite(angle) && finite(aspect)))
    return std::nullopt;
  if (aspect <= 0 || x < 0 || x > 1 || y < 0 || y > 1)
    return std::nullopt;

  const double lens = std::tan(16.0 * M_PI / 180.0);
  const double camera = 0.5 * std::max(3.0, 2.0 / aspect) / lens * 1.17;
  const double sp = std::sin(pitch), cp = std::cos(pitch);
  const double sy = std::sin(yaw + angle), cy = std::cos(yaw + angle);

  // Inverse of the original viewer's Euler XYZ rotation: Ry^-1 * Rx^-1
  const double ox = -sy * cp * camera;
  const double oy = sp * camera;
  const double oz = cy * cp * camera;

  const double vx = (2 * x - 1) * aspect * lens;
  const double vy = (1 - 2 * y) * lens;

  const double dy = cp * vy - sp;
  const double z = -sp * vy - cp;
  const double dx = cy * vx - sy * z;
  const double dz = sy * vx + cy * z;

  if (std::abs(dz) < 1e-7)
    return std::nullopt;

  const bool front = oz >= 0;
  const double t = ((front ? 0.025 : -0.025) - oz) / dz;
  if (t < 0)
    return std::nullopt;

  const double hx = ox + t * dx;
  const double hy = oy + t * dy;
  const double qx = std::max(std::abs(hx) - 1 + 0.06, 0.0);
  const double qy = std::max(std::abs(hy) - 1.5 + 0.06, 0.0);
  if (std::sqrt(qx * qx + qy * qy) > 0.06)
    return std::nullopt;

  return HitResult{0.5 + (front ? 1.0 : -1.0) * hx / 2.0, 0.5 - hy / 3.0,
                   front ? "front" : "back"};
}

} // namespace astro
