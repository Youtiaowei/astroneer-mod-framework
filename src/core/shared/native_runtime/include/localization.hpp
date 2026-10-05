#pragma once
#include <string>
#include <string_view>

namespace astro::i18n {
inline bool valid(std::string_view language) {
  return language == "zh-CN" || language == "en";
}
inline std::string text(std::string_view language, std::string_view chinese) {
  struct Message { std::string_view key, zh, en; };
  static constexpr Message messages[] = {
      {"mod.load_error", "加载失败，请使用安装器修复", "Load failed. Repair using the installer."},
      {"mod.runtime_error", "运行异常，已停用；请重新启动游戏", "Runtime failure. Disabled; restart the game."},
      {"mod.cleanup_error", "清理异常，回调已停用；请重新启动游戏", "Cleanup failed. Callbacks disabled; restart the game."},
      {"wide.range_template","吸收 / 挖掘范围：{value}×","Absorb / dig radius: {value}x"},
      {"wide.baseline","1× 为原版已开启宽口调节器的范围","1x is the native enabled Wide Mod radius"},
      {"wide.description","只扩大吸收 / 挖掘范围","Absorb / dig radius only"},
      {"wide.waiting_world","等待游戏就绪","Waiting for the game"},
      {"wide.disabled","范围 Mod 已关闭，倍率设置保留","Radius mod disabled; factor retained"},
      {"wide.equipment_required","需装上并开启宽口调节器","Equip and enable the Wide Mod"},
      {"wide.augment_off","请开启宽口调节器","Enable the Wide Mod"},
      {"wide.no_power","未获原版供电，使用原版范围","No native power; original radius applies"},
      {"wide.original_mode","铺土 / 平整使用原版范围","Add / flatten uses the original radius"},
      {"wide.ready","吸收 / 挖掘可生效","Ready for absorb / dig"},
      {"wide.invalid_value","倍率超出候选调节范围","Factor outside the candidate range"},
  };
  for (const auto &message : messages)
    if (chinese == message.key)
      return std::string(language == "en" ? message.en : message.zh);
  if (language != "en")
    return std::string(chinese);
  struct Entry { std::string_view zh, en; };
  static constexpr Entry entries[] = {
      {"MOD 管理", "Mods"}, {"打赏作者", "Support"},
      {"万能插槽", "Universal Slots"}, {"快捷键", "Hotkey"},
      {"平整星球", "Planet Flattening"},
      {"配合对齐调节器使用", "Use with Alignment Mod"},
      {"感谢你的支持", "Thank you for your support"},
      {"打赏方式待添加", "Support options are not available yet"},
      {"更多mod", "More Mods"}, {"打开爱发电", "Afdian"},
      {"将在浏览器中打开", "Opens in your browser"},
      {"已请求在浏览器中打开", "Browser launch requested"},
      {"无法打开，请使用卡片二维码", "Unable to open. Use the card's QR code."},
      {"无法打开，请访问 mod.cab", "Unable to open. Visit mod.cab."},
      {"尚未安装 Mod", "No mods installed"},
      {"卡片暂不可用，请重新安装 Mod 资源", "Card unavailable. Reinstall the mod assets."},
      {"加载失败，请使用安装器修复", "Load failed. Repair using the installer."},
      {"等待游戏就绪", "Waiting for the game"}, {"切换失败", "Toggle failed"},
      {"设置保存失败", "Could not save settings"},
      {"语言保存失败", "Could not save language"},
      {"已开启", "Enabled"}, {"已关闭", "Disabled"},
      {"允许大件放入背包、平台和设备的小插槽。关闭后，已有物品保持缩小并继续工作；取下恢复原尺寸。",
       "Fit large items into small backpack, platform and equipment slots. When disabled, attached items stay scaled down and retain their normal functions. Removing them restores their original size."},
  };
  for (const auto &entry : entries)
    if (entry.zh == chinese)
      return std::string(entry.en);
  return std::string(chinese); // Unknown diagnostics/identities are never rewritten.
}
inline std::string module_name(std::string_view language, std::string_view id,
                               const std::string &fallback) {
  if (id == "planet_flattening") return text(language, "平整星球");
  if (id == "wide_range") return language=="en"?"Wide Radius":"宽口范围";
  return id == "universal_slot" ? text(language, "万能插槽") : fallback;
}
} // namespace astro::i18n
