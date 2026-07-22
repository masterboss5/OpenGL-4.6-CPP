#pragma once

#include <stdexcept>
#include <string>

namespace core
{
class WindowException final : public std::runtime_error
{
  public:
	explicit WindowException(const std::string &Diagnostic) : std::runtime_error(Diagnostic)
	{
	}
};

class ContextException final : public std::runtime_error
{
  public:
	explicit ContextException(const std::string &Diagnostic) : std::runtime_error(Diagnostic)
	{
	}
};
} // namespace core
