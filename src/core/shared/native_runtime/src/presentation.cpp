#include "presentation.hpp"
#include "localization.hpp"
#include "umg.hpp"
#include "number_view.hpp"
#include <iomanip>
#include <sstream>
namespace astro {
struct Presentation::Impl {
  Engine &e;
  UMG ui;
  HUD hud;
  Object gi, menu, root, slot, scroll, card_note, link_status;
  int serial = 0;
  std::string registry, page = "mods", key, failed_key, error, hud_error,
                        link_status_text = "将在浏览器中打开";
  bool blocked = false, scrollbar = false, card_error_shown = false;
  std::optional<LayoutResult> layout;
  struct Original {
    Object widget;
    Value scale, pivot;
  };
  std::vector<Original> originals;
  struct Tab {
    Object check, bg, label, scroll, items;
    std::optional<bool> selected, focused;
  };
  std::map<std::string, Tab> tabs;
  struct Row {
    ControlRow metadata;
    Object check, status, rail, thumb, name, shortcut;
    bool presented = false, active = false;
    std::optional<bool> visual, focused;
    std::string message, status_text, shortcut_text;
    std::unique_ptr<NumberView> number;
  };
  std::vector<Row> rows;
  struct Link {
    LinkEntry metadata;
    Object check, bg;
    int attempts = 0;
    std::optional<bool> focused;
  };
  std::vector<Link> links;
  std::string language = "zh-CN";
  struct TextBinding { Object widget; std::string chinese; };
  std::vector<TextBinding> texts;
  struct LanguageOption {
    std::string id;
    Object check, background, label;
    std::optional<bool> selected, focused;
  };
  std::vector<LanguageOption> language_options;
  Object language_status;
  bool language_save_failed = false;
  std::string language_status_text;
  std::string tr(const std::string &value) const { return i18n::text(language, value); }
  Object translated(const std::string &value, int size, const std::string &name) {
    auto widget = ui.text(tr(value), size, name);
    texts.push_back({widget, value});
    return widget;
  }
  void set_language(const std::string &value) {
    if (language == value)
      return;
    language = value;
    // Checkboxes model mutually exclusive language choices. Consume the old
    // selection before processing another option, including initial restore.
    for (const auto &option : language_options)
      if (e.valid(option.check))
        e.call(option.check, "SetIsChecked", {option.id == language});
    for (const auto &binding : texts)
      if (e.valid(binding.widget))
        e.call(binding.widget, "SetText", {tr(binding.chinese)});
    if (e.valid(link_status))
      e.call(link_status, "SetText", {tr(link_status_text)});
    if (card_error_shown && e.valid(card_note))
      e.call(card_note, "SetText", {tr("卡片暂不可用，请重新安装 Mod 资源")});
    key.clear();
  }
  std::unique_ptr<CardView> card;
  explicit Impl(Engine &engine) : e(engine), ui(engine), hud(engine) {}
  bool modal() {
    auto popout = e.get(menu, "ActivePopoutWidget").object();
    bool visible = false;
    if (e.valid(popout)) {
      auto v = e.call(popout, "GetVisibility").integer();
      visible = v != 1 && v != 2;
    }
    return e.call(menu, "GetAnyGameMenuWarningVisible").boolean() || visible;
  }
  bool live() {
    return e.valid(menu) && e.call(menu, "IsInViewport").boolean() &&
           e.valid(root) && e.valid(e.call(root, "GetParent").object());
  }
  void clear() {
    menu = {};
    root = {};
    slot = {};
    scroll = {};
    card_note = {};
    link_status = {};
    gi = {};
    rows.clear();
    tabs.clear();
    links.clear();
    texts.clear();
    language_options.clear();
    language_status = {};
    language_save_failed = false;
    language_status_text.clear();
    originals.clear();
    layout.reset();
    card.reset();
    key.clear();
    registry.clear();
    page = "mods";
    blocked = false;
    scrollbar = false;
    card_error_shown = false;
    ui.tree = {};
    ui.font = {};
  }
  void discard() {
    if (card)
      card->discard();
    clear();
    failed_key.clear();
    error.clear();
    try {
      hud.destroy();
    } catch (const std::exception &ex) {
      // Viewport cleanup must not abort input/world invalidation in suspend().
      hud.discard();
      hud_error = ex.what();
    }
  }
  void close() {
    if (card)
      card->close();
    // Visibility ends a mount, not the lifetime of these native wrappers.
    // Restore same-epoch valid objects before dropping their saved transforms.
    if (e.valid(menu))
      for (auto &r : originals)
        if (e.valid(r.widget)) {
          e.call(r.widget, "SetRenderScale", {r.scale});
          e.call(r.widget, "SetRenderTransformPivot", {r.pivot});
        }
    if (e.valid(root))
      e.call(root, "RemoveFromParent");
    clear();
  }
  void mount(Object game, Object native_menu,
             const std::vector<ControlRow> &source, const std::string &ids) {
    ++serial;
    gi = game;
    menu = native_menu;
    registry = ids;
    auto parent = e.get(menu, "OuterMenuWrapper").object();
    if (!e.valid(parent) || !e.is_a(parent, "/Script/UMG.CanvasPanel"))
      throw std::runtime_error("Native outer canvas unavailable");
    for (auto field :
         {"MenuContentsWrapper_Left", "MenuContentsWrapper_Right",
          "CursorAwarenessBorder_Left", "CursorAwarenessBorder_Right"}) {
      auto w = e.get(menu, field).object();
      if (!e.valid(w) || e.get(w, "RenderTransform.Scale.X").number() != 1 ||
          e.get(w, "RenderTransform.Scale.Y").number() != 1)
        throw std::runtime_error(
            "Unexpected native transform; menu layout left untouched");
      originals.push_back({w, xy(1, 1),
                           xy(e.get(w, "RenderTransformPivot.X").number(),
                              e.get(w, "RenderTransformPivot.Y").number())});
    }
    ui.tree = e.get(menu, "WidgetTree").object();
    ui.prefix = "AstroModMenu_" + std::to_string(serial) + "_";
    root = ui.make("Button", "Panel");
    e.set(root, "IsFocusable", true);
    e.call(root, "SetBackgroundColor", {color(1, 1, 1, 0)});
    auto column = ui.make("VerticalBox", "Column");
    auto cs = ui.content(root, column);
    e.call(cs, "SetHorizontalAlignment", {0});
    e.call(cs, "SetVerticalAlignment", {0});
    auto nav = ui.make("HorizontalBox", "Navigation");
    for (auto [id, title] :
         std::initializer_list<std::pair<const char *, const char *>>{
             {"mods", "MOD 管理"}, {"support", "打赏作者"}}) {
      auto bg = ui.make("Border", std::string(id) + "_TabBackground");
      e.call(bg, "SetPadding", {padding(12, 8, 12, 8)});
      auto label = translated(title, 20, std::string(id) + "_TabLabel");
      e.call(label, "SetAutoWrapText", {false});
      auto ls = ui.content(bg, label);
      e.call(ls, "SetHorizontalAlignment", {2});
      e.call(ls, "SetVerticalAlignment", {2});
      auto check = ui.flat(std::string(id) + "_Tab", bg);
      e.call(check, "SetIsChecked", {page == id});
      ui.fill(ui.add(nav, check, "HorizontalBox"));
      tabs[id] = {check, bg, label};
    }
    ui.add(column, ui.fixed(nav, 48, "NavigationSize"));
    auto rule = ui.make("Border", "NavigationRule");
    e.call(rule, "SetBrushColor", {color(0, .58, .88)});
    ui.add(column, ui.fixed(rule, 4, "NavigationRuleSize"));
    auto body = ui.make("Border", "Body");
    e.call(body, "SetBrushColor", {color(1, 1, 1)});
    e.call(body, "SetPadding", {padding(12, 8, 12, 8)});
    ui.fill(ui.add(column, body));
    auto pages = ui.make("VerticalBox", "Pages");
    ui.content(body, pages);
    for (auto id : {"mods", "support"}) {
      auto &tab = tabs.at(id);
      tab.scroll = ui.make("ScrollBox", std::string(id) + "_Scroll");
      e.call(tab.scroll, "SetConsumeMouseWheel",
             {e.enum_value("/Script/SlateCore.EConsumeMouseWheel", "Always")});
      tab.items = ui.make("VerticalBox", std::string(id) + "_Items");
      e.call(tab.scroll, "AddChild", {tab.items});
      ui.fill(ui.add(pages, tab.scroll));
    }
    scroll = tabs.at("mods").scroll;
    auto card_items = ui.make("VerticalBox", "SupportCardItems");
    ui.add(tabs.at("support").items, card_items);
    ui.add(card_items, ui.fixed(translated("感谢你的支持", 20, "SupportTitle"), 48,
                                "SupportTitleSize"));
    card_note = translated("打赏方式待添加", 16, "SupportPending");
    ui.add(card_items, ui.fixed(card_note, 72, "SupportPendingSize"));
    card = std::make_unique<CardView>(e, ui, gi, menu, card_items, parent);
    card->rectangle = [this](double w, double h) -> std::optional<LayoutRect> {
      if (!layout)
        return {};
      auto r = layout->mod;
      return LayoutRect{r.x + (r.width - w) / 2, r.y + 48 + 4 + 8 + 4, w, h};
    };
    card->active = [this] {
      return page == "support" && !blocked && e.valid(menu) &&
             e.call(menu, "IsInViewport").boolean() && e.valid(root) &&
             !e.call(tabs.at("mods").check, "IsChecked").boolean() && !modal();
    };
    auto actions = ui.make("HorizontalBox", "SupportActions");
    int index = 0;
    for (auto entry : card_config::links()) {
      std::string id = entry.id;
      auto bg = ui.make("Border", id + "_OpenBackground");
      e.call(bg, "SetPadding", {padding(12, 8, 12, 8)});
      auto label = translated(entry.label, 18, id + "_OpenLabel");
      e.call(label, "SetAutoWrapText", {false});
      ui.tone(label, color(1, 1, 1));
      auto ls = ui.content(bg, label);
      e.call(ls, "SetHorizontalAlignment", {2});
      e.call(ls, "SetVerticalAlignment", {2});
      auto check = ui.flat(id + "_Open", bg);
      e.call(check, "SetIsChecked", {false});
      auto s = ui.add(actions, check, "HorizontalBox");
      ui.fill(s);
      e.call(s, "SetPadding", {padding(index ? 6 : 0, 0, index ? 0 : 6, 0)});
      ++index;
      links.push_back({entry, check, bg});
    }
    auto action_slot = ui.add(tabs.at("support").items,
                              ui.fixed(actions, 44, "SupportActionsSize"));
    e.call(action_slot, "SetPadding", {padding(0, 12, 0, 0)});
    link_status_text = "将在浏览器中打开";
    link_status = ui.text(tr(link_status_text), 14, "SupportLinkStatus");
    e.call(link_status, "SetAutoWrapText", {false});
    ui.tone(link_status, color(.35, .43, .47));
    ui.add(tabs.at("support").items,
           ui.fixed(link_status, 24, "SupportLinkStatusSize"));
    auto items = tabs.at("mods").items;
    auto language_row = ui.make("HorizontalBox", "LanguageRow");
    auto language_label = ui.add(language_row, ui.text("语言 / Language", 14, "LanguageLabel"), "HorizontalBox");
    ui.fill(language_label);
    e.call(language_label, "SetVerticalAlignment", {2});
    e.call(language_label, "SetPadding", {padding(0, 0, 8, 0)});
    for (auto [id, title] : std::initializer_list<std::pair<const char *, const char *>>{
             {"zh-CN", "简体中文"}, {"en", "English"}}) {
      auto background = ui.make("Border", std::string("LanguageBackground_") + id);
      e.call(background, "SetPadding", {padding(10, 8, 10, 8)});
      auto label = ui.text(title, 14, std::string("LanguageLabel_") + id);
      e.call(label, "SetAutoWrapText", {false});
      auto ls = ui.content(background, label);
      e.call(ls, "SetHorizontalAlignment", {2});
      e.call(ls, "SetVerticalAlignment", {2});
      auto check = ui.flat(std::string("Language_") + id, background);
      e.call(check, "SetIsChecked", {language == id});
      auto child = ui.add(language_row, check, "HorizontalBox");
      e.call(child, "SetPadding", {padding(4, 0, 0, 0)});
      language_options.push_back({id, check, background, label});
    }
    ui.add(items, ui.fixed(language_row, 44, "LanguageRowSize"));
    language_status = ui.text("", 14, "LanguageStatus");
    e.call(language_status, "SetVisibility", {1});
    ui.add(items, language_status);
    for (size_t i = 0; i < source.size(); ++i) {
      auto &row = source[i];
      auto line = ui.make("HorizontalBox", row.id + "_Row");
      auto name = ui.text(i18n::module_name(language, row.id, row.name), 18, row.id + "_Name");
      e.call(name, "SetAutoWrapText", {false});
      auto ns = ui.add(line, name, "HorizontalBox");
      e.call(ns, "SetVerticalAlignment", {2});
      e.call(ns, "SetPadding", {padding(0, 0, 8, 0)});
      auto shortcut =
          ui.text(row.id == "planet_flattening" ? tr(row.description)
                  : row.id == "wide_range" ? tr("wide.description")
                  : tr("快捷键") + "[" + row.hotkey + "]", 12, row.id + "_Shortcut");
      e.call(shortcut, "SetAutoWrapText", {false});
      auto hs = ui.add(line, shortcut, "HorizontalBox");
      ui.fill(hs);
      e.call(hs, "SetVerticalAlignment", {2});
      Object check, rail, ts;
      {
        auto canvas = ui.make("CanvasPanel", row.id + "_SwitchCanvas");
        rail = ui.make("Border", row.id + "_Rail");
        e.call(rail, "SetPadding", {padding(0, 0, 0, 0)});
        auto rs = ui.add(canvas, rail, "Canvas");
        e.call(rs, "SetPosition", {xy(0, 3)});
        e.call(rs, "SetSize", {xy(56, 30)});
        auto thumb = ui.make("Border", row.id + "_Thumb");
        e.call(thumb, "SetBrushColor", {color(1, 1, 1)});
        ts = ui.add(canvas, thumb, "Canvas");
        e.call(ts, "SetSize", {xy(24, 24)});
        e.call(ts, "SetPosition", {xy(row.enabled ? 29 : 3, 6)});
        auto size = ui.fixed(canvas, 36, row.id + "_SwitchSize");
        e.call(size, "SetWidthOverride", {56});
        check = ui.flat(row.id + "_Control", size);
        e.call(check, "SetIsChecked", {row.enabled});
        e.call(check, "SetIsEnabled", {row.active});
        auto s = ui.add(line, check, "HorizontalBox");
        e.call(s, "SetVerticalAlignment", {2});
        e.call(s, "SetPadding", {padding(8, 0, 0, 0)});
      }
      ui.add(items, ui.fixed(line, 56, row.id + "_RowSize"));
      auto status = ui.text("", 14, row.id + "_Status");
      e.call(status, "SetVisibility", {1});
      ui.add(items, status);
      std::unique_ptr<NumberView> number;
      if(!row.number.null()) {
        number=std::make_unique<NumberView>();number->mount(ui,items,row.id,row.number,language);
      }
      if (i + 1 < source.size()) {
        auto divider = ui.make("Border", row.id + "_Divider");
        e.call(divider, "SetBrushColor", {color(.72, .76, .78)});
        ui.add(items, ui.fixed(divider, 1, row.id + "_DividerSize"));
      }
      rows.push_back({row, check, status, rail, ts, name, shortcut, row.enabled,
                      row.active});
      rows.back().number=std::move(number);
    }
    if (source.empty())
      ui.add(items,
             ui.fixed(translated("尚未安装 Mod", 16, "Empty"), 64, "EmptySize"));
    slot = ui.add(parent, root, "Canvas");
    e.call(slot, "SetAutoSize", {false});
    e.call(slot, "SetZOrder", {20});
  }
  void update_links(bool active) {
    for (auto &link : links) {
      if (e.call(link.check, "IsChecked").boolean()) {
        e.call(link.check, "SetIsChecked", {false});
        if (active) {
          ++link.attempts;
          bool ok = true;
          try {
            auto system =
                ui.resource("/Script/Engine.Default__KismetSystemLibrary");
            std::string url = link.metadata.url;
            if (url != "https://mod.cab" &&
                url != "https://afdian.com/a/Youtiaowei")
              throw std::runtime_error("Unpackaged URL");
            if (!e.call(system, "CanLaunchURL", {url}).boolean())
              throw std::runtime_error("No URL handler");
            e.call(system, "LaunchURL", {url});
          } catch (const std::exception &) {
            ok = false;
          }
          link_status_text = ok ? "已请求在浏览器中打开"
              : std::string(link.metadata.url) == "https://mod.cab"
                  ? "无法打开，请访问 mod.cab" : "无法打开，请使用卡片二维码";
          e.call(link_status, "SetText", {tr(link_status_text)});
          ui.tone(link_status,
                  ok ? color(.35, .43, .47) : color(.64, .12, .07));
        }
      }
      bool focus = e.call(link.check, "HasKeyboardFocus").boolean();
      if (link.focused != focus) {
        e.call(link.bg, "SetBrushColor",
               {focus ? color(0, .39, .66) : color(0, .58, .88)});
        link.focused = focus;
      }
    }
  }
  void update_panel(
      Object game, Object native_menu, const std::vector<ControlRow> &source,
      const std::function<SetModuleResult(const std::string &, bool)> &change,
      const std::string &selected_language,
      const std::function<bool(const std::string &)> &change_language,
      const std::function<Value(const std::string &,double)> &change_number) {
    if (!e.valid(native_menu) ||
        !e.call(native_menu, "IsInViewport").boolean()) {
      close();
      failed_key.clear();
      return;
    }
    auto left = e.get(native_menu, "MenuContentsWrapper_Left").object(),
         right = e.get(native_menu, "MenuContentsWrapper_Right").object();
    if (!e.valid(left) || !e.valid(right))
      return;
    auto ls = e.get(left, "Slot").object(), rs = e.get(right, "Slot").object();
    if (!e.valid(ls) || !e.valid(rs))
      return;
    auto a = e.call(ls, "GetSize"), b = e.call(rs, "GetSize");
    if (a.at("X").number() <= 0 || b.at("X").number() <= 0)
      return;
    auto lib = ui.resource("/Script/UMG.Default__WidgetLayoutLibrary");
    auto view = e.call(lib, "GetViewportSize", {game});
    auto dpi = e.call(lib, "GetViewportScale", {game}).number();
    if (dpi <= 0 || view.at("X").number() <= 0 || view.at("Y").number() <= 0)
      throw std::runtime_error("Invalid viewport");
    std::string ids;
    for (auto &row : source) {
      if (!ids.empty())
        ids += '|';
      ids += row.id;
      ids += row.number.null() ? ":plain" : ":number";
    }
    std::ostringstream stamp;
    stamp << std::setprecision(17) << e.name(native_menu) << ':'
          << native_menu.index << ':' << native_menu.serial << ':'
          << view.at("X").number() << ':' << view.at("Y").number() << ':' << dpi
          << ':' << a.at("X").number() << ':' << a.at("Y").number() << ':'
          << b.at("X").number() << ':' << b.at("Y").number() << ':' << ids;
    auto attempt = stamp.str();
    if (failed_key == attempt)
      return;
    try {
      if (menu != native_menu || registry != ids) {
        close();
        mount(game, native_menu, source, ids);
      }
      bool is_modal = modal();
      set_language(i18n::valid(selected_language) ? selected_language : "zh-CN");
      for (auto id : {"mods", "support"})
        if (!is_modal && page != id &&
            e.call(tabs.at(id).check, "IsChecked").boolean()) {
          page = id;
          break;
        }
      for (auto &[id, tab] : tabs) {
        bool selected = id == page;
        if (e.call(tab.check, "IsChecked").boolean() != selected)
          e.call(tab.check, "SetIsChecked", {selected});
        bool focus = e.call(tab.check, "HasKeyboardFocus").boolean();
        if (tab.selected != selected || tab.focused != focus) {
          e.call(tab.bg, "SetBrushColor",
                 {selected ? color(0, .58, .88)
                  : focus  ? color(.84, .94, 1)
                           : color(1, 1, 1)});
          ui.tone(tab.label, selected ? color(1, 1, 1) : color(.10, .14, .17));
          e.call(tab.scroll, "SetVisibility", {selected ? 0 : 1});
          tab.selected = selected;
          tab.focused = focus;
        }
      }
      scroll = tabs.at(page).scroll;
      for (auto &option : language_options) {
        if (e.call(option.check, "IsChecked").boolean() && option.id != language &&
            !is_modal && page == "mods") {
          bool saved = false;
          try { saved = change_language && change_language(option.id); }
          catch (const std::exception &) { saved = false; }
          language_save_failed = !saved;
          if (saved) set_language(option.id);
          key.clear();
        }
      }
      for (auto &option : language_options) {
        const bool selected = option.id == language;
        if (e.call(option.check, "IsChecked").boolean() != selected)
          e.call(option.check, "SetIsChecked", {selected});
        const bool focused = e.call(option.check, "HasKeyboardFocus").boolean();
        if (option.selected != selected || option.focused != focused) {
          e.call(option.background, "SetBrushColor",
                 {selected ? (focused ? color(0, .39, .66) : color(0, .58, .88))
                           : (focused ? color(.84, .94, 1) : color(.91, .95, .97))});
          ui.tone(option.label, selected ? color(1, 1, 1) : color(.10, .14, .17));
          option.selected = selected;
          option.focused = focused;
        }
      }
      const auto language_message = language_save_failed ? tr("语言保存失败") : "";
      if (language_status_text != language_message) {
        e.call(language_status, "SetText", {language_message});
        e.call(language_status, "SetVisibility", {language_save_failed ? 0 : 1});
        ui.tone(language_status, color(.64, .12, .07));
        language_status_text = language_message;
      }
      auto solve = [&](double desired, double limit = 540) {
        return solve_layout({view.at("X").number() / dpi,
                             view.at("Y").number() / dpi, a.at("X").number(),
                             a.at("Y").number(), b.at("X").number(),
                             b.at("Y").number(), desired, limit});
      };
      double card_height = 120;
      if (card) {
        if (page == "support" && !is_modal) {
          auto cap = solve(1000000, 720);
          auto max_height =
              std::min(480.0, cap.mod.height - 48 - 4 - 16 - 24 - 80);
          auto width = std::max(
              32.0, std::min({320.0, cap.mod.width - 40, max_height / 1.5}));
          if (card->sync(true, width, width * 1.5))
            card_height = width * 1.5;
        } else
          card->sync(false);
        if (!card->error.empty() && !card_error_shown) {
          e.call(card_note, "SetText", {tr("卡片暂不可用，请重新安装 Mod 资源")});
          card_error_shown = true;
        }
      }
      double errors = 0, numeric_height=0;
      for (auto &row : rows)
        {if (!row.message.empty())errors += 24;if(row.number)numeric_height+=112;}
      double height = 48 + 4 + 16 +
                      (page == "support" ? card_height + 80
                       : 44 + (language_save_failed ? 24 : 0) + (source.empty()
                           ? 64
                           : source.size() * 56 + (source.size() - 1) + errors+numeric_height));
      auto next = attempt + ":" + page + ":" + std::to_string(height);
      if (key != next) {
        e.call(root, "ForceLayoutPrepass");
        height =
            std::max(height, e.call(root, "GetDesiredSize").at("Y").number());
        auto solved = solve(height, page == "support" ? 720 : 540);
        e.call(slot, "SetPosition", {xy(solved.mod.x, solved.mod.y)});
        e.call(slot, "SetSize", {xy(solved.mod.width, solved.mod.height)});
        for (size_t i = 0; i < originals.size(); ++i) {
          auto w = originals[i].widget;
          e.call(w, "SetRenderTransformPivot", {xy(i % 2 ? 1 : 0, 0)});
          e.call(w, "SetRenderScale",
                 {xy(solved.native_scale, solved.native_scale)});
        }
        e.call(scroll, "SetScrollBarVisibility",
               {solved.scroll_required ? 0 : 1});
        scrollbar = solved.scroll_required;
        if (!scrollbar)
          e.call(scroll, "SetScrollOffset", {0});
        layout = solved;
        key = next;
        error.clear();
      }
      if (blocked != is_modal) {
        e.call(root, "SetVisibility", {is_modal ? 2 : 0});
        e.call(root, "SetIsEnabled", {!is_modal});
        blocked = is_modal;
      }
      update_links(page == "support" && !is_modal);
      for (size_t i = 0; i < rows.size(); ++i) {
        auto &entry = rows[i];
        auto row = source.at(i);
        const auto localized_name = i18n::module_name(language, row.id, row.name);
        const auto shortcut = row.id == "planet_flattening" ? tr(row.description)
                              : row.id == "wide_range" ? tr("wide.description")
                              : tr("快捷键") + "[" + row.hotkey + "]";
        if (entry.metadata.name != localized_name || entry.shortcut_text != shortcut) {
          e.call(entry.name, "SetText", {localized_name});
          e.call(entry.shortcut, "SetText", {shortcut});
        }
        entry.metadata.name = localized_name;
        entry.metadata.hotkey = row.hotkey;
        entry.shortcut_text = shortcut;
        if (row.id != entry.metadata.id)
          throw std::runtime_error("Registry changed during menu lifetime");
        if(entry.number) {
          std::function<Value(double)> set;
          if(change_number)set=[&](double value){return change_number(row.id,value);};
          entry.number->update(ui,row.number,!is_modal&&page=="mods"&&row.active,language,set);
          entry.metadata.number=row.number;
        }
        auto checked = e.call(entry.check, "IsChecked").boolean();
        bool enabled = row.enabled;
        std::string message = row.error;
        if (!is_modal && page == "mods" && row.active &&
            checked != entry.presented) {
          try {
            auto result = change(row.id, checked);
            enabled = result.enabled;
            message = result.error;
          } catch (const std::exception &) {
            message = tr("切换失败");
          }
        }
        if (e.call(entry.check, "IsChecked").boolean() != enabled)
          e.call(entry.check, "SetIsChecked", {enabled});
        entry.presented = enabled;
        if (entry.active != row.active) {
          e.call(entry.check, "SetIsEnabled", {row.active});
          entry.active = row.active;
        }
        entry.status_text =
            message.empty()
                ? (row.id == "planet_flattening" ? shortcut
                   : "[" + row.hotkey + "] " + tr(enabled ? "已开启" : "已关闭"))
                : message;
        if (entry.message != message) {
          e.call(entry.status, "SetText", {message});
          e.call(entry.status, "SetVisibility", {message.empty() ? 1 : 0});
          ui.tone(entry.status, color(.64, .12, .07));
          entry.message = message;
        }
        bool focused = e.call(entry.check, "HasKeyboardFocus").boolean();
        if (entry.visual != enabled || entry.focused != focused) {
          e.call(entry.rail, "SetBrushColor",
                 {enabled ? (focused ? color(0, .39, .66) : color(0, .58, .88))
                          : (focused ? color(.34, .39, .43)
                                     : color(.58, .63, .66))});
          e.call(entry.thumb, "SetPosition", {xy(enabled ? 29 : 3, 6)});
          entry.visual = enabled;
          entry.focused = focused;
        }
      }
    } catch (const std::exception &ex) {
      error = ex.what();
      failed_key = attempt;
      close();
    }
  }
  Value snapshot() {
    bool visible = live();
    Value::Record panel{{"created", visible},
                        {"visible", visible && !blocked},
                        {"blocked_modal", blocked},
                        {"serial", serial},
                        {"language", language},
                        {"error", error},
                        {"rows", Value::Array{}},
                        {"layout", layout ? layout_value(*layout) : Value{}}};
    if (visible) {
      panel["page"] = page;
      panel["scroll"] = e.call(scroll, "GetScrollOffset");
      panel["menu"] = e.name(menu);
      panel["scrollbar_visible"] = scrollbar;
      panel["support_link_status"] = tr(link_status_text);
      Value::Record options;
      for (const auto &option : language_options) options[option.id] = e.name(option.check);
      panel["language_options"] = options;
      Value::Array actions;
      for (auto &link : links)
        actions.push_back(record({{"id", link.metadata.id},
                                  {"label", tr(link.metadata.label)},
                                  {"url", link.metadata.url},
                                  {"widget_name", e.name(link.check)},
                                  {"attempts", link.attempts}}));
      panel["support_links"] = actions;
      Value::Record tab_names;
      for (auto &[id, tab] : tabs)
        tab_names[id] = e.name(tab.check);
      panel["tabs"] = tab_names;
      panel["card"] = card ? card->snapshot()
                           : record({{"created", false}, {"active", false}});
      Value::Array row_values;
      for (auto &r : rows)
        row_values.push_back(
            record({{"id", r.metadata.id},
                    {"checked", e.valid(r.check) ? e.call(r.check, "IsChecked") : Value(false)},
                    {"focused", e.valid(r.check) ? e.call(r.check, "HasKeyboardFocus") : Value(false)},
                    {"widget_name", e.valid(r.check) ? e.name(r.check) : ""},
                    {"status_text", r.status_text},
                    {"name_text", r.metadata.name},
                    {"shortcut_text", r.shortcut_text},
                    {"name_widget", e.name(r.name)},
                    {"shortcut_widget", e.name(r.shortcut)},
                    {"number_widget", r.number?r.number->widget(e):""},
                    {"switch_position",
                     r.visual.value_or(false) ? "right" : "left"}}));
      panel["rows"] = row_values;
    }
    return record({{"panel", panel},
                   {"hud", hud.snapshot()},
                   {"error", !hud_error.empty() ? hud_error : error}});
  }
};
Presentation::Presentation(Engine &e) : impl(std::make_unique<Impl>(e)) {}
Presentation::~Presentation() = default;
void Presentation::discard() { impl->discard(); }
void Presentation::suspend() {
  // Menu widgets can remain attached to the GameInstance-owned viewport even
  // after playable-world readiness disappears. Close valid UI while its weak
  // references still belong to the current epoch, then abandon the references.
  std::string cleanup_error;
  try {
    impl->close();
  } catch (const std::exception &ex) {
    cleanup_error = ex.what();
  } catch (...) {
    cleanup_error = "Non-standard menu suspension cleanup failure";
  }
  try {
    impl->discard();
  } catch (...) {
    // HUD destruction can throw a non-standard exception too. Local state and
    // runtime input/epoch invalidation must still complete; do not claim detach.
    impl->hud.discard();
    if (cleanup_error.empty())
      cleanup_error = "Non-standard HUD suspension cleanup failure";
  }
  if (!cleanup_error.empty())
    impl->error = std::move(cleanup_error);
}
void Presentation::close() {
  impl->close();
  impl->hud.destroy();
}
void Presentation::tick_card() {
  if (impl->card)
    impl->card->tick();
}
void Presentation::update(
    Object gi, Object menu, const std::vector<ControlRow> &rows,
    std::function<SetModuleResult(const std::string &, bool)> change,
    const HUDView &hud, const std::string &language,
    std::function<bool(const std::string &)> change_language,
    std::function<Value(const std::string &,double)> change_number) {
  try {
    impl->update_panel(gi, menu, rows, change, language, change_language,change_number);
  } catch (const std::exception &ex) {
    impl->error = ex.what();
  }
  try {
    impl->hud.render(gi, hud);
  } catch (const std::exception &ex) {
    impl->hud_error = ex.what();
  }
}
Value Presentation::snapshot() { return impl->snapshot(); }
} // namespace astro
