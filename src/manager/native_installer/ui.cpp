// Win32 native UI implementation.
// Native controls, Chinese text, DPI-aware, embedded B icon.
#include "ui.h"
#include "output_file.h"
#include "localization.h"
#include "json.hpp"
#include "safe_io.h"
#include <algorithm>
#include <commctrl.h>
#include <fstream>
#include <iostream>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <thread>
#include <windows.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace ui {

// ── Control IDs ─────────────────────────────────────────────────────────────
enum {
  ID_LABEL_DIR = 100,
  ID_EDIT_PATH = 101,
  ID_BTN_CHOOSE = 102,
  ID_BTN_REFRESH = 103,
  ID_LIST_MODULES = 104,
  ID_CHECK_MODULE = 105,
  ID_LABEL_STATUS = 106,
  ID_LABEL_DESC = 107,
  ID_LABEL_MESSAGE = 108,
  ID_BTN_BACKUP = 109,
  ID_BTN_COMMUNITY = 110,
  ID_BTN_UNINSTALL = 111,
  ID_BTN_INSTALL = 112,
  ID_LABEL_FOOTER = 113,
  ID_LABEL_TITLE = 114,
  ID_LABEL_VERSION = 115,
  ID_LANGUAGE = 116,
  ID_LANGUAGE_LABEL = 117,
  ID_BTN_INSTALL_MENU = 118,
  ID_BTN_UNINSTALL_MENU = 119,
  ID_LABEL_MENU = 120,
  ID_CHECK_SAVE_BACKUP = 121,
  // Confirmation dialog
  ID_DLG_CONFIRM = 200,
  ID_DLG_TEXT = 201,
  ID_DLG_CHECK_ACK = 202,
  ID_DLG_BTN_CANCEL = 203,
  ID_DLG_BTN_CONFIRM = 204,
};

// ── Globals for window procedure ────────────────────────────────────────────
static AppState g_state;
static std::string g_language = "zh-CN";
std::string language() { return g_language; }
static std::string tr(const std::string &value) {
  return installer_i18n::text(g_language, value);
}
AppState &state() { return g_state; }
static Callbacks g_cb;
static HFONT g_font = nullptr;
static HFONT g_font_bold = nullptr;
static HFONT g_font_small = nullptr;
static HWND g_hwnd_main = nullptr;
static HWND g_hwnd_path = nullptr;
static HWND g_hwnd_choose = nullptr;
static HWND g_hwnd_refresh = nullptr;
static HWND g_hwnd_message = nullptr;
static HWND g_hwnd_install = nullptr;
static HWND g_hwnd_uninstall = nullptr;
static HWND g_hwnd_backup = nullptr;
static HWND g_hwnd_community = nullptr;
static HWND g_hwnd_title = nullptr;
static HWND g_hwnd_version = nullptr;
static HWND g_hwnd_footer = nullptr;
static HWND g_hwnd_status = nullptr;
static HWND g_hwnd_desc = nullptr;
static HWND g_hwnd_dlg = nullptr;
static HWND g_hwnd_language = nullptr;
static HICON g_icon = nullptr;
static bool g_smoke_test = false;
static std::thread g_worker;
static std::string g_report_path;
static int g_scale_num = 1, g_scale_den = 1;
static int g_smoke_dpi = 0;
static std::string g_smoke_action;
static int g_action_stage = 0;
static bool g_operation_ok = true;
void set_smoke_action(const std::string &action) {
  if (action != "install" && action != "uninstall")
    throw std::runtime_error("Invalid smoke action");
  g_smoke_action = action;
}
void set_smoke_dpi(int dpi) {
  if (dpi != 96 && dpi != 144 && dpi != 192)
    throw std::runtime_error("Unsupported smoke DPI");
  g_smoke_dpi = dpi;
}
static json::Array g_geometry;
static bool g_geometry_ok = true;
static void measure_children(HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  for (HWND c = GetWindow(window, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
    RECT r;
    GetWindowRect(c, &r);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&r), 2);
    bool inside = r.left >= 0 && r.top >= 0 && r.right <= client.right && r.bottom <= client.bottom;
    g_geometry_ok = g_geometry_ok && inside;
    g_geometry.push_back(json::Object{{"id", GetDlgCtrlID(c)},
                                      {"x", int(r.left)},
                                      {"y", int(r.top)},
                                      {"width", int(r.right - r.left)},
                                      {"height", int(r.bottom - r.top)},
                                      {"inside", inside}});
  }
}

// ── DPI helpers ─────────────────────────────────────────────────────────────
static int scale(int v) { return v * g_scale_num / g_scale_den; }

static HFONT create_font(int size, bool bold = false) {
  return CreateFontW(-scale(size), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH, L"Microsoft YaHei UI");
}

// ── Helper to set window text from UTF-8 ────────────────────────────────────
static void set_text(HWND hwnd, const std::string &utf8) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  std::wstring wstr(wlen, 0);
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wstr.data(), wlen);
  SetWindowTextW(hwnd, wstr.c_str());
}

static std::string get_text(HWND hwnd) {
  int len = GetWindowTextLengthW(hwnd);
  if (len <= 0)
    return "";
  std::wstring wstr(len + 1, 0);
  GetWindowTextW(hwnd, wstr.data(), len + 1);
  int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string utf8(ulen, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, utf8.data(), ulen, nullptr, nullptr);
  utf8.resize(ulen - 1);
  return utf8;
}

static void enable_window(HWND hwnd, bool enabled) { EnableWindow(hwnd, enabled ? TRUE : FALSE); }

// ── Module list rendering ───────────────────────────────────────────────────
static int g_module_scroll = 0;
static void refresh_ui();
static RECT toggle_rect(const RECT &item) {
  return {item.right - scale(100), item.top + scale(44), item.right - scale(12), item.bottom - scale(10)};
}
static void paint_modules(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc;
  GetClientRect(hwnd, &rc);
  FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));

  int y = 4 - g_module_scroll;
  int item_h = scale(86);
  int w = rc.right - 8;

  for (size_t i = 0; i < g_state.modules.size(); ++i) {
    auto &m = g_state.modules[i];
    const auto name = g_language == "en" && !m.name_en.empty() ? m.name_en : tr(m.name);
    const auto description = g_language == "en" && !m.description_en.empty() ? m.description_en : tr(m.description);
    const auto status = tr(m.status);
    RECT item_rc = {4, y, 4 + w, y + item_h};
    // Card background
    HBRUSH card_brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &item_rc, card_brush);
    DeleteObject(card_brush);
    // Border
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(206, 220, 229));
    HPEN old = (HPEN)SelectObject(hdc, pen);
    Rectangle(hdc, item_rc.left, item_rc.top, item_rc.right, item_rc.bottom);
    SelectObject(hdc, old);
    DeleteObject(pen);

    // Checkbox area
    int cb_x = item_rc.left + scale(12);
    int cb_y = item_rc.top + scale(12);
    RECT cb_rc = {cb_x, cb_y, cb_x + scale(20), cb_y + scale(20)};
    DrawFrameControl(hdc, &cb_rc, DFC_BUTTON, DFCS_BUTTONCHECK | (m.selected ? DFCS_CHECKED : 0));

    // Module name
    int tx = cb_x + scale(26);
    SetBkMode(hdc, TRANSPARENT);
    HFONT old_font = (HFONT)SelectObject(hdc, g_font_bold);
    SetTextColor(hdc, RGB(22, 62, 88));
    RECT name_rc = {tx, item_rc.top + scale(12), item_rc.right - scale(168),
                    item_rc.top + scale(32)};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wname(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname.data(), wlen);
    DrawTextW(hdc, wname.c_str(), -1, &name_rc, DT_LEFT | DT_SINGLELINE);

    // Status text
    SelectObject(hdc, g_font_small);
    SetTextColor(hdc, m.installed ? RGB(44, 117, 88) : RGB(107, 126, 139));
    RECT status_rc = {item_rc.right - scale(160), item_rc.top + scale(14),
                      item_rc.right - scale(12), item_rc.top + scale(30)};
    wlen = MultiByteToWideChar(CP_UTF8, 0, status.c_str(), -1, nullptr, 0);
    std::wstring wstatus(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, status.c_str(), -1, wstatus.data(), wlen);
    DrawTextW(hdc, wstatus.c_str(), -1, &status_rc, DT_RIGHT | DT_SINGLELINE);

    // Description
    SelectObject(hdc, g_font);
    SetTextColor(hdc, RGB(80, 107, 126));
    RECT desc_rc = {tx, item_rc.top + scale(36), item_rc.right - scale(m.installed ? 110 : 12),
                    item_rc.bottom - scale(8)};
    wlen = MultiByteToWideChar(CP_UTF8, 0, description.c_str(), -1, nullptr, 0);
    std::wstring wdesc(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, description.c_str(), -1, wdesc.data(), wlen);
    DrawTextW(hdc, wdesc.c_str(), -1, &desc_rc, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);

    if (m.installed) {
      auto button = toggle_rect(item_rc);
      const bool active = g_state.valid && !g_state.running && !g_state.busy;
      DrawFrameControl(hdc, &button, DFC_BUTTON, DFCS_BUTTONPUSH | (active ? 0 : DFCS_INACTIVE));
      SetTextColor(hdc, active ? RGB(22, 62, 88) : RGB(130, 140, 145));
      const auto caption = io::wide(tr(m.enabled ? "停用" : "启用"));
      DrawTextW(hdc, caption.c_str(), -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(hdc, old_font);
    y += item_h + scale(8);
  }
  EndPaint(hwnd, &ps);
}

static HWND g_hwnd_modules = nullptr;
static WNDPROC g_orig_modules_proc = nullptr;
static LRESULT CALLBACK modules_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    paint_modules(hwnd);
    return 0;
  }
  if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL) {
    SCROLLINFO si{sizeof(si), SIF_ALL};
    GetScrollInfo(hwnd, SB_VERT, &si);
    int position = si.nPos;
    if (msg == WM_MOUSEWHEEL) position -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * scale(48);
    else switch (LOWORD(wp)) {
      case SB_LINEUP: position -= scale(24); break;
      case SB_LINEDOWN: position += scale(24); break;
      case SB_PAGEUP: position -= si.nPage; break;
      case SB_PAGEDOWN: position += si.nPage; break;
      case SB_THUMBTRACK: position = si.nTrackPos; break;
    }
    si.fMask = SIF_POS;
    si.nPos = position;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    g_module_scroll = GetScrollPos(hwnd, SB_VERT);
    InvalidateRect(hwnd, nullptr, TRUE);
    return 0;
  }
  if (msg == WM_LBUTTONDOWN) {
    if (g_state.busy) return 0;
    const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    const int item_h = scale(86), stride = item_h + scale(8);
    const int content_y = point.y + g_module_scroll - 4;
    const int idx = content_y / stride;
    if (content_y >= 0 && idx < (int)g_state.modules.size() && content_y % stride < item_h) {
      RECT client; GetClientRect(hwnd, &client);
      const int top = 4 + idx * stride - g_module_scroll;
      RECT item{4, top, client.right - 4, top + item_h};
      RECT checkbox{item.left + scale(12), top + scale(12), item.left + scale(32), top + scale(32)};
      auto &row = g_state.modules[idx];
      const auto button = toggle_rect(item);
      if (PtInRect(&checkbox, point)) {
        row.selected = !row.selected;
        refresh_ui();
      } else if (row.installed && PtInRect(&button, point) && g_state.valid && !g_state.running && g_cb.toggle) {
        const auto id = row.id;
        g_cb.toggle(id, !row.enabled);
      }
    }
    return 0;
  }
  if (msg == WM_ERASEBKGND)
    return 1;
  return CallWindowProcW(g_orig_modules_proc, hwnd, msg, wp, lp);
}

// ── Refresh UI from state ───────────────────────────────────────────────────
static void refresh_ui() {
  for (const auto &[id, text] : std::initializer_list<std::pair<int, const char *>>{
           {ID_LABEL_TITLE, "异星探险家 · MOD"}, {ID_LABEL_DIR, "目录"},
           {ID_BTN_CHOOSE, "选择"}, {ID_BTN_REFRESH, "刷新"},
           {ID_BTN_BACKUP, "备份目录"}, {ID_BTN_COMMUNITY, "加入 QQ 群"},
           {ID_BTN_UNINSTALL, "卸载所选"}, {ID_BTN_INSTALL, "安装 / 更新所选"},
           {ID_BTN_INSTALL_MENU, "仅安装菜单"}, {ID_BTN_UNINSTALL_MENU, "卸载空菜单"},
           {ID_CHECK_SAVE_BACKUP, "同时备份存档（可选）"},
           {ID_LANGUAGE_LABEL, "语言"},
           {ID_LABEL_FOOTER, "离线安装 · 无需常驻 · 卸载前先取下特殊插槽内的大件，并保存退出游戏。"}})
    set_text(GetDlgItem(g_hwnd_main, id), tr(text));
  set_text(g_hwnd_main, tr("异星探险家 · Mod 安装器"));
  set_text(g_hwnd_path, g_state.game_path);
  set_text(g_hwnd_message, tr(g_state.message));
  set_text(g_hwnd_version, "Steam · " + g_state.game_version);
  set_text(GetDlgItem(g_hwnd_main, ID_LABEL_MENU),
           tr(g_state.menu_installed ? "共享菜单：已安装" :
              (g_state.menu_offered ? "共享菜单：未安装（安装 Mod 时自动补装）" :
                                     "共享菜单：未安装（请先安装公共框架包）")));
  bool installable = false, removable = false;
  for (const auto &row : g_state.modules) if (row.selected) {
    installable = installable || row.offered;
    removable = removable || row.installed;
  }
  const bool active = g_state.valid && !g_state.busy && !g_state.running;
  const bool framework_package = g_state.menu_offered &&
      std::none_of(g_state.modules.begin(), g_state.modules.end(), [](const auto &row) { return row.offered; });
  enable_window(g_hwnd_install, active && (installable || framework_package));
  enable_window(g_hwnd_uninstall, active && (removable || (framework_package && g_state.menu_installed)));
  enable_window(GetDlgItem(g_hwnd_main, ID_BTN_INSTALL_MENU), active && g_state.menu_offered);
  enable_window(GetDlgItem(g_hwnd_main, ID_BTN_UNINSTALL_MENU),
                active && g_state.menu_installed &&
                std::none_of(g_state.modules.begin(), g_state.modules.end(),
                             [](const auto &row) { return row.installed; }));
  enable_window(g_hwnd_path, !g_state.busy);
  enable_window(g_hwnd_choose, !g_state.busy);
  enable_window(g_hwnd_refresh, !g_state.busy);
  enable_window(g_hwnd_language, !g_state.busy && !g_hwnd_dlg);
  enable_window(GetDlgItem(g_hwnd_main, ID_CHECK_SAVE_BACKUP), active && !g_hwnd_dlg);
  SendMessageW(GetDlgItem(g_hwnd_main, ID_CHECK_SAVE_BACKUP), BM_SETCHECK,
               g_state.backup_saves ? BST_CHECKED : BST_UNCHECKED, 0);

  // Update message background color
  InvalidateRect(g_hwnd_message, nullptr, TRUE);
  // Redraw module list
  if (g_hwnd_modules) {
    RECT client; GetClientRect(g_hwnd_modules, &client);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMax = std::max(0, 8 + int(g_state.modules.size()) * scale(94) - 1);
    si.nPage = client.bottom;
    si.nPos = g_module_scroll;
    SetScrollInfo(g_hwnd_modules, SB_VERT, &si, TRUE);
    g_module_scroll = GetScrollPos(g_hwnd_modules, SB_VERT);
    InvalidateRect(g_hwnd_modules, nullptr, TRUE);
  }
}

void run_work(std::function<AppState(AppState)> operation) {
  if (g_state.busy)
    return;
  if (g_worker.joinable())
    g_worker.join();
  auto initial = g_state;
  g_state.busy = true;
  g_state.message = "正在备份、写入并核对文件。请勿启动游戏。";
  refresh_ui();
  g_worker = std::thread([initial, operation] {
    auto result = new AppState(initial);
    try {
      *result = operation(initial);
    } catch (const std::exception &e) {
      result->has_error = true;
      result->message = e.what();
    }
    result->busy = false;
    result->pending_text.clear();
    if (!PostMessageW(g_hwnd_main, WM_APP + 1, 0, reinterpret_cast<LPARAM>(result)))
      delete result;
  });
}

// ── Confirmation dialog ─────────────────────────────────────────────────────
static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    // Text area
    CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, scale(14),
        scale(14), scale(552), scale(280), hwnd, (HMENU)ID_DLG_TEXT, nullptr, nullptr);
    SendMessage(GetDlgItem(hwnd, ID_DLG_TEXT), WM_SETFONT, (WPARAM)g_font, TRUE);

    // Acknowledgment checkbox
    if (g_state.requires_ack) {
      HWND chk = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 scale(14), scale(300), scale(552), scale(24), hwnd,
                                 (HMENU)ID_DLG_CHECK_ACK, nullptr, nullptr);
      SendMessage(chk, WM_SETFONT, (WPARAM)g_font, TRUE);
      set_text(chk, tr(g_state.pending_action == "uninstall" || g_state.pending_action == "uninstall-menu"
                          ? "我已了解存档风险，确认卸载所选功能"
                          : "我确认备份并接管已识别的旧版安装"));
    }

    // Buttons
    HWND btn_cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      scale(380), scale(330), scale(80), scale(34), hwnd,
                                      (HMENU)ID_DLG_BTN_CANCEL, nullptr, nullptr);
    SendMessage(btn_cancel, WM_SETFONT, (WPARAM)g_font, TRUE);

    HWND btn_confirm = CreateWindowExW(
        0, L"BUTTON", L"确认并执行", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, scale(470),
        scale(330), scale(100), scale(34), hwnd, (HMENU)ID_DLG_BTN_CONFIRM, nullptr, nullptr);
    SendMessage(btn_confirm, WM_SETFONT, (WPARAM)g_font_bold, TRUE);

    // Set text
    set_text(btn_cancel, tr("取消"));
    set_text(btn_confirm, tr("确认并执行"));
    set_text(GetDlgItem(hwnd, ID_DLG_TEXT),
             installer_i18n::confirmation(g_language, g_state.pending_action,
                                          g_state.backup_path, g_state.legacy, g_state.backup_saves));

    // If no ack required, enable confirm immediately
    if (!g_state.requires_ack)
      enable_window(btn_confirm, true);
    else
      enable_window(btn_confirm, false);

    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp) == ID_DLG_BTN_CANCEL) {
      g_cb.cancel();
      DestroyWindow(hwnd);
      return 0;
    }
    if (LOWORD(wp) == ID_DLG_BTN_CONFIRM) {
      bool ack = !g_state.requires_ack;
      if (g_state.requires_ack) {
        ack = SendMessage(GetDlgItem(hwnd, ID_DLG_CHECK_ACK), BM_GETCHECK, 0, 0) == BST_CHECKED;
      }
      if (!ack)
        return 0;
      g_cb.confirm(ack);
      refresh_ui();
      DestroyWindow(hwnd);
      return 0;
    }
    if (LOWORD(wp) == ID_DLG_CHECK_ACK && HIWORD(wp) == BN_CLICKED) {
      bool checked = SendMessage((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
      enable_window(GetDlgItem(hwnd, ID_DLG_BTN_CONFIRM), checked);
      return 0;
    }
    break;
  case WM_CTLCOLORSTATIC: {
    HDC hdc = (HDC)wp;
    SetBkMode(hdc, TRANSPARENT);
    return (LRESULT)GetStockObject(WHITE_BRUSH);
  }
  case WM_DESTROY:
    g_hwnd_dlg = nullptr;
    EnableWindow(g_hwnd_main, TRUE);
    refresh_ui();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_confirmation() {
  if (g_hwnd_dlg)
    return;
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = dlg_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"AstroConfirmDlg";
  RegisterClassExW(&wc);

  RECT client = {0, 0, scale(580), scale(380)};
  AdjustWindowRectEx(&client, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
  int dlg_w = client.right - client.left, dlg_h = client.bottom - client.top;
  RECT parent;
  GetWindowRect(g_hwnd_main, &parent);
  int x = parent.left + (parent.right - parent.left - dlg_w) / 2;
  int y = parent.top + (parent.bottom - parent.top - dlg_h) / 2;

  g_hwnd_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AstroConfirmDlg",
                               io::wide(tr(g_state.pending_action == "uninstall" || g_state.pending_action == "uninstall-menu"
                                               ? "确认卸载与存档风险"
                                               : "确认安装 / 更新")).c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, dlg_w, dlg_h, g_hwnd_main,
                               nullptr, GetModuleHandleW(nullptr), nullptr);

  EnableWindow(g_hwnd_main, FALSE);
  ShowWindow(g_hwnd_dlg, SW_SHOW);
  UpdateWindow(g_hwnd_dlg);
}

// ── Main window procedure ───────────────────────────────────────────────────
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_APP + 1: {
    auto result = reinterpret_cast<AppState *>(lp);
    g_state = *result;
    delete result;
    if (g_worker.joinable())
      g_worker.join();
    refresh_ui();
    return 0;
  }
  case WM_DPICHANGED: {
    const int old = g_scale_num, next = HIWORD(wp);
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
      RECT r;
      GetWindowRect(c, &r);
      MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&r), 2);
      SetWindowPos(c, nullptr, MulDiv(r.left, next, old), MulDiv(r.top, next, old),
                   MulDiv(r.right - r.left, next, old), MulDiv(r.bottom - r.top, next, old),
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_scale_num = next;
    g_scale_den = 96;
    DeleteObject(g_font);
    DeleteObject(g_font_bold);
    DeleteObject(g_font_small);
    g_font = create_font(13);
    g_font_bold = create_font(13, true);
    g_font_small = create_font(11);
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
      SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    auto r = reinterpret_cast<RECT *>(lp);
    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hwnd_dlg) {
      DestroyWindow(g_hwnd_dlg);
      show_confirmation();
    }
    refresh_ui();
    return 0;
  }
  case WM_CREATE: {
    g_hwnd_main = hwnd;
    int pad = scale(16);
    int y = pad;
    int w = scale(600) - pad * 2;

    // Title bar
    g_hwnd_title = CreateWindowExW(
        0, L"STATIC", L"异星探险家 · MOD", WS_CHILD | WS_VISIBLE | SS_LEFT, pad + scale(8), y,
        scale(216), scale(24), hwnd, (HMENU)ID_LABEL_TITLE, nullptr, nullptr);
    SendMessage(g_hwnd_title, WM_SETFONT, (WPARAM)g_font_bold, TRUE);

    g_hwnd_version =
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT, scale(244),
                        y, scale(150), scale(24), hwnd, (HMENU)ID_LABEL_VERSION, nullptr, nullptr);
    SendMessage(g_hwnd_version, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    {
      std::string ver = "Steam · 1.43.2.0  /  v0.2.0";
      set_text(g_hwnd_version, ver);
    }
    auto language_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        scale(404), y + scale(3), scale(62), scale(22), hwnd,
        (HMENU)ID_LANGUAGE_LABEL, nullptr, nullptr);
    SendMessage(language_label, WM_SETFONT, (WPARAM)g_font, TRUE);
    g_hwnd_language = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        scale(472), y - scale(1), scale(112), scale(120), hwnd,
        (HMENU)ID_LANGUAGE, nullptr, nullptr);
    SendMessage(g_hwnd_language, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_hwnd_language, CB_ADDSTRING, 0, (LPARAM)L"简体中文");
    SendMessageW(g_hwnd_language, CB_ADDSTRING, 0, (LPARAM)L"English");
    SendMessageW(g_hwnd_language, CB_SETCURSEL, g_language == "en" ? 1 : 0, 0);
    y += scale(30);

    // Game path row
    HWND lbl_dir =
        CreateWindowExW(0, L"STATIC", L"目录", WS_CHILD | WS_VISIBLE | SS_LEFT, pad, y + scale(4),
                        scale(44), scale(24), hwnd, (HMENU)ID_LABEL_DIR, nullptr, nullptr);
    SendMessage(lbl_dir, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_hwnd_path =
        CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                        pad + scale(50), y, w - scale(50) - scale(164) - scale(8), scale(34), hwnd,
                        (HMENU)ID_EDIT_PATH, nullptr, nullptr);
    SendMessage(g_hwnd_path, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_hwnd_choose = CreateWindowExW(0, L"BUTTON", L"选择", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    pad + w - scale(164), y, scale(76), scale(34), hwnd,
                                    (HMENU)ID_BTN_CHOOSE, nullptr, nullptr);
    SendMessage(g_hwnd_choose, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_hwnd_refresh = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     pad + w - scale(80), y, scale(76), scale(34), hwnd,
                                     (HMENU)ID_BTN_REFRESH, nullptr, nullptr);
    SendMessage(g_hwnd_refresh, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += scale(42);

    // Module list (custom painted)
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AstroModuleList";
    RegisterClassExW(&wc);

    g_hwnd_modules =
        CreateWindowExW(0, L"AstroModuleList", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL,
                        pad, y, w, scale(100), hwnd, (HMENU)ID_LIST_MODULES, nullptr, nullptr);
    g_orig_modules_proc =
        (WNDPROC)SetWindowLongPtrW(g_hwnd_modules, GWLP_WNDPROC, (LONG_PTR)modules_proc);
    y += scale(108);

    auto menu_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        pad, y + scale(4), scale(568), scale(24), hwnd, (HMENU)ID_LABEL_MENU, nullptr, nullptr);
    SendMessageW(menu_label, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    y += scale(32);
    for (const auto &[id, x] : std::initializer_list<std::pair<int, int>>{
             {ID_BTN_INSTALL_MENU, 0}, {ID_BTN_UNINSTALL_MENU, 184}}) {
      auto button = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
          pad + scale(x), y, scale(176), scale(34), hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
      SendMessageW(button, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    y += scale(42);

    // World saves are opt-in; Mod/config rollback copies remain automatic.
    auto save_backup = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | BS_AUTOCHECKBOX, pad, y, scale(300), scale(24), hwnd,
        (HMENU)ID_CHECK_SAVE_BACKUP, nullptr, nullptr);
    SendMessageW(save_backup, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += scale(30);

    // Status/message area
    g_hwnd_message = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, pad,
        y, w, scale(42), hwnd, (HMENU)ID_LABEL_MESSAGE, nullptr, nullptr);
    SendMessage(g_hwnd_message, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += scale(50);

    // Bottom buttons
    g_hwnd_backup =
        CreateWindowExW(0, L"BUTTON", L"备份目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, pad, y,
                        scale(70), scale(34), hwnd, (HMENU)ID_BTN_BACKUP, nullptr, nullptr);
    SendMessage(g_hwnd_backup, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_hwnd_community = CreateWindowExW(
        0, L"BUTTON", L"加入 QQ 群", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, pad + scale(76), y,
        scale(90), scale(34), hwnd, (HMENU)ID_BTN_COMMUNITY, nullptr, nullptr);
    SendMessage(g_hwnd_community, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_hwnd_uninstall = CreateWindowExW(
        0, L"BUTTON", L"卸载所选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, pad + w - scale(220), y,
        scale(80), scale(34), hwnd, (HMENU)ID_BTN_UNINSTALL, nullptr, nullptr);
    SendMessage(g_hwnd_uninstall, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_hwnd_install =
        CreateWindowExW(0, L"BUTTON", L"安装 / 更新所选", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        pad + w - scale(136), y, scale(136), scale(34), hwnd, (HMENU)ID_BTN_INSTALL,
                        nullptr, nullptr);
    SendMessage(g_hwnd_install, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
    y += scale(42);

    // Footer
    g_hwnd_footer = CreateWindowExW(
        0, L"STATIC", L"离线安装 · 无需常驻 · 卸载前先取下特殊插槽内的大件，并保存退出游戏。",
        WS_CHILD | WS_VISIBLE | SS_LEFT, pad, y, w, scale(28), hwnd, (HMENU)ID_LABEL_FOOTER,
        nullptr, nullptr);
    SendMessage(g_hwnd_footer, WM_SETFONT, (WPARAM)g_font_small, TRUE);

    refresh_ui();
    return 0;
  }
  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case ID_CHECK_SAVE_BACKUP:
      if (HIWORD(wp) == BN_CLICKED && !g_state.busy && !g_hwnd_dlg) {
        g_state.backup_saves = SendMessageW(GetDlgItem(hwnd, ID_CHECK_SAVE_BACKUP),
                                          BM_GETCHECK, 0, 0) == BST_CHECKED;
        refresh_ui();
      }
      return 0;
    case ID_LANGUAGE:
      if (HIWORD(wp) == CBN_SELCHANGE && !g_state.busy && !g_hwnd_dlg) {
        const auto selected = SendMessageW(g_hwnd_language, CB_GETCURSEL, 0, 0);
        if (selected == 0 || selected == 1) {
          const std::string next = selected == 1 ? "en" : "zh-CN";
          if (next != g_language) {
#if defined(INSTALLER_TEST_DRIVER) || defined(MANAGER_TEST_DRIVER)
            g_language = next;
#else
            if (installer_i18n::save_language(next))
              g_language = next;
            else {
              g_state.message = "语言设置保存失败，已保留原语言。";
              g_state.has_error = true;
            }
#endif
          }
        }
        SendMessageW(g_hwnd_language, CB_SETCURSEL, g_language == "en" ? 1 : 0, 0);
        refresh_ui();
      }
      return 0;
    case ID_BTN_CHOOSE:
      g_cb.choose_game();
      refresh_ui();
      return 0;
    case ID_BTN_REFRESH:
      g_cb.refresh();
      refresh_ui();
      return 0;
    case ID_BTN_INSTALL: {
      // Collect selected modules
      g_state.pending_action = "install";
      g_cb.install();
      if (g_state.busy || !g_state.pending_text.empty())
        show_confirmation();
      refresh_ui();
      return 0;
    }
    case ID_BTN_UNINSTALL: {
      g_state.pending_action = "uninstall";
      g_cb.uninstall();
      if (g_state.busy || !g_state.pending_text.empty())
        show_confirmation();
      refresh_ui();
      return 0;
    }
    case ID_BTN_INSTALL_MENU:
    case ID_BTN_UNINSTALL_MENU: {
      if (LOWORD(wp) == ID_BTN_INSTALL_MENU) g_cb.install_menu();
      else g_cb.uninstall_menu();
      if (!g_state.pending_text.empty()) show_confirmation();
      refresh_ui();
      return 0;
    }
    case ID_BTN_BACKUP:
      g_cb.open_backups();
      return 0;
    case ID_BTN_COMMUNITY:
      g_cb.open_community();
      return 0;
    case ID_EDIT_PATH:
      if (HIWORD(wp) == EN_KILLFOCUS) {
        std::string path = get_text(g_hwnd_path);
        g_cb.set_game_path(path);
        refresh_ui();
      }
      return 0;
    }
    break;
  case WM_CTLCOLORSTATIC: {
    HDC hdc = (HDC)wp;
    HWND child = (HWND)lp;
    SetBkMode(hdc, TRANSPARENT);
    if (child == g_hwnd_message) {
      if (g_state.has_error) {
        SetTextColor(hdc, RGB(147, 70, 61));
        static HBRUSH err_brush = CreateSolidBrush(RGB(247, 233, 230));
        return (LRESULT)err_brush;
      } else {
        SetTextColor(hdc, RGB(53, 91, 114));
        static HBRUSH ok_brush = CreateSolidBrush(RGB(223, 234, 241));
        return (LRESULT)ok_brush;
      }
    }
    if (child == g_hwnd_title) {
      SetTextColor(hdc, RGB(22, 62, 88));
    }
    if (child == g_hwnd_version || child == g_hwnd_footer) {
      SetTextColor(hdc, RGB(99, 126, 144));
    }
    return (LRESULT)GetStockObject(WHITE_BRUSH);
  }
  case WM_CLOSE:
    if (g_state.busy)
      return 0;
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    // No fallible file IO may escape a Win32 window callback.
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── DPI awareness setup ─────────────────────────────────────────────────────
static void set_dpi_awareness() {
  // Try SetProcessDpiAwarenessContext (Win10 1703+)
  typedef BOOL(WINAPI * fn)(HANDLE);
  auto p = (fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
  if (p) {
    p((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    return;
  }
  // Fallback: SetProcessDPIAware
  typedef BOOL(WINAPI * fn2)();
  auto p2 = (fn2)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
  if (p2)
    p2();
}

static void detect_scale() {
  HDC hdc = GetDC(nullptr);
  int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
  ReleaseDC(nullptr, hdc);
  g_scale_num = dpi;
  g_scale_den = 96;
}

// ── Run UI ──────────────────────────────────────────────────────────────────

int run_ui(const AppState &initial, Callbacks cb, const std::string &report_path) {
  check_new_output(report_path);
#if defined(INSTALLER_TEST_DRIVER) || defined(MANAGER_TEST_DRIVER)
  // Owned GUI checks select a language in this child only; no user registry IO.
  wchar_t language[16]{};
  GetEnvironmentVariableW(L"ASTRO_FIXTURE_LANGUAGE", language, 16);
  g_language = std::wstring(language) == L"en" ? "en" : "zh-CN";
#else
  g_language = installer_i18n::load_language();
#endif
  g_state = initial;
  g_cb = cb;
  if (g_cb.refresh)
    g_cb.refresh();
  if (!report_path.empty() && g_cb.validate_report) g_cb.validate_report(g_state);
  g_report_path = report_path;
  g_smoke_test = !report_path.empty();

  set_dpi_awareness();
  detect_scale();
  if (g_smoke_test && g_smoke_dpi) {
    g_scale_num = g_smoke_dpi;
    g_scale_den = 96;
  }

  HINSTANCE hinst = GetModuleHandleW(nullptr);

  // Create fonts
  g_font = create_font(13);
  g_font_bold = create_font(13, true);
  g_font_small = create_font(11);

  // Load icon from embedded resource or file
  g_icon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
  if (!g_icon) {
    // Try loading from file
    g_icon = (HICON)LoadImageW(nullptr, L"installer.ico", IMAGE_ICON, 0, 0,
                               LR_LOADFROMFILE | LR_DEFAULTSIZE);
  }

  // Register window class
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = hinst;
  wc.hIcon = g_icon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"AstroneerModInstaller";
  RegisterClassExW(&wc);

  // Create main window
  int w = scale(600), h = scale(444);
  RECT rc = {0, 0, w, h};
  AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

  g_hwnd_main = CreateWindowExW(WS_EX_LEFT, L"AstroneerModInstaller", io::wide(tr("异星探险家 · Mod 安装器")).c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);

  if (!g_hwnd_main)
    return 1;

  // Set icon
  if (g_icon) {
    SendMessage(g_hwnd_main, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
    SendMessage(g_hwnd_main, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
  }

  ShowWindow(g_hwnd_main, SW_SHOW);
  UpdateWindow(g_hwnd_main);

  if (!g_smoke_test && g_state.start_uninstall && g_state.valid &&
      !g_state.running && !g_state.has_error)
    PostMessageW(g_hwnd_main, WM_COMMAND, ID_BTN_UNINSTALL, 0);

  // If smoke test, schedule auto-close
  if (g_smoke_test) {
    SetTimer(g_hwnd_main, 1, 1500, nullptr); // Close after 1.5s
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (msg.message == WM_TIMER && g_smoke_test) {
      if (!g_smoke_action.empty()) {
        if (g_action_stage == 0) {
          SendMessageW(g_hwnd_main, WM_COMMAND,
                       g_smoke_action == "install" ? ID_BTN_INSTALL : ID_BTN_UNINSTALL, 0);
          if (!g_hwnd_dlg) {
            g_operation_ok = false;
            g_action_stage = 3;
          } else {
            SendMessageW(g_hwnd_dlg, WM_COMMAND, ID_DLG_BTN_CANCEL, 0);
            g_operation_ok = !g_state.busy && g_state.pending_text.empty();
            SendMessageW(g_hwnd_main, WM_COMMAND,
                         g_smoke_action == "install" ? ID_BTN_INSTALL : ID_BTN_UNINSTALL, 0);
            if (g_state.requires_ack) {
              g_operation_ok =
                  g_operation_ok && !IsWindowEnabled(GetDlgItem(g_hwnd_dlg, ID_DLG_BTN_CONFIRM));
              SendMessageW(g_hwnd_dlg, WM_COMMAND, ID_DLG_BTN_CONFIRM, 0);
              g_operation_ok = g_operation_ok && !g_state.busy;
              SendMessageW(GetDlgItem(g_hwnd_dlg, ID_DLG_CHECK_ACK), BM_SETCHECK, BST_CHECKED, 0);
            }
            SendMessageW(g_hwnd_dlg, WM_COMMAND, ID_DLG_BTN_CONFIRM, 0);
            g_action_stage = 1;
            continue;
          }
        }
        if (g_state.busy)
          continue;
        if (g_action_stage == 1) {
          g_operation_ok = g_operation_ok && !g_state.has_error && !g_state.modules.empty() &&
                           g_state.modules[0].installed == (g_smoke_action == "install");
          g_action_stage = 2;
        }
      }
      measure_children(g_hwnd_main);
      g_state.pending_action = "uninstall";
      g_state.requires_ack = true;
      g_state.pending_text = "卸载后，大件物品的尺寸或挂接可能变化。请先取下特"
                             "殊插槽内的大件，保存并退出游戏。\r\n\r\n备份：" +
                             g_state.backup_path;
      show_confirmation();
      measure_children(g_hwnd_dlg);
      DestroyWindow(g_hwnd_dlg);
      DestroyWindow(g_hwnd_main);
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Cleanup
  if (g_worker.joinable())
    g_worker.join();
  if (g_font)
    DeleteObject(g_font);
  if (g_font_bold)
    DeleteObject(g_font_bold);
  if (g_font_small)
    DeleteObject(g_font_small);

  if (g_smoke_test) {
    const bool ok = g_geometry_ok && g_icon != nullptr && g_operation_ok;
    json::Value report = json::Object{
        {"ok", ok}, {"operation_ok", !g_smoke_action.empty() && g_operation_ok},
        {"operation_message", g_state.message}, {"operation_result", g_state.operation_result},
        {"backup_saves", g_state.backup_saves},
        {"controls", g_geometry}, {"dpi", g_scale_num},
        {"scope", "Native window/control geometry, not screenshot or interaction approval"},
        {"backend", "engine::Manager"}, {"game_path", g_state.game_path},
        {"logical_size", json::Array{600, 340}}, {"window_icon_loaded", g_icon != nullptr},
        {"scale", std::to_string(g_scale_num) + "/" + std::to_string(g_scale_den)},
        {"modules_count", static_cast<int>(g_state.modules.size())}, {"errors", json::Array{}}};
    const bool written = write_new_output(report, g_report_path, true, [&] {
      if (g_cb.validate_report) g_cb.validate_report(g_state);
    });
    std::cout << json::serialize(report) << std::endl;
    return !ok ? 1 : written ? 0 : 2;
  }
  return 0;
}

int smoke_test(const AppState &initial, Callbacks cb, const std::string &report_path) {
  return run_ui(initial, cb, report_path);
}

} // namespace ui
