#pragma once

#include "src/types.h"

#include <iostream>
#include <source_location>
#include <syncstream>
#include <utility>

#ifndef OPENGL_ENABLE_LOGGING
#define OPENGL_ENABLE_LOGGING 1
#endif

#ifndef OPENGL_ENABLE_LOG_COLORS
#define OPENGL_ENABLE_LOG_COLORS 1
#endif

namespace util
{
enum class LogLevel : uint8
{
	Information,
	Warning,
	Error
};

struct LogStyle final
{
	string_view Label;
	string_view Color;
};

[[nodiscard]] inline constexpr LogStyle GetLogStyle(const LogLevel Level) noexcept
{
#if OPENGL_ENABLE_LOG_COLORS
	switch (Level)
	{
	case LogLevel::Information:
		return {"INFO", "\033[37;4m"};
	case LogLevel::Warning:
		return {"WARN", "\033[33;4m"};
	case LogLevel::Error:
		return {"ERROR", "\033[31;4m"};
	}
#else
	switch (Level)
	{
	case LogLevel::Information:
		return {"INFO", {}};
	case LogLevel::Warning:
		return {"WARN", {}};
	case LogLevel::Error:
		return {"ERROR", {}};
	}
#endif
	return {"UNKNOWN", {}};
}

template <typename MessageType>
void WriteLog(const LogLevel Level, MessageType &&Message, const std::source_location Location = std::source_location::current())
{
	const LogStyle Style = GetLogStyle(Level);
	std::osyncstream Output(std::cerr);
	Output << Style.Color << '[' << Style.Label << "][" << Location.file_name() << ": " << Location.function_name() << ": "
		   << Location.line() << "] " << std::forward<MessageType>(Message);
#if OPENGL_ENABLE_LOG_COLORS
	Output << "\033[0m";
#endif
	Output << '\n';
}
} // namespace util

#if OPENGL_ENABLE_LOGGING
#define LOG_INFO(Message) ::util::WriteLog(::util::LogLevel::Information, (Message))
#define LOG_WARN(Message) ::util::WriteLog(::util::LogLevel::Warning, (Message))
#define LOG_ERROR(Message) ::util::WriteLog(::util::LogLevel::Error, (Message))
#else
#define LOG_INFO(Message) ((void)0)
#define LOG_WARN(Message) ((void)0)
#define LOG_ERROR(Message) ((void)0)
#endif
