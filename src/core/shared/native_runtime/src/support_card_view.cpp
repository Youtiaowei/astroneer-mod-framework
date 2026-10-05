#include "support_card_projection.hpp"
#include "umg.hpp"
#include <iomanip>
#include <sstream>
namespace astro {
static CardConfig frozen() {
  auto &g = card_config::card_geometry();
  CardConfig c{{g.size[0], g.size[1]}, {}};
  for (auto &q : g.entries)
    c.entries.push_back({q.id, {q.rect[0], q.rect[1], q.rect[2], q.rect[3]}});
  return c;
}
CardView::CardView(Engine &engine, UMG &builder, Object g, Object m, Object p,
                   Object c)
    : e(engine), ui(builder), gi(g), menu(m), parent(p), canvas(c),
      state(frozen()) {}
void CardView::stop(bool hide) {
  shown = false;
  state.visible = false;
  state.x = state.y = 0;
  last_time = -1;
  if (hide && !closed && e.valid(menu) &&
      e.call(menu, "IsInViewport").boolean() && e.valid(box))
    e.call(box, "SetVisibility", {1});
  box_visible = false;
}
void CardView::restore() {
  if (e.valid(menu)) {
    if (e.valid(box))
      e.call(box, "RemoveFromParent");
    if (e.valid(spacer))
      e.call(spacer, "RemoveFromParent");
    for (auto &[w, visibility] : originals)
      if (e.valid(w))
        e.call(w, "SetVisibility", {visibility});
  }
  present = false;
}
bool CardView::place() {
  auto r = rectangle ? rectangle(width, height) : std::nullopt;
  if (!r || r->width <= 0 || r->height <= 0)
    return false;
  std::ostringstream key;
  key << std::setprecision(17) << r->x << ':' << r->y << ':' << r->width << ':'
      << r->height;
  if (key.str() != rect_key) {
    e.call(canvas_slot, "SetPosition", {xy(r->x, r->y)});
    e.call(canvas_slot, "SetSize", {xy(r->width, r->height)});
    rect = r;
    rect_key = key.str();
    dirty = true;
  }
  if (shown && !box_visible) {
    e.call(box, "SetVisibility", {0});
    box_visible = true;
  }
  return true;
}
void CardView::attach() {
  if (!e.valid(menu) || !e.call(menu, "IsInViewport").boolean())
    throw std::runtime_error("Card menu is no longer live");
  auto system = ui.resource("/Script/Engine.Default__KismetSystemLibrary");
  auto soft = e.call(system, "MakeSoftObjectPath", {card_config::MATERIAL});
  auto reference = e.call(system, "Conv_SoftObjPathToSoftObjRef", {soft});
  auto base = e.call(system, "LoadAsset_Blocking", {reference}).object();
  if (!e.valid(base) || e.get(base, "MaterialDomain").integer() != 5)
    throw std::runtime_error(
        "Cooked card UI material is missing or incompatible");
  image = ui.make("Border", "SupportCardImage");
  e.call(image, "SetPadding", {padding(0, 0, 0, 0)});
  e.call(image, "SetBrushColor", {color(1, 1, 1)});
  e.call(image, "SetBrushFromMaterial", {base});
  material = e.call(image, "GetDynamicMaterial").object();
  if (!e.valid(material))
    throw std::runtime_error("Cannot create private card material");
  retainer = ui.make("RetainerBox", "SupportCardRetainer");
  e.set(retainer, "RenderOnPhase", false);
  e.set(retainer, "RenderOnInvalidation", false);
  e.call(retainer, "SetRetainRendering", {true});
  ui.content(retainer, image);
  check = ui.flat("SupportCardInput", retainer);
  e.set(check, "IsFocusable", false);
  e.call(check, "SetIsChecked", {false});
  box = ui.make("SizeBox", "SupportCardSize");
  e.call(box, "SetWidthOverride", {width});
  e.call(box, "SetHeightOverride", {height});
  ui.content(box, check);
  e.call(box, "SetVisibility", {1});
  spacer = ui.make("SizeBox", "SupportCardSpacer");
  e.call(spacer, "SetWidthOverride", {width});
  e.call(spacer, "SetHeightOverride", {height});
  int n = e.call(parent, "GetChildrenCount").integer();
  if (n < 0 || n > 64)
    throw std::runtime_error("Card child count exceeds bound");
  for (int i = 0; i < n; ++i) {
    auto child = e.call(parent, "GetChildAt", {i}).object();
    originals.emplace_back(child, e.call(child, "GetVisibility").integer());
  }
  for (auto [w, v] : originals)
    e.call(w, "SetVisibility", {1});
  e.call(ui.add(parent, spacer), "SetHorizontalAlignment", {2});
  canvas_slot = ui.add(canvas, box, "Canvas");
  e.call(canvas_slot, "SetAutoSize", {false});
  e.call(canvas_slot, "SetZOrder", {21});
  place();
  present = true;
  dirty = true;
}
void CardView::draw() {
  if (!rect)
    return;
  int scan = state.scanId == "qq" ? 1 : state.scanId == "afdian" ? 2 : 0;
  std::ostringstream key;
  key << std::setprecision(17) << state.tiltY << ':' << state.tiltX << ':'
      << state.angle << ':' << scan << ':' << rect->width << ':'
      << rect->height;
  if (!dirty && key.str() == draw_key)
    return;
  e.call(material, "SetVectorParameterValue",
         {"Pose", color(state.tiltY, state.tiltX, state.angle, 0)});
  auto r = state.scanRect;
  e.call(material, "SetVectorParameterValue",
         {"ScanRect", color(r[0], r[1], r[2], r[3])});
  e.call(material, "SetScalarParameterValue", {"Scan", scan});
  e.call(material, "SetScalarParameterValue",
         {"Aspect", rect->width / rect->height});
  e.call(retainer, "RequestRender");
  ++updates;
  draw_key = key.str();
  dirty = false;
}
void CardView::tick() {
  if (!shown || closed)
    return;
  try {
    if (!e.valid(menu) || !e.call(menu, "IsInViewport").boolean() || !active ||
        !active()) {
      stop();
      return;
    }
    if (!place())
      return;
    std::optional<HitResult> hit;
    double x = 0, y = 0;
    pointer = {};
    pointer.hovered = e.call(check, "IsHovered").boolean();
    if (pointer.hovered) {
      auto library = ui.resource("/Script/UMG.Default__WidgetLayoutLibrary");
      auto point = e.call(library, "GetMousePositionOnViewport", {gi});
      double dpi = e.call(library, "GetViewportScale", {gi}).number();
      pointer.sampled = true;
      pointer.raw_x = point.at("X").number();
      pointer.raw_y = point.at("Y").number();
      pointer.dpi = dpi;
      if (dpi > 0) {
        // GetMousePositionOnViewport already returns viewport-widget local
        // coordinates, matching this CanvasPanel's layout rect. Dividing by
        // viewport DPI again rejects valid windowed clicks (DPI != 1).
        x = (point.at("X").number() - rect->x) / rect->width;
        y = (point.at("Y").number() - rect->y) / rect->height;
        hit = card_hit(x, y, state.tiltX, state.tiltY, state.angle,
                       rect->width / rect->height);
      }
    }
    if (hit)
      state.pointer(2 * x - 1, 1 - 2 * y);
    else
      state.pointer(0, 0);
    pointer.x = x;
    pointer.y = y;
    pointer.hit = hit.has_value();
    pointer.checked = e.call(check, "IsChecked").boolean();
    if (pointer.checked) {
      e.call(check, "SetIsChecked", {false});
      if (hit)
        state.click(hit->u, hit->v);
    }
    auto now = e.call(ui.resource("/Script/Engine.Default__GameplayStatics"),
                      "GetRealTimeSeconds", {gi})
                   .number();
    state.step(last_time < 0 ? .016 : std::max(0.0, now - last_time));
    last_time = now;
    draw();
  } catch (const std::exception &ex) {
    error = ex.what();
    stop();
  }
}
bool CardView::sync(bool show, double w, double h) {
  if (closed)
    return false;
  if (!show) {
    stop();
    return present;
  }
  if (!error.empty())
    return false;
  if (width != w || height != h) {
    width = w;
    height = h;
    dirty = true;
    if (present)
      for (auto o : {box, spacer}) {
        e.call(o, "SetWidthOverride", {w});
        e.call(o, "SetHeightOverride", {h});
      }
  }
  if (!present) {
    try {
      attach();
    } catch (const std::exception &ex) {
      error = ex.what();
      restore();
      return false;
    }
  }
  if (!shown)
    e.call(check, "SetIsChecked", {false});
  shown = true;
  state.visible = true;
  place();
  return true;
}
void CardView::discard() {
  shown = false;
  closed = true;
  present = false;
  state.open = false;
  state.visible = false;
  image = {};
  material = {};
  retainer = {};
  check = {};
  box = {};
  spacer = {};
  canvas_slot = {};
  originals.clear();
  rectangle = {};
  active = {};
}
void CardView::close() {
  if (closed)
    return;
  stop();
  restore();
  discard();
}
Value CardView::snapshot() const {
  return record({{"created", present},
                 {"active", shown},
                 {"material_updates", updates},
                 {"error", error},
                 {"width", width},
                 {"height", height},
                 {"revision", card_config::REVISION},
                 {"scan_id", state.scanId},
                 {"back", state.back},
                 {"angle", state.angle},
                 {"tilt_x", state.tiltX},
                 {"tilt_y", state.tiltY},
                 {"pointer", record({{"hovered", pointer.hovered},
                                     {"sampled", pointer.sampled},
                                     {"checked", pointer.checked},
                                     {"hit", pointer.hit},
                                     {"raw_x", pointer.raw_x},
                                     {"raw_y", pointer.raw_y},
                                     {"dpi", pointer.dpi},
                                     {"x", pointer.x},
                                     {"y", pointer.y}})},
                 {"rect", rect ? rect_value(*rect) : Value{}}});
}
} // namespace astro
