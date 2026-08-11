#pragma once

#include "src/core/EngineAPI.h"

#include <stdexcept>
#include <string>

namespace core
{
class ENGINE_API WindowException final : public std::runtime_error
{
  public:
	explicit WindowException(const std::string &Diagnostic) : std::runtime_error(Diagnostic)
	{
	}
};

class ENGINE_API ContextException final : public std::runtime_error
{
  public:
	explicit ContextException(const std::string &Diagnostic) : std::runtime_error(Diagnostic)
	{
	}
};
} // namespace core
