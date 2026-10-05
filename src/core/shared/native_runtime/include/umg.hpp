#pragma once
#include "controls.hpp"
#include "engine.hpp"
#include "side_panel_layout.hpp"
#include "support_card_config.hpp"
#include "support_card_state.hpp"
namespace astro {
inline Value xy(double x, double y) { return record({{"X", x}, {"Y", y}}); }
inline Value color(double r, double g, double b, double a = 1) {
  return record({{"R", r}, {"G", g}, {"B", b}, {"A", a}});
}
inline Value padding(double left, double top, double right, double bottom) {
  return record(
      {{"Left", left}, {"Top", top}, {"Right", right}, {"Bottom", bottom}});
}
inline Value rect_value(const LayoutRect &r) {
  return record(
      {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}});
}
inline Value layout_value(const LayoutResult &r) {
  return record({{"mode", r.mode},
                 {"native_scale", r.native_scale},
                 {"left", rect_value(r.left)},
                 {"right", rect_value(r.right)},
                 {"mod", rect_value(r.mod)},
                 {"scroll_required", r.scroll_required}});
}
struct UMG {
  Engine &e;
  Object tree, font;
  std::string prefix;
  explicit UMG(Engine &engine) : e(engine) {}
  Object resource(const std::string &path) {
    auto o = e.find(path);
    if (!e.valid(o))
      throw std::runtime_error("UI resource unavailable: " + path);
    return o;
  }
  Object make(const std::string &type, const std::string &name) {
    auto o = e.construct("/Script/UMG." + type, tree, prefix + name);
    if (!e.valid(o))
      throw std::runtime_error("Cannot construct UI " + type);
    return o;
  }
  Object text(const std::string &value, int size, const std::string &name) {
    if (!e.valid(font))
      font = resource(
          "/Game/UI/fonts/NDAstroneer-Regular_Font.NDAstroneer-Regular_Font");
    auto w = make("TextBlock", name);
    e.set(w, "Font.FontObject", font);
    e.set(w, "Font.Size", size);
    e.call(w, "SetAutoWrapText", {true});
    e.call(w, "SetText", {value});
    tone(w, color(.10, .20, .24));
    return w;
  }
  void tone(Object w, const Value &c) {
    e.call(w, "SetColorAndOpacity",
           {record({{"SpecifiedColor", c}, {"ColorUseRule", 0}})});
  }
  Object add(Object parent, Object child,
             const std::string &kind = "VerticalBox") {
    return e.call(parent, "AddChildTo" + kind, {child}).object();
  }
  Object content(Object parent, Object child) {
    return e.call(parent, "SetContent", {child}).object();
  }
  void fill(Object slot) {
    e.call(slot, "SetSize", {record({{"Value", 1}, {"SizeRule", 1}})});
  }
  Object fixed(Object child, double height, const std::string &name) {
    auto box = make("SizeBox", name);
    e.call(box, "SetHeightOverride", {height});
    content(box, child);
    return box;
  }
  Object flat(const std::string &name, Object child) {
    auto w = make("CheckBox", name);
    e.set(w, "IsFocusable", true);
    e.set(w, "HorizontalAlignment", 0);
    e.set(w, "WidgetStyle.CheckBoxType",
          e.enum_value("/Script/SlateCore.ESlateCheckBoxType", "ToggleButton"));
    e.set(w, "WidgetStyle.Padding", padding(0, 0, 0, 0));
    int no_draw =
        e.enum_value("/Script/SlateCore.ESlateBrushDrawType", "NoDrawType");
    for (auto state : {"Unchecked", "Checked", "Undetermined"})
      for (auto phase : {"Image", "HoveredImage", "PressedImage"})
        e.set(w, "WidgetStyle." + std::string(state) + phase + ".DrawAs",
              no_draw);
    content(w, child);
    return w;
  }
};
struct HUD {
  Engine &e;
  Object widget, title;
  int serial = 0;
  bool visible = false;
  double opacity = 0;
  std::string text, tone;
  explicit HUD(Engine &engine) : e(engine) {}
  void render(Object gi, const HUDView &);
  void destroy();
  void discard() {
    widget = {};
    title = {};
    visible = false;
    opacity = 0;
    text.clear();
    tone.clear();
  }
  Value snapshot() {
    return record({{"created", e.valid(widget)},
                   {"visible", visible},
                   {"opacity", opacity},
                   {"text", text},
                   {"hit_test_invisible", true},
                   {"serial", serial}});
  }
};
struct CardView {
  Engine &e;
  UMG &ui;
  Object gi, menu, parent, canvas;
  Object image, material, retainer, check, box, spacer, canvas_slot;
  std::vector<std::pair<Object, int>> originals;
  CardState state;
  bool present = false, shown = false, closed = false, dirty = true,
       box_visible = false;
  double width = 320, height = 480, last_time = -1;
  int updates = 0;
  // Bounded, read-only last sample. No per-tick logging or extra engine calls.
  struct PointerSample {
    bool hovered = false, sampled = false, checked = false, hit = false;
    double raw_x = 0, raw_y = 0, dpi = 0, x = 0, y = 0;
  } pointer;
  std::string error, draw_key, rect_key;
  std::optional<LayoutRect> rect;
  std::function<std::optional<LayoutRect>(double, double)> rectangle;
  std::function<bool()> active;
  CardView(Engine &, UMG &, Object gi, Object menu, Object parent,
           Object canvas);
  void stop(bool hide = true);
  void restore();
  void attach();
  bool place();
  void draw();
  void tick();
  bool sync(bool shown, double width = 320, double height = 480);
  void close();
  void discard();
  Value snapshot() const;
};
} // namespace astro
