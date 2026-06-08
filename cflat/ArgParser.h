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
	}

	// Register an option that takes a value (e.g. --output foo.ll)
	void addOption(const std::string& name, char shortName, const std::string& description, const std::string& defaultValue = "")
	{
		m_options.push_back({ name, shortName, description, defaultValue });
		if (!defaultValue.empty())
			m_optionValues[name] = defaultValue;
	}

	// Register a repeatable option that takes a value and may appear multiple times
	// (e.g. --c-include a --c-include b). Each occurrence is appended.
	void addMultiOption(const std::string& name, char shortName, const std::string& description)
	{
		m_multiOptions.push_back({ name, shortName, description, "" });
	}

	// Register a named positional argument (for usage display only)
	void addPositional(const std::string& name, const std::string& description)
	{
		m_positionals.push_back({ name, description });
	}

	bool parse(int argc, char* argv[])
	{
		m_program = argv[0];
		for (int i = 1; i < argc; ++i)
		{
			std::string arg = argv[i];

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

			if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-')
			{
				std::string name = arg.substr(2);
				if (isFlag(name))
				{
					m_flagValues[name] = true;
				}
				else if (isOption(name))
				{
					if (i + 1 >= argc)
					{
						std::cerr << "Error: --" << name << " requires a value.\n";
						return false;
					}
					m_optionValues[name] = argv[++i];
				}
				else if (isMultiOption(name))
				{
					if (i + 1 >= argc)
					{
						std::cerr << "Error: --" << name << " requires a value.\n";
						return false;
					}
					m_multiOptionValues[name].push_back(argv[++i]);
				}
				else
				{
					std::cerr << "Error: unknown argument '--" << name << "'.\n";
					return false;
				}
			}
			else if (arg.size() >= 2 && arg[0] == '-')
			{
				if (arg.size() == 2)
				{
					char s = arg[1];
					std::string name = resolveShort(s);
					if (name.empty())
					{
						std::cerr << "Error: unknown argument '-" << s << "'.\n";
						return false;
					}
					if (isFlag(name))
					{
						m_flagValues[name] = true;
					}
					else if (isOption(name))
					{
						if (i + 1 >= argc)
						{
							std::cerr << "Error: -" << s << " requires a value.\n";
							return false;
						}
						m_optionValues[name] = argv[++i];
					}
					else if (isMultiOption(name))
					{
						if (i + 1 >= argc)
						{
							std::cerr << "Error: -" << s << " requires a value.\n";
							return false;
						}
						m_multiOptionValues[name].push_back(argv[++i]);
					}
				}
				else
				{
					// Allow single-dash long flags like -O2, -O1
					std::string name = arg.substr(1);
					if (isFlag(name))
					{
						m_flagValues[name] = true;
					}
					else
					{
						std::cerr << "Error: unknown argument '" << arg << "'.\n";
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
			std::cerr << "Error: --xthread-scan requires a level of 1, 2, or 3 (got '" << *val << "').\n";
			return 0;
		}
		return level;
	}

	void printUsage() const
	{
		std::cout << "Usage: " << m_program;
		for (const auto& p : m_positionals)
			std::cout << " <" << p.name << ">";
		if (!m_flags.empty() || !m_options.empty() || !m_multiOptions.empty())
			std::cout << " [options]";
		std::cout << "\n";

		if (!m_positionals.empty())
		{
			std::cout << "\nArguments:\n";
			for (const auto& p : m_positionals)
				std::cout << "  " << p.name << "\t\t" << p.description << "\n";
		}

		if (!m_flags.empty() || !m_options.empty() || !m_multiOptions.empty())
		{
			std::cout << "\nOptions:\n";
			std::cout << "  -h, --help\t\tShow this help message\n";
			for (const auto& f : m_flags)
				std::cout << "  -" << f.shortName << ", --" << f.name << "\t\t" << f.description << "\n";
			for (const auto& o : m_options)
			{
				std::cout << "  -" << o.shortName << ", --" << o.name << " <value>\t" << o.description;
				if (!o.defaultValue.empty())
					std::cout << " (default: " << o.defaultValue << ")";
				std::cout << "\n";
			}
			for (const auto& o : m_multiOptions)
				std::cout << "  -" << o.shortName << ", --" << o.name << " <value>\t" << o.description << " (repeatable)\n";
		}
	}

private:
	struct FlagDef   { std::string name; char shortName; std::string description; };
	struct OptionDef { std::string name; char shortName; std::string description; std::string defaultValue; };
	struct PositionalDef { std::string name; std::string description; };

	std::vector<FlagDef>        m_flags;
	std::vector<OptionDef>      m_options;
	std::vector<OptionDef>      m_multiOptions;
	std::vector<PositionalDef>  m_positionals;

	std::unordered_map<std::string, bool>                     m_flagValues;
	std::unordered_map<std::string, std::string>              m_optionValues;
	std::unordered_map<std::string, std::vector<std::string>> m_multiOptionValues;
	std::vector<std::string>                                  m_positionalValues;
	std::string                                  m_program;
	bool                                         m_showVersion = false;

	bool isFlag(const std::string& name) const
	{
		for (const auto& f : m_flags)
			if (f.name == name) return true;
		return false;
	}

	bool isOption(const std::string& name) const
	{
		for (const auto& o : m_options)
			if (o.name == name) return true;
		return false;
	}

	bool isMultiOption(const std::string& name) const
	{
		for (const auto& o : m_multiOptions)
			if (o.name == name) return true;
		return false;
	}

	std::string resolveShort(char s) const
	{
		for (const auto& f : m_flags)
			if (f.shortName == s) return f.name;
		for (const auto& o : m_options)
			if (o.shortName == s) return o.name;
		for (const auto& o : m_multiOptions)
			if (o.shortName == s) return o.name;
		return "";
	}
};
