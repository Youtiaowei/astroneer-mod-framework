#pragma once
#include "controls.hpp"
#include "module_contract.hpp"
#include "number_contract.hpp"
#include <memory>
#include <string>
namespace astro {
enum class ModuleCodeKind { dynamic_library, static_fixture };
// Created by the authenticated module loader, or explicitly by owned fixtures.
// Registration does not itself call a plugin factory or activate its behavior.
struct ModuleRegistration {
  CatalogEntry catalog;
  module::Api api{};
  std::shared_ptr<void> code_lifetime;
  std::string version;
  std::string load_error;
  ModuleCodeKind code_kind = ModuleCodeKind::dynamic_library;
  // Authenticated localized display text. catalog.name/description carry the
  // Chinese strings; these keep the English pair signed by the record instead
  // of silently dropping it. Presentation integration is a later shared-menu
  // gate and is unchanged here.
  std::string name_en, description_en;
  module::NumberApi number{};
};
} // namespace astro
