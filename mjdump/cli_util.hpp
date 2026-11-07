#pragma once

#include <Windows.h>

#include <jninfra/dump/class_filter.hpp>

#include <string>
#include <vector>

namespace mjdump {

/// @brief Shared command-line options for commands that target a JVM process.
///
/// The CLI keeps parsing deliberately small and predictable. More advanced
/// session configuration belongs in the in-process agent/pipe protocol rather
/// than in per-command ad hoc parsing.
struct TargetOptions
{
    DWORD        pid = 0;
    std::wstring dll_path;
};

/// @brief Parse `-p <pid>` / `--pid <pid>` from a command argument range.
/// @param argc Number of command arguments, including the subcommand name.
/// @param argv Command argument array, including the subcommand name.
/// @param pid Output process id.
/// @return true when a valid pid was found.
bool parse_pid_option(int argc, char* argv[], DWORD& pid);

/// @brief Return true when @p flag is present in a command argument range.
bool has_flag(int argc, char* argv[], const char* flag);

/// @brief Parse an unsigned integer option, returning @p default_value when absent.
unsigned long parse_ulong_option(int argc, char* argv[], const char* name,
                                 unsigned long default_value);

/// @brief Return true when `--group-by` is absent or set to none/package/module.
bool validate_group_by_option(int argc, char* argv[]);

/// @brief Parse `--group-by`, returning `none` when absent.
std::string parse_group_by_option(int argc, char* argv[]);

/// @brief Normalize dot/slash class CLI input to JVM internal class names.
std::string normalize_class_name_arg(const char* value);

/// @brief Convert a package CLI shortcut to a subtree include pattern.
std::string package_pattern_arg(const char* value);

/// @brief Parse common class filter flags from a command argument range.
///
/// Supported flags are `--include`, `--exclude`, `--package`, `--class`,
/// `--regex`/`--include-regex`, `--exclude-regex`, and `--excludeJdk`.
jninfra::dump::ClassNameFilterConfig parse_filter_options(int argc, char* argv[]);

/// @brief Print filter diagnostics in a CLI-friendly form.
/// @return true when diagnostics were printed.
bool print_filter_diagnostics(
    const std::vector<jninfra::dump::ClassNameFilterDiagnostic>& diagnostics);

/// @brief Convert a UTF-8/narrow command argument to a Windows wide string.
std::wstring widen(const char* text);

/// @brief Return a human-readable Windows error message.
std::string windows_error_message(DWORD error);

/// @brief Print a Windows API failure with GetLastError detail.
void print_last_error(const char* operation);

/// @brief Resolve a path beside the running `mjdump.exe`.
std::wstring path_next_to_exe(const wchar_t* file_name);

/// @brief Try to enable SeDebugPrivilege for remote-process operations.
///
/// This is best effort. Commands still work for same-user processes when the
/// privilege cannot be enabled.
void enable_debug_privilege();

/// @brief Check that the target process has the same WOW64 architecture state.
///
/// CreateRemoteThread + LoadLibrary requires matching bitness. The project
/// builds x64 by default, so this catches accidental injection into 32-bit JVMs.
bool process_bitness_matches(HANDLE process);

}   // namespace mjdump
