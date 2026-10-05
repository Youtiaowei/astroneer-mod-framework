#include "settings.hpp"
#include "localization.hpp"
#include "strict_json.hpp"
#include <cmath>
namespace astro {
namespace {
bool parameters_valid(const std::map<std::string,double>& values) {
  if(values.size()>32)return false;
  for(const auto& [id,value]:values)
    if(id.empty()||id.size()>64||id[0]<'a'||id[0]>'z'
       ||id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos
       ||!std::isfinite(value)||value<0||value>1000000)return false;
  return true;
}
}
bool SettingsManager::modules_valid(const std::map<std::string, bool> &values) {
  if (values.size() > 32 || !values.contains("universal_slot"))
    return false;
  for (auto &[id, value] : values) {
    if (id.empty() || id.size() > 64 || id[0] < 'a' || id[0] > 'z')
      return false;
    for (char c : id)
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
        return false;
  }
  return true;
}
std::optional<Preferences>
SettingsManager::parse_journal(const std::string &text) {
  try {
    auto v = json::parse(text, 4096);
    auto schema = v.at("schema").number(), seq = v.at("seq").number();
    if ((schema != 1 && schema != 2) || seq < 1 || seq >= 1000000000 ||
        std::floor(seq) != seq)
      return {};
    Preferences p;
    p.schema = int(schema);
    p.seq = int(seq);
    p.enabled = v.at("enabled").boolean();
    p.visible = v.at("visible").boolean();
    if (v.record().contains("language")) {
      const auto &raw = v.at("language");
      // Unsupported languages fall back without discarding gameplay preferences.
      if (std::holds_alternative<std::string>(raw.data) && i18n::valid(raw.string()))
        p.language = raw.string();
    }
    if (schema == 1)
      p.modules = {{"universal_slot", p.enabled}};
    else
      for (auto &[id, value] : v.at("modules").record())
        p.modules.emplace(id, value.boolean());
    if(v.record().contains("parameters"))
      for(const auto& [id,value]:v.at("parameters").record())p.parameters.emplace(id,value.number());
    if(!parameters_valid(p.parameters))return {};
    if (!modules_valid(p.modules) ||
        p.modules.at("universal_slot") != p.enabled)
      return {};
    return p;
  } catch (const std::exception &) {
    return {};
  }
}
SettingsManager::SettingsManager(Files &files, bool enabled, bool visible)
    : files_(files), path_a_("ingame-settings-a.json"),
      path_b_("ingame-settings-b.json") {
  current_.enabled = enabled;
  current_.visible = visible;
  current_.modules = {{"universal_slot", enabled}};
  // Journal schema is the original source contract. A migrated ui-preferences
  // journal is accepted only when it meets the same complete schema.
  for (auto path : {std::string("ui-preferences.json"), path_a_, path_b_})
    if (auto raw = files_.read(path, 4096))
      if (auto parsed = parse_journal(*raw);
          parsed && parsed->seq > current_.seq)
        current_ = *parsed;
}
Preferences SettingsManager::read() const { return current_; }
bool SettingsManager::save(const Preferences &value) {
  if (!i18n::valid(value.language) || !modules_valid(value.modules) || !parameters_valid(value.parameters) ||
      value.modules.at("universal_slot") != value.enabled) {
    last_error_ = "Invalid Mod preferences";
    return false;
  }
  auto row = value;
  row.schema = 2;
  row.seq = current_.seq + 1;
  if (row.seq >= 1000000000) {
    last_error_ = "Preference journal exhausted";
    return false;
  }
  Value::Record modules;
  for (auto &[id, on] : row.modules)
    modules.emplace(id, on);
  Value::Record parameters;
  for(const auto& [id,number]:row.parameters)parameters.emplace(id,number);
  try {
    auto encoded = json::encode(record({{"schema", 2},
                                        {"seq", row.seq},
                                        {"enabled", row.enabled},
                                        {"visible", row.visible},
                                        {"language", row.language},
                                        {"modules", modules},{"parameters", parameters}}),
                                4096);
    auto path = row.seq % 2 ? path_a_ : path_b_;
    if (!files_.write_atomic(path, encoded))
      throw std::runtime_error("Preference write failed");
    auto raw = files_.read(path, 4096);
    auto verified = raw ? parse_journal(*raw) : std::nullopt;
    if (!verified || verified->seq != row.seq ||
        verified->enabled != row.enabled || verified->visible != row.visible ||
        verified->modules != row.modules || verified->language != row.language || verified->parameters != row.parameters)
      throw std::runtime_error("Preference readback failed");
    current_ = row;
    last_error_.clear();
    return true;
  } catch (const std::exception &ex) {
    last_error_ = ex.what();
    return false;
  }
}
} // namespace astro
