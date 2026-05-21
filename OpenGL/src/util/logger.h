#pragma once
#include <iostream>

#define OGL_GLSL_INFO_LOGGING_SIZE 2048

#define LOG
#define COLOR_LOGS

// --- Reset ---
#define ANSI_RESET          "\033[0m"

#ifdef COLOR_LOGS
	// --- Regular Colors ---
	#define ANSI_BLACK          "\033[30m"
	#define ANSI_RED            "\033[31m"
	#define ANSI_GREEN          "\033[32m"
	#define ANSI_YELLOW         "\033[33m"
	#define ANSI_BLUE           "\033[34m"
	#define ANSI_MAGENTA        "\033[35m"
	#define ANSI_CYAN           "\033[36m"
	#define ANSI_WHITE          "\033[37m"

	// --- Bright Colors ---
	#define ANSI_BRIGHT_BLACK   "\033[90m"
	#define ANSI_BRIGHT_RED     "\033[91m"
	#define ANSI_BRIGHT_GREEN   "\033[92m"
	#define ANSI_BRIGHT_YELLOW  "\033[93m"
	#define ANSI_BRIGHT_BLUE    "\033[94m"
	#define ANSI_BRIGHT_MAGENTA "\033[95m"
	#define ANSI_BRIGHT_CYAN    "\033[96m"
	#define ANSI_BRIGHT_WHITE   "\033[97m"

	// --- Bold Colors ---
	#define ANSI_BOLD_BLACK     "\033[1;30m"
	#define ANSI_BOLD_RED       "\033[1;31m"
	#define ANSI_BOLD_GREEN     "\033[1;32m"
	#define ANSI_BOLD_YELLOW    "\033[1;33m"
	#define ANSI_BOLD_BLUE      "\033[1;34m"
	#define ANSI_BOLD_MAGENTA   "\033[1;35m"
	#define ANSI_BOLD_CYAN      "\033[1;36m"
	#define ANSI_BOLD_WHITE     "\033[1;37m"

	// --- Background Colors ---
	#define ANSI_BG_BLACK       "\033[40m"
	#define ANSI_BG_RED         "\033[41m"
	#define ANSI_BG_GREEN       "\033[42m"
	#define ANSI_BG_YELLOW      "\033[43m"
	#define ANSI_BG_BLUE        "\033[44m"
	#define ANSI_BG_MAGENTA     "\033[45m"
	#define ANSI_BG_CYAN        "\033[46m"
	#define ANSI_BG_WHITE       "\033[47m"

	// --- Styles ---
	#define ANSI_BOLD           "\033[1m"
	#define ANSI_UNDERLINE      "\033[4m"
	#define ANSI_ITALIC         "\033[3m"
#else
	// --- Regular Colors ---
	#define ANSI_BLACK          ""
	#define ANSI_RED            ""
	#define ANSI_GREEN          ""
	#define ANSI_YELLOW         ""
	#define ANSI_BLUE           ""
	#define ANSI_MAGENTA        ""
	#define ANSI_CYAN           ""
	#define ANSI_WHITE          ""

	// --- Bright Colors ---
	#define ANSI_BRIGHT_BLACK   ""
	#define ANSI_BRIGHT_RED     ""
	#define ANSI_BRIGHT_GREEN   ""
	#define ANSI_BRIGHT_YELLOW  ""
	#define ANSI_BRIGHT_BLUE    ""
	#define ANSI_BRIGHT_MAGENTA ""
	#define ANSI_BRIGHT_CYAN    ""
	#define ANSI_BRIGHT_WHITE   ""

	// --- Bold Colors ---
	#define ANSI_BOLD_BLACK     ""
	#define ANSI_BOLD_RED       ""
	#define ANSI_BOLD_GREEN     ""
	#define ANSI_BOLD_YELLOW    ""
	#define ANSI_BOLD_BLUE      ""
	#define ANSI_BOLD_MAGENTA   ""
	#define ANSI_BOLD_CYAN      ""
	#define ANSI_BOLD_WHITE     ""

	// --- Background Colors ---
	#define ANSI_BG_BLACK       ""
	#define ANSI_BG_RED         ""
	#define ANSI_BG_GREEN       ""
	#define ANSI_BG_YELLOW      ""
	#define ANSI_BG_BLUE        ""
	#define ANSI_BG_MAGENTA     ""
	#define ANSI_BG_CYAN        ""
	#define ANSI_BG_WHITE       ""

	// --- Styles ---
	#define ANSI_BOLD           ""
	#define ANSI_UNDERLINE      ""
	#define ANSI_ITALIC         ""
#endif

#ifdef LOG
	#define LOG_INFO(message) \
		std::cerr \
		<< ANSI_WHITE \
		<< ANSI_UNDERLINE \
		<< "[INFO]" \
		<< "[" << __FILE__ \
		<< ": " \
		<< __func__ \
		<< ": " \
		<< __LINE__ \
		<< "] " \
		<< message \
		<< ANSI_RESET \
		<< std::endl;

	#define LOG_WARN(message) \
		std::cerr \
		<< ANSI_YELLOW \
		<< ANSI_UNDERLINE \
		<< "[WARN]" \
		<< "[" << __FILE__ \
		<< ": " \
		<< __func__ \
		<< ": " \
		<< __LINE__ \
		<< "] " \
		<< message \
		<< ANSI_RESET \
		<< std::endl;

	#define LOG_ERROR(message) \
		std::cerr \
		<< ANSI_RED \
		<< ANSI_UNDERLINE \
		<< "[ERROR]" \
		<< "[" << __FILE__ \
		<< ": " \
		<< __func__ \
		<< ": " \
		<< __LINE__ \
		<< "] " \
		<< message \
		<< ANSI_RESET \
		<< std::endl;
#else
	#define LOG_INFO(message)
	#define LOG_WARN(message)
	#define LOG_ERROR(message)
#endif