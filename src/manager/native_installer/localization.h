#pragma once
#include <string>
#include <string_view>
#include <windows.h>

// Display-only localization. CLI JSON, file names, module IDs, engine errors and
// installation transactions retain their canonical bytes.
namespace installer_i18n {
inline constexpr wchar_t registry_key[] = L"Software\\Youtiaowei\\AstroneerModInstaller";
inline bool valid(std::string_view id) { return id == "zh-CN" || id == "en"; }
inline std::string load_language(const wchar_t *key = registry_key) {
  wchar_t value[16]{};
  DWORD bytes = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, key, L"Language", RRF_RT_REG_SZ, nullptr,
                   value, &bytes) != ERROR_SUCCESS)
    return "zh-CN";
  return std::wstring_view(value) == L"en" ? "en" : "zh-CN";
}
inline bool save_language(std::string_view id, const wchar_t *key = registry_key) {
  if (!valid(id)) return false;
  HKEY handle = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &handle, nullptr) != ERROR_SUCCESS)
    return false;
  const wchar_t *value = id == "en" ? L"en" : L"zh-CN";
  const auto bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
  const auto result = RegSetValueExW(handle, L"Language", 0, REG_SZ,
                                    reinterpret_cast<const BYTE *>(value), bytes);
  RegCloseKey(handle);
  return result == ERROR_SUCCESS && load_language(key) == id;
}
inline std::string text(std::string_view language, const std::string &source) {
  if (language != "en") return source;
  // Translate the established leading status independently from the appended
  // bilingual library diagnostics. Paths/ids in the tail retain original bytes.
  const auto line_end = source.find("\r\n");
  if (line_end != source.npos)
    return text(language, source.substr(0, line_end)) + source.substr(line_end);
  struct Entry { const char *zh, *en; };
  static constexpr Entry exact[] = {
      {"异星探险家 · MOD", "ASTRONEER · MODS"},
      {"异星探险家 · Mod 安装器", "ASTRONEER · Mod Installer"},
      {"语言", "Language"}, {"目录", "Folder"}, {"选择", "Browse"}, {"刷新", "Refresh"},
      {"备份目录", "Backups"}, {"加入 QQ 群", "QQ Group"}, {"卸载所选", "Uninstall"},
      {"同时备份存档（可选）", "Also back up saved games (optional)"},
      {"安装 / 更新所选", "Install / Update"}, {"取消", "Cancel"},
      {"仅安装菜单", "Install Menu Only"}, {"卸载空菜单", "Remove Empty Menu"},
      {"平整星球", "Planet Flattening"},
      {"配合对齐调节器使用", "Use with Alignment Mod"},
      {"共享菜单：已安装", "Shared menu: Installed"},
      {"共享菜单：未安装（安装 Mod 时自动补装）", "Shared menu: Not installed (added with a Mod)"},
      {"共享菜单：未安装（请先安装公共框架包）", "Shared menu: Not installed (install common framework first)"},
      {"仍有已安装 Mod，不能单独卸载共享菜单。", "Installed Mods still require the shared menu. It cannot be removed separately."},
      {"尚未安装共享菜单。", "The shared menu is not installed."},
      {"请先安装公共框架包，再安装这个 Mod。", "Install the common framework package before this Mod."},
      {"轻量包资源缺失，请先完整解压整个压缩包，再运行安装程序。", "Resources are missing. Extract the complete ZIP before running the installer."},
      {"轻量包资源目录缺失，请重新完整解压。", "A resource directory is missing. Extract the complete ZIP again."},
      {"轻量包资源文件缺失或正在被修改，请重新完整解压。", "A resource file is missing or being modified. Extract the complete ZIP again."},
      {"轻量包资源校验失败，请重新下载并完整解压。", "Resource verification failed. Download and extract the complete ZIP again."},
      {"停用", "Disable"}, {"启用", "Enable"},
      {"本安装包没有可安装的已选模块。", "This package does not offer any selected modules."},
      {"请先勾选已安装模块。", "Select an installed module first."},
      {"确认并执行", "Confirm"}, {"确认卸载与存档风险", "Confirm Uninstall and Save Risk"},
      {"确认安装 / 更新", "Confirm Install / Update"},
      {"我已了解存档风险，确认卸载所选功能", "I understand the save risk and confirm uninstalling the selected features."},
      {"我确认备份并接管已识别的旧版安装", "I approve backing up and taking over the identified legacy installation."},
      {"离线安装 · 无需常驻 · 卸载前先取下特殊插槽内的大件，并保存退出游戏。",
       "Offline · No background app · Before uninstalling, remove large items, then save and exit."},
      {"万能插槽", "Universal Slots"}, {"未安装", "Not installed"},
      {"大件放入背包、平台和设备的小插槽。游戏中按 F8 或打开 ESC 菜单切换。",
       "Fit large items into small backpack, platform and equipment slots. Toggle with F8 or the ESC menu."},
      {"正在备份、写入并核对文件。请勿启动游戏。", "Backing up, writing and verifying files. Do not start the game."},
      {"请选择 ASTRONEER 游戏目录。", "Select your ASTRONEER game folder."},
      {"请保存并退出游戏后重试。", "Save and exit the game, then try again."},
      {"已核对游戏目录。安装器无需常驻运行。", "Game folder verified. The installer does not need to stay open."},
      {"检测到文件异常，操作会重新核对；请先检查备份。", "File issues detected. Check your backups; operations will verify the files again."},
      {"请先勾选功能模块。", "Select at least one feature first."},
      {"操作后读回发现文件异常", "File verification failed after the operation"},
      {"临时文件清理提醒", "Temporary File Cleanup Notice"}, {"安装器错误", "Installer Error"},
      {"语言设置保存失败，已保留原语言。", "Could not save the language. The previous language is unchanged."},
      {"另一个安装器正在操作这个游戏目录。", "Another installer is working on this game folder."},
      {"游戏正在运行。请保存并退出游戏。", "The game is running. Save and exit first."},
      {"备份必须放在游戏目录和安装包目录外。", "Backups must be outside the game and payload folders."},
      {"安装记录无效；未修改游戏。", "Invalid installation record; the game was not modified."},
      {"安装记录包含非本产品文件。", "The installation record contains files not owned by this product."},
      {"其他加载器布局或自定义入口，拒绝覆盖。", "A different loader layout or custom entry point was found. Overwrite refused."},
      {"已有加载器不完整。", "The existing loader is incomplete."},
      {"共享加载器配置与已验证环境不同。", "The shared loader configuration differs from the verified configuration."},
      {"旧版接管需要明确确认。", "Taking over a legacy installation requires explicit approval."},
      {"请选择有效模块。", "Select a valid feature."},
      {"请确认存档风险：卸载后尺寸和挂接可能变化。", "Acknowledge the save risk: item size or attachments may change after uninstalling."},
      {"请选择已管理模块。", "Select an installed, managed feature."},
      {"保留用户原有 UE4SS。", "Your existing UE4SS installation was retained."},
      {"保留仍被其他模块使用的共享加载器。", "The shared loader was retained because other modules still use it."},
      {"备份目录已被占用。请使用新的备份位置。", "The backup folder already exists. Use a new backup location."},
      {"游戏已启动，取消并回滚。", "The game started. The operation was cancelled and rolled back."},
      {"未能安全清理，未执行强制删除。", "Safe cleanup was not possible. No forced deletion was performed."},
  };
  for (const auto &entry : exact)
    if (source == entry.zh) return entry.en;
  // Only translate a recognized leading message. Never replace substrings in
  // the remaining path, identifier or verbatim diagnostic details.
  static constexpr Entry prefixes[] = {
      {"已安装 ", "Installed "}, {"未安装 ", "Not installed "}, {"可更新 ", "Update "},
      {"无法打开：", "Could not open: "},
      {"操作完成。备份：", "Completed. Backup: "},
      {"旧版文件未知或被修改：", "Unknown or modified legacy file: "},
      {"已安装文件被修改：", "Installed file was modified: "},
      {"存在未受管理的文件：", "Unmanaged file exists: "},
      {"文件已被修改，未删除：", "Modified file was not deleted: "},
      {"计划目录被文件占用：", "A file occupies a planned directory: "},
      {"计划目录在保护交接时变化：", "A planned directory changed during protection handoff: "},
      {"无法确认新建目录的归属，已取消操作。目录：", "Cannot establish ownership of a new directory. Operation cancelled. Directory: "},
      {"备份路径不在本次事务目录内：", "Backup path is outside this transaction: "},
      {"备份内部目录已被占用，取消操作：", "A backup subdirectory already exists. Operation cancelled: "},
      {"文件已提交，事务日志后处理未完成：", "Files were committed, but transaction journal finalization did not complete: "},
      {"备份或日志失败；未修改游戏：", "Backup or journal failed; the game is unchanged: "},
      {"备份失败；未修改游戏：", "Backup failed; the game is unchanged: "},
      {"目录保留（无法证明所有权或非空）：", "Directory retained (ownership unproven or not empty): "},
      {"文件卸载已提交，目录清理失败：", "File uninstall committed, but directory cleanup failed: "},
      {"目标文件在备份后变化：", "Target file changed after backup: "},
      {"计划新建目录已被占用，取消安装：", "A planned new directory already exists. Installation cancelled: "},
      {"目标父目录不在已保护计划中：", "Target parent directory is outside the protected plan: "},
      {"目录清理未完成：", "Directory cleanup did not complete: "},
      {"目录回滚保护或准备失败：", "Directory rollback protection or preparation failed: "},
      {"回滚日志写入失败。请勿启动游戏。备份：", "Rollback journal write failed. DO NOT START THE GAME. Backup: "},
      {"操作失败，已回滚文件。备份：", "Operation failed; files were rolled back. Backup: "},
      {"操作失败且回滚未完成。请勿启动游戏。备份：", "Operation and rollback failed. DO NOT START THE GAME. Backup: "},
      {"文件卸载已提交，目录清理未完成：", "File uninstall committed, but directory cleanup did not complete: "},
      {"不支持符号链接或目录联接：", "Symbolic links and directory junctions are not supported: "},
      {"未知目录：", "Unknown directory: "}, {"未知文件：", "Unknown file: "},
      {"轻量包资源文件缺失或正在被修改，请重新完整解压。", "A resource file is missing or being modified. Extract the complete ZIP again."},
      {"轻量包资源校验失败，请重新下载并完整解压。", "Resource verification failed. Download and extract the complete ZIP again."},
  };
  for (const auto &entry : prefixes)
    if (source.starts_with(entry.zh))
      return std::string(entry.en) + source.substr(std::string_view(entry.zh).size());
  return source;
}
inline std::string confirmation(std::string_view language, const std::string &action,
                                const std::string &backup, bool legacy, bool backup_saves = false) {
  const bool removing = action == "uninstall" || action == "uninstall-menu";
  const bool menu_only = action == "install-menu" || action == "uninstall-menu";
  std::string message;
  if (language == "en") {
    message = std::string(removing ? "Uninstall" : "Install / update") +
        (menu_only ? " the shared menu only.\r\n\r\n" : " the selected modules.\r\n\r\n") +
        "Mod files, configuration and installation records will be backed up automatically. "
        "Do not start the game during this operation.\r\n" +
        (backup_saves ? "Selected: also copy saves in the local default save folder.\r\n"
                      : "Not selected: saved games will not be copied or modified.\r\n") +
        "Backup: " + backup;
    if (legacy && removing)
      message += "\r\n\r\nRemove only verified legacy Mod files; no upgrade or new Mod installation is required. Settings and preferences are retained.";
    else if (legacy)
      message += "\r\n\r\nTake over the identified legacy files, migrate feature preferences and remove the legacy business Lua.";
    if (removing)
      message += "\r\n\r\nItem sizes or attachments may change after uninstalling. Remove large items from special slots, then save and exit the game first.";
  } else {
    message = "即将" + std::string(removing ? "卸载" : "安装 / 更新") +
        (menu_only ? "共享菜单（不安装 Mod）。\r\n\r\n" : "所选模块。\r\n\r\n") +
        "自动备份将改动的 Mod 文件、配置和安装记录；操作时请勿启动游戏。\r\n" +
        (backup_saves ? "已勾选：同时复制本机默认目录中的存档。\r\n"
                      : "未勾选：本次不备份存档，也不会修改存档。\r\n") + "备份：" + backup;
    if (legacy && removing)
      message += "\r\n\r\n仅卸载已核对的旧版 Mod 文件，不必先升级或安装新 Mod；保留设置和偏好。";
    else if (legacy)
      message += "\r\n\r\n接管已识别的旧版文件，迁移开关偏好，并移除旧版业务 Lua。";
    if (removing)
      message += "\r\n\r\n卸载后大件物品尺寸或挂接可能变化。请先取下特殊插槽内的大件，保存并退出游戏。";
  }
  return message;
}
} // namespace installer_i18n
