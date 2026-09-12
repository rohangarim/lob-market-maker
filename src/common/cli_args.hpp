#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lob {

// Minimal `--flag value` / `--flag=value` command-line parser. Deliberately
// small and dependency-free: every executable in this project takes at
// most a handful of flags, so a full CLI-parsing library would be more
// machinery than the problem needs.
class CliArgs {
public:
    CliArgs(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.rfind("--", 0) != 0) continue;
            arg = arg.substr(2);
            auto eq = arg.find('=');
            if (eq != std::string::npos) {
                values_[arg.substr(0, eq)] = arg.substr(eq + 1);
            } else if (i + 1 < argc && std::string_view(argv[i + 1]).rfind("--", 0) != 0) {
                values_[arg] = argv[++i];
            } else {
                values_[arg] = "true";
            }
        }
    }

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const {
        auto it = values_.find(std::string(key));
        if (it == values_.end()) return std::nullopt;
        return it->second;
    }

    [[nodiscard]] std::string get_or(std::string_view key, std::string default_value) const {
        return get(key).value_or(std::move(default_value));
    }

    [[nodiscard]] double get_double_or(std::string_view key, double default_value) const {
        auto v = get(key);
        if (!v) return default_value;
        return std::stod(*v);
    }

    [[nodiscard]] long get_long_or(std::string_view key, long default_value) const {
        auto v = get(key);
        if (!v) return default_value;
        return std::stol(*v);
    }

    [[nodiscard]] bool has(std::string_view key) const { return values_.contains(std::string(key)); }

private:
    std::unordered_map<std::string, std::string> values_;
};

}  // namespace lob
