#pragma once
#include <string>
#include <vector>

/*
 * Namespaces whose declarations ALL name a std:: specialization. Binding such a signature is
 * deferred to first use, so these namespaces exist only if the deferral still registers them.
 * nsstd_mix is the control: one std-free declaration registered it even before the fix.
 */
namespace nsstd_param { inline int lens(const std::string& s) { return (int)s.size(); } }

namespace nsstd_ret { inline std::string makes(const char* p) { return std::string(p); } }

namespace nsstd_vec { inline int usevec(const std::vector<int>& v) { return (int)v.size() + 20; } }

namespace nsstd_outer { namespace inner {
    inline int lens(const std::string& s) { return (int)s.size() + 1; }
} }

namespace nsstd_mix { inline int plain(int v) { return v + 2; }
                      inline int lens(const std::string& s) { return (int)s.size() + 7; } }
