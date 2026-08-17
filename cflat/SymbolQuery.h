#pragma once

#include "ArgParser.h"
#include <string>

int RunSymbolQuery(ArgParser& args, const std::string& runtimeDir, bool showProgress);
int RunSymbolDumpQuery(ArgParser& args, const std::string& runtimeDir, bool showProgress);
