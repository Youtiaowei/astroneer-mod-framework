#pragma once
#include <map>
#include <set>
#include <string>
namespace catalog {
inline const std::map<std::string, std::set<std::string>> &legacy_hashes() {
  static const std::map<std::string, std::set<std::string>> v = {
      {"dev_probes.lua", {"0f055a211a3159c1cd8fd9c90e3b60c1317184128704a6a1332a03c42b3ae47c"}},
      {"ingame_controls.lua",
       {"b2f37946ca24e3d85b9cfd092b7cdb985572c30a3c78e3d01f785882e5b94794",
        "d30cedab9dbde2c6a7b58b656b3740e18ab88bfc3da6052145d13fb78d2f475e"}},
      {"ingame_hud.lua", {"6d17442148b5e7ee0fea4e173658c091d19b691988985ec64466b037be57bcf1"}},
      {"ingame_panel.lua", {"2c838f60aea568f3d9ca2ccde1402ffb15879b1d9d2c8365360aa84bf2a754b6"}},
      {"ingame_runtime.lua", {"f3e13c76633db3e26b6a131e2b6bea9fb10caef3e9f0181f519928890902d21e"}},
      {"ingame_settings.lua", {"52224ab24c31d36588fd1de2eae4fb34a78c411511d87534ac6a793bb7dcf442"}},
      {"json.lua", {"0eaccda57fabc0330736de25f45cf589821a42b5e0fe02e4e3125f7dc0bf2b7e"}},
      {"main.lua",
       {"de77da15c67aea0401a5c80e386802341cb8ba783c1d9084066867f211a55768",
        "df51f939a37579240a33ec48d9deccf6d863e110ffdba571c231e40c3938ab95"}},
      {"side_panel_layout.lua",
       {"6c620df5cf9c63ab9184baf5d8c52c8edb322382d9779162e8ebf34b2cc41cd2"}},
      {"support_card_config.lua",
       {"e58303f4b6f76876b2b9ccd6d5faaecffdf61e049f105936afbb1b6b67d08a7f"}},
      {"support_card_projection.lua",
       {"d396ef6c24103e68bfc53ae4bddaf4516abba3316faf6965b85467c1123586b4"}},
      {"support_card_state.lua",
       {"4fbfc87c4d50c84ef1309d7a5beaeae71f70568d58ce91f69fdf974bf81551ca"}},
      {"support_card_view.lua",
       {"2f5024ffa4fce00bcddd468ad8ff597c1bfeedb225610e2cc53830b442868c30"}},
      {"universal_slot.lua", {"1a9574a8932e745a587807da1a06de43ea19bac923aabb5b16365b5d69afd148"}},
  };
  return v;
}
} // namespace catalog
