#include "umg.hpp"
namespace astro {
void HUD::destroy() {
  if (e.valid(widget))
    e.call(widget, "RemoveFromParent");
  discard();
}
void HUD::render(Object gi, const HUDView &v) {
  if (!e.valid(gi))
    throw std::runtime_error("HUD requires a valid game instance");
  if (!e.valid(widget) || !e.call(widget, "IsInViewport").boolean()) {
    if (!v.show)
      return;
    destroy();
    UMG ui(e);
    ui.prefix = "AstroConsoleHUD_" + std::to_string(++serial) + "_";
    ui.tree = gi;
    widget = ui.make("UserWidget", "Root");
    ui.tree = widget;
    auto tree = ui.make("WidgetTree", "Tree");
    e.set(widget, "WidgetTree", tree);
    ui.tree = tree;
    auto canvas = ui.make("CanvasPanel", "Canvas");
    e.set(tree, "RootWidget", canvas);
    auto border = ui.make("Border", "Panel");
    e.call(border, "SetBrushColor", {color(.93, .97, 1, .86)});
    e.call(border, "SetPadding", {padding(12, 8, 14, 9)});
    auto column = ui.make("VerticalBox", "Column");
    ui.content(border, column);
    title = ui.text("", 16, "Status");
    ui.add(column, title);
    auto slot = ui.add(canvas, border, "Canvas");
    e.call(slot, "SetAutoSize", {true});
    e.call(widget, "SetAnchorsInViewport",
           {record({{"Minimum", xy(0, 0)}, {"Maximum", xy(0, 0)}})});
    e.call(widget, "SetAlignmentInViewport", {xy(0, 0)});
    e.call(widget, "SetPositionInViewport", {xy(24, 26), false});
    e.call(widget, "SetDesiredSizeInViewport", {xy(430, 48)});
    e.call(widget, "AddToViewport", {80});
    e.call(widget, "SetVisibility", {3});
  }
  if (text != v.text) {
    e.call(title, "SetText", {v.text});
    text = v.text;
  }
  auto next = v.error ? "error" : v.enabled ? "on" : "off";
  if (tone != next) {
    auto c = v.error     ? color(.6, .12, .07)
             : v.enabled ? color(0, .26, .42)
                         : color(.18, .22, .26);
    e.call(title, "SetColorAndOpacity",
           {record({{"SpecifiedColor", c}, {"ColorUseRule", 0}})});
    tone = next;
  }
  if (visible != v.show) {
    e.call(widget, "SetVisibility", {v.show ? 3 : 2});
    visible = v.show;
  }
  if (opacity != v.opacity) {
    e.call(widget, "SetRenderOpacity", {v.opacity});
    opacity = v.opacity;
  }
}
} // namespace astro
