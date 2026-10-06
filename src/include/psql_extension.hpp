#pragma once

#include "duckdb.hpp"

namespace duckdb {

class PsqlExtension : public Extension {
public:
  void Load(ExtensionLoader &loader) override;
  std::string Name() override { return "psql"; }
};

} // namespace duckdb
