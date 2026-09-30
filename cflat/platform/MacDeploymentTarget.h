#pragma once

#include <cstdlib>
#include <string>
#if defined(__APPLE__)
#include <sys/utsname.h>
#include <llvm/Support/VersionTuple.h>
#include <llvm/TargetParser/Triple.h>
#endif

namespace cflat::platform
{
    /*
     * The deployment target clang++'s Darwin driver picks without -mmacos-version-min:
     * MACOSX_DEPLOYMENT_TARGET when set (malformed -> error), else the host triple's
     * darwin<kernel> version mapped by llvm::Triple::getMacOSXVersion (Darwin 25 -> 26.0.0).
     */
    struct MacDeployment
    {
        std::string version;
        std::string error;   // non-empty: MACOSX_DEPLOYMENT_TARGET is not a valid version
    };

    inline const MacDeployment& MacDeploymentInfo()
    {
        static const MacDeployment info = []() -> MacDeployment {
#if defined(__APPLE__)
            auto valid = [](const std::string& value) {
                unsigned parts[3] = {};
                size_t component = 0;
                bool digit = false;
                for (char c : value)
                {
                    if (c == '.' && digit && component < 2)
                    {
                        ++component;
                        digit = false;
                    }
                    else if (c >= '0' && c <= '9')
                    {
                        parts[component] = parts[component] * 10 + (c - '0');
                        if (parts[component] > 65535) return false;
                        digit = true;
                    }
                    else return false;
                }
                return digit && parts[0] >= 10 && parts[1] < 100 && parts[2] < 100;
            };
            MacDeployment result;
            if (const char* env = std::getenv("MACOSX_DEPLOYMENT_TARGET"); env != nullptr && *env)
            {
                if (valid(env)) return {env, {}};
                result.error = std::string("invalid version number in 'MACOSX_DEPLOYMENT_TARGET=")
                    + env + "'";
            }
            struct utsname host = {};
            if (uname(&host) == 0)
            {
                llvm::VersionTuple version;
                if (llvm::Triple(std::string("arm64-apple-darwin") + host.release)
                        .getMacOSXVersion(version))
                {
                    result.version = std::to_string(version.getMajor()) + "."
                        + std::to_string(version.getMinor().value_or(0)) + "."
                        + std::to_string(version.getSubminor().value_or(0));
                    if (valid(result.version)) return result;
                }
            }
            result.version = "11.0.0";
            return result;
#else
            return {"11.0.0", {}};
#endif
        }();
        return info;
    }

    inline const std::string& MacDeploymentVersion()
    {
        return MacDeploymentInfo().version;
    }
}
