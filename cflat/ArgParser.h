#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <optional>

class ArgParser
{
public:
	// Register a boolean flag (e.g. --verbose)
	void addFlag(const std::string& name, char shortName, const std::string& description)
	{
		m_flags.push_back({ name, shortName, description });
		m_nameKind[name] = ArgKind::Flag;
		if (shortName) m_shortToName[shortName] = name;
	}

	// Register an option that takes a value (e.g. --output foo.ll)
	void addOption(const std::string& name, char shortName, const std::string& description, const std::string& defaultValue = "")
	{
		m_options.push_back({ name, shortName, description, defaultValue });
		m_nameKind[name] = ArgKind::Option;
		if (shortName) m_shortToName[shortName] = name;
		if (!defaultValue.empty())
			m_optionValues[name] = defaultValue;
	}

	// Register a repeatable option that takes a value and may appear multiple times
	// (e.g. --c-include a --c-include b). Each occurrence is appended.
	void addMultiOption(const std::string& name, char shortName, const std::string& description)
	{
		m_multiOptions.push_back({ name, shortName, description, "" });
		m_nameKind[name] = ArgKind::MultiOption;
		if (shortName) m_shortToName[shortName] = name;
	}

	// Register a named positional argument (for usage display only)
	void addPositional(const std::string& name, const std::string& description)
	{
		m_positionals.push_back({ name, description });
	}

	// Returns the error message from the most recent failed parse() or
	// getXthreadScanLevel() call. Empty if no error has occurred.
	const std::string& getError() const { return m_errorMsg; }

	bool parse(int argc, char* argv[])
	{
		m_errorMsg.clear();
		m_program = argv[0];
		for (int i = 1; i < argc; ++i)
		{
			std::string arg = argv[i];

			// A bare "--" ends option parsing: everything after it is collected verbatim as
			// program arguments (passthrough), forwarded to the JIT'd program under --run.
			// This keeps program args from being mistaken for compiler flags or source files.
			if (arg == "--")
			{
				for (int j = i + 1; j < argc; ++j)
					m_passthrough.push_back(argv[j]);
				break;
			}

			// clang-compatible spelling for --asan. Accepted as a no-op-cost alias so
			// existing -fsanitize=address habits/build scripts work; sets the "asan" flag.
			if (arg == "-fsanitize=address" || arg == "--fsanitize=address")
			{
				m_flagValues["asan"] = true;
				continue;
			}

			if (arg == "--help" || arg == "-h")
			{
				printUsage();
				return false;
			}

			if (arg == "--version")
			{
				m_showVersion = true;
				return true;
			}

			// Dispatch a resolved long name: set flag, consume next token for option/multi-option,
			// or set m_errorMsg and return false. display is the token as it appeared on the
			// command line (e.g. "--verbose" or "-v") used only in error messages.
			auto applyArg = [&](const std::string& name, const std::string& display) -> bool {
				if (isFlag(name))
				{
					m_flagValues[name] = true;
				}
				else if (isOption(name))
				{
					if (i + 1 >= argc) { m_errorMsg = "Error: " + display + " requires a value."; return false; }
					m_optionValues[name] = argv[++i];
				}
				else if (isMultiOption(name))
				{
					if (i + 1 >= argc) { m_errorMsg = "Error: " + display + " requires a value."; return false; }
					m_multiOptionValues[name].push_back(argv[++i]);
				}
				else
				{
					m_errorMsg = "Error: unknown argument '" + display + "'.";
					return false;
				}
				return true;
			};

			if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-')
			{
				std::string name = arg.substr(2);
				if (!applyArg(name, "--" + name)) return false;
			}
			else if (arg.size() >= 2 && arg[0] == '-')
			{
				if (arg.size() == 2)
				{
					char s = arg[1];
					std::string name = resolveShort(s);
					if (name.empty())
					{
						m_errorMsg = std::string("Error: unknown argument '-") + s + "'.";
						return false;
					}
					if (!applyArg(name, std::string("-") + s)) return false;
				}
				else
				{
					// Allow single-dash long flags like -O2, -O1
					std::string name = arg.substr(1);
					if (isFlag(name))
						m_flagValues[name] = true;
					else
					{
						m_errorMsg = "Error: unknown argument '" + arg + "'.";
						return false;
					}
				}
			}
			else
			{
				m_positionalValues.push_back(arg);
			}
		}
		return true;
	}

	bool showVersion() const { return m_showVersion; }

	bool hasFlag(const std::string& name) const
	{
		auto it = m_flagValues.find(name);
		return it != m_flagValues.end() && it->second;
	}

	std::optional<std::string> getOption(const std::string& name) const
	{
		auto it = m_optionValues.find(name);
		if (it != m_optionValues.end())
			return it->second;
		return std::nullopt;
	}

	// Return all values supplied for a repeatable option (empty if none).
	std::vector<std::string> getMultiOption(const std::string& name) const
	{
		auto it = m_multiOptionValues.find(name);
		if (it != m_multiOptionValues.end())
			return it->second;
		return {};
	}

	std::optional<std::string> getPositional(size_t index) const
	{
		if (index < m_positionalValues.size())
			return m_positionalValues[index];
		return std::nullopt;
	}

	size_t positionalCount() const { return m_positionalValues.size(); }

	// Program arguments collected after a bare "--" (empty if none). Meaningful only under
	// --run, where they become argv[1..] of the JIT'd program.
	const std::vector<std::string>& passthrough() const { return m_passthrough; }

	int getOptimizationLevel() const
	{
		if (hasFlag("O2")) return 2;
		if (hasFlag("O1")) return 1;
		return 0;
	}

	// Cross-thread sharing scan level (information-gathering survey). Consumes the value
	// of the `--xthread-scan N` option (N in 1..3). 0 = silent (default, fully
	// backward-compatible); higher levels widen the set of memory reported as escaping
	// across a thread-spawn boundary. An out-of-range value is reported and treated as 0.
	int getXthreadScanLevel() const
	{
		auto val = getOption("xthread-scan");
		if (!val || val->empty())
			return 0;
		int level = 0;
		try { level = std::stoi(*val); }
		catch (...) { level = -1; }
		if (level < 1 || level > 3)
		{
			m_errorMsg = "Error: --xthread-scan requires a level of 1, 2, or 3 (got '" + *val + "').";
			return 0;
		}
		return level;
	}

	void printUsage() const
	{
		std::cout << "Usage: " << m_program;
		for (const auto& p : m_positionals)
			std::cout << std::format(" <{}>", p.name);
		if (!m_flags.empty() || !m_options.empty() || !m_multiOptions.empty())
			std::cout << " [options]";
		std::cout << "\n";

		if (!m_positionals.empty())
		{
			std::cout << "\nArguments:\n";
			for (const auto& p : m_positionals)
				std::cout << std::format("  {}\t\t{}\n", p.name, p.description);
		}

		if (!m_flags.empty() || !m_options.empty() || !m_multiOptions.empty())
		{
			std::cout << "\nOptions:\n";
			std::cout << "  -h, --help\t\tShow this help message\n";
			for (const auto& f : m_flags)
				std::cout << std::format("  -{}, --{}\t\t{}\n", f.shortName, f.name, f.description);
			for (const auto& o : m_options)
			{
				std::cout << std::format("  -{}, --{} <value>\t{}", o.shortName, o.name, o.description);
				if (!o.defaultValue.empty())
					std::cout << std::format(" (default: {})", o.defaultValue);
				std::cout << "\n";
			}
			for (const auto& o : m_multiOptions)
				std::cout << std::format("  -{}, --{} <value>\t{} (repeatable)\n", o.shortName, o.name, o.description);
		}
	}

private:
	enum class ArgKind { Flag, Option, MultiOption };
	struct FlagDef   { std::string name; char shortName; std::string description; };
	struct OptionDef { std::string name; char shortName; std::string description; std::string defaultValue; };
	struct PositionalDef { std::string name; std::string description; };

	std::vector<FlagDef>        m_flags;
	std::vector<OptionDef>      m_options;
	std::vector<OptionDef>      m_multiOptions;
	std::vector<PositionalDef>  m_positionals;

	std::unordered_map<std::string, ArgKind> m_nameKind;
	std::unordered_map<char, std::string>    m_shortToName;

	std::unordered_map<std::string, bool>                     m_flagValues;
	std::unordered_map<std::string, std::string>              m_optionValues;
	std::unordered_map<std::string, std::vector<std::string>> m_multiOptionValues;
	std::vector<std::string>                                  m_positionalValues;
	std::vector<std::string>                                  m_passthrough;
	std::string                                  m_program;
	mutable std::string                          m_errorMsg;
	bool                                         m_showVersion = false;

	bool isFlag(const std::string& name) const
	{
		auto it = m_nameKind.find(name);
		return it != m_nameKind.end() && it->second == ArgKind::Flag;
	}

	bool isOption(const std::string& name) const
	{
		auto it = m_nameKind.find(name);
		return it != m_nameKind.end() && it->second == ArgKind::Option;
	}

	bool isMultiOption(const std::string& name) const
	{
		auto it = m_nameKind.find(name);
		return it != m_nameKind.end() && it->second == ArgKind::MultiOption;
	}

	std::string resolveShort(char s) const
	{
		auto it = m_shortToName.find(s);
		return it != m_shortToName.end() ? it->second : std::string{};
	}
};
