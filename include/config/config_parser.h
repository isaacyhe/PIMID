#ifndef PIMID_CONFIG_CONFIG_PARSER_H
#define PIMID_CONFIG_CONFIG_PARSER_H

#include <string>
#include <map>
#include <vector>

// Forward declare YAML::Node to avoid exposing yaml-cpp in header
namespace YAML {
    class Node;
}

namespace pimid {
namespace config {

/**
 * Generic YAML configuration parser
 * Parses YAML files into a flat key-value map (1.12.26: ConfigManager, its former consumer, is removed)
 */
class ConfigParser {
public:
    ConfigParser();

    // Parse YAML file into flat key-value map
    // Keys are dot-separated paths (e.g., "memory.dram.frequency")
    bool parseFile(const std::string& filename,
                   std::map<std::string, std::string>& config);

    // Parse YAML string
    bool parseString(const std::string& yaml_content,
                     std::map<std::string, std::string>& config);

    // Convert map back to YAML string
    static std::string toYAML(const std::map<std::string, std::string>& config);

    // Convert map to JSON string
    static std::string toJSON(const std::map<std::string, std::string>& config);

    // Parse list from comma-separated string
    static std::vector<std::string> parseList(const std::string& list_str);

private:
    // Recursive node parsing
    bool parseNode(const YAML::Node& node,
                   const std::string& prefix,
                   std::map<std::string, std::string>& config);
};

} // namespace config
} // namespace pimid

#endif // PIMID_CONFIG_CONFIG_PARSER_H
