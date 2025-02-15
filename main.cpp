#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <optional>

namespace fs = std::filesystem;

/**
 * This structure represents a single ignore rule from a .gitignore file.
 * It captures:
 *   - pattern: the original pattern text (after trimming comments/spaces).
 *   - negate: true if the pattern starts with '!' (i.e., un-ignore).
 *   - directoryOnly: true if the pattern ends with '/' (only matches directories).
 *   - anchored: true if the pattern starts with '/' (pattern is relative to .gitignore's directory).
 *   - tokens: a tokenized version of the pattern for matching (splitting on slashes and interpreting '**').
 */
struct GitIgnoreRule {
    std::string pattern;
    bool negate = false;
    bool directoryOnly = false;
    bool anchored = false;
    
    // Split pattern into slash-separated components, taking into account '**'.
    // Example: "src/**/test/*.cpp" -> ["src", "**", "test", "*.cpp"]
    std::vector<std::string> tokens;
};

/**
 * Trim whitespace from left and right.
 */
static inline std::string trim(const std::string &s) {
    const char* ws = " \t\r\n";
    auto start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

/**
 * Split a string by delimiter, returning a vector of tokens.
 */
static std::vector<std::string> split(const std::string &str, char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    while (true) {
        auto pos = str.find(delimiter, start);
        if (pos == std::string::npos) {
            result.push_back(str.substr(start));
            break;
        } else {
            result.push_back(str.substr(start, pos - start));
            start = pos + 1;
        }
    }
    return result;
}

/**
 * Parse a single line from a .gitignore file into a GitIgnoreRule, if valid.
 * Returns std::nullopt if the line is empty or a comment line.
 */
std::optional<GitIgnoreRule> parseGitIgnoreLine(const std::string &line) {
    std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
        // comment or empty line
        return std::nullopt;
    }
    
    GitIgnoreRule rule;
    rule.pattern = trimmed;

    // Check for negate: patterns that start with '!'
    if (!trimmed.empty() && trimmed[0] == '!') {
        rule.negate = true;
        rule.pattern.erase(rule.pattern.begin()); // remove '!'
        // re-trim in case there's leftover space
        rule.pattern = trim(rule.pattern);
    }

    // Check for directoryOnly: patterns that end with '/'
    if (!rule.pattern.empty() && rule.pattern.back() == '/') {
        rule.directoryOnly = true;
        rule.pattern.pop_back(); // remove trailing slash
    }

    // Check for anchored: patterns that start with '/'
    if (!rule.pattern.empty() && rule.pattern.front() == '/') {
        rule.anchored = true;
        rule.pattern.erase(rule.pattern.begin()); // remove leading slash
    }

    // Now tokenize by splitting on '/'
    // Example: "foo/bar", "foo/*/bar", "foo/**/bar"
    rule.tokens = split(rule.pattern, '/');
    return rule;
}

/**
 * Parse the given .gitignore file, returning a vector of GitIgnoreRule.
 */
std::vector<GitIgnoreRule> parseGitIgnore(const fs::path &gitignorePath) {
    std::vector<GitIgnoreRule> rules;
    std::ifstream in(gitignorePath);
    if (!in.is_open()) {
        return rules; // no file or can't open
    }
    
    std::string line;
    while (std::getline(in, line)) {
        auto maybeRule = parseGitIgnoreLine(line);
        if (maybeRule.has_value()) {
            rules.push_back(maybeRule.value());
        }
    }
    return rules;
}

/**
 * Match a single path component (no slashes) against a pattern token,
 * supporting '*', '?', and full '**'.
 *   - A token like "foo" must match exactly "foo".
 *   - A token like "*.cpp" matches anything ending with ".cpp".
 *   - A token like "?" matches any single character except slash.
 *   - There's a special token "**" that we handle at a higher level,
 *     meaning "match zero or more directories."
 */
bool matchToken(const std::string &token, const std::string &component) {
    // If token is "**", that logic is handled at a higher level (for entire path segments).
    // So this function should never get "**".
    // But let's guard anyway:
    if (token == "**") {
        // This special case is handled outside. Return false here as a fallback.
        return false;
    }

    // We'll do a standard wildcard match ignoring slash.
    // Essentially a shell-glob match, but ignoring '/'. 
    // This is a simpler approach that covers '*', '?', etc.
    size_t t = 0, c = 0;
    size_t starPos = std::string::npos;
    size_t starBackup = 0;

    while (c < component.size()) {
        // If the pattern is still valid:
        if (t < token.size() && (token[t] == component[c] || token[t] == '?')) {
            t++;
            c++;
        }
        else if (t < token.size() && token[t] == '*') {
            starPos = t;
            starBackup = c;
            t++;
        }
        else if (starPos != std::string::npos) {
            // If we've seen a star, skip a character in component
            // and retry matching it
            t = starPos + 1;
            c = ++starBackup;
        }
        else {
            return false;
        }
    }
    // Consume any trailing stars in token
    while (t < token.size() && token[t] == '*') {
        t++;
    }
    return t == token.size();
}

/**
 * Given a file/directory path relative to the base where the .gitignore is interpreted,
 * and a single GitIgnoreRule, determine if the path **would** match that rule.
 *
 * This function implements the "double-star" directory handling:
 *  - "**" can match zero or more entire path components.
 *  - Patterns may be anchored or unanchored:
 *      - anchored => must match from the beginning of the relative path
 *      - unanchored => can match any subset of the path
 *
 * If the path is a directory, `isDir` = true.
 */
bool pathMatchesRule(const std::vector<std::string> &pathComponents,
                     bool isDir,
                     const GitIgnoreRule &rule)
{
    if (rule.directoryOnly && !isDir) {
        // The rule specifically matches directories only, but this is a file.
        return false;
    }

    // If the rule has no tokens (which is weird), skip.
    if (rule.tokens.empty()) {
        return false;
    }

    // C++20 recursive lambda using the lambda's parameter to refer to itself.
    auto matchFromIndex = [&](auto self, size_t pcStart) -> bool {
        size_t i = 0;        // index in rule.tokens
        size_t j = pcStart;  // index in pathComponents
        while (i < rule.tokens.size() && j < pathComponents.size()) {
            const auto &token = rule.tokens[i];
            
            if (token == "**") {
                // If '**' is the last token, then match everything.
                if (i == rule.tokens.size() - 1) {
                    return true;
                }
                // Otherwise, try matching '**' with zero or more components.
                for (size_t skip = 0; j + skip <= pathComponents.size(); skip++) {
                    if (self(self, j + skip)) {
                        return true;
                    }
                }
                return false;
            } else {
                // Normal token => must match exactly one component.
                if (!matchToken(token, pathComponents[j])) {
                    return false;
                }
                i++;
                j++;
            }
        }

        // If tokens remain, they must all be "**" to match empty trailing.
        if (i < rule.tokens.size()) {
            for (; i < rule.tokens.size(); i++) {
                if (rule.tokens[i] != "**") {
                    return false;
                }
            }
            return true;
        }
        else {
            // All tokens matched; valid only if all path components are consumed.
            return (j == pathComponents.size());
        }
    };

    if (rule.anchored) {
        // Must match from the beginning.
        return matchFromIndex(matchFromIndex, 0);
    } else {
        // Try matching starting at any component offset.
        for (size_t start = 0; start < pathComponents.size(); start++) {
            if (matchFromIndex(matchFromIndex, start)) {
                return true;
            }
        }
        return false;
    }
}

/**
 * The core logic: given a list of GitIgnoreRule objects that have been accumulated
 * from parent directories and the current directory, determine if a path is ignored.
 *
 * In Git, the "last matching rule" takes precedence. If that rule is a negate rule,
 * the file is not ignored; if that rule is a normal rule, the file is ignored.
 *
 * param fullPath: The absolute path to the file/directory in question.
 * param baseDir:  The directory to which these .gitignore rules are anchored.
 * param rules:    All the rules from baseDir and parent directories.
 * param isDir:    Whether the path is a directory (affects directory-only rules).
 */
bool isIgnored(const fs::path &fullPath,
               const fs::path &baseDir,
               const std::vector<GitIgnoreRule> &rules,
               bool isDir)
{
    // Compute the relative path from baseDir to fullPath.
    fs::path rel = fs::relative(fullPath, baseDir);

    // Convert the relative path to a vector of components.
    std::vector<std::string> components;
    for (const auto &part : rel) {
        components.push_back(part.string());
    }

    bool ignored = false; // default is not ignored
    // The last matching rule wins.
    for (auto &rule : rules) {
        if (pathMatchesRule(components, isDir, rule)) {
            ignored = !rule.negate;
        }
    }
    return ignored;
}

/**
 * Recursively gather rules from .gitignore files in parent directories
 * (up to the root or a boundary). This is how Git typically merges .gitignore:
 *   - .gitignore in the root folder
 *   - subfolder .gitignore appended
 *   - deeper subfolder .gitignore appended
 * and so on.
 *
 * For the sake of demonstration, we gather them all into one vector in order
 * from top to bottom. Then the last match in that combined vector has precedence.
 */
std::vector<GitIgnoreRule> gatherGitIgnoreRulesRecursively(const fs::path &dir) {
    // Collect all parent directories.
    std::vector<fs::path> dirs;
    fs::path current = fs::canonical(dir);
    while (true) {
        dirs.push_back(current);
        if (current.has_parent_path() && current.parent_path() != current) {
            current = current.parent_path();
        } else {
            break;
        }
    }
    std::reverse(dirs.begin(), dirs.end());

    std::vector<GitIgnoreRule> allRules;
    for (auto &d : dirs) {
        fs::path gi = d / ".gitignore";
        if (fs::exists(gi) && fs::is_regular_file(gi)) {
            auto localRules = parseGitIgnore(gi);
            allRules.insert(allRules.end(), localRules.begin(), localRules.end());
        }
    }
    return allRules;
}

/**
 * Recursively process the directory, respecting .gitignore rules.
 *
 * param dir: the directory to traverse
 * param out: a stream to which we write the combined output
 * param rules: the combined .gitignore rules for this directory
 */
void processDirectory(const fs::path &dir,
                      std::ofstream &out,
                      const std::vector<GitIgnoreRule> &rules)
{
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (!entry.exists()) continue;

        fs::path path = entry.path();
        bool directory = fs::is_directory(path);
        if (path.filename() == ".gitignore") {
            continue;
        }

        // Check if ignored.
        if (isIgnored(path, dir, rules, directory)) {
            continue;
        }

        if (directory) {
            auto subDirRules = rules;
            fs::path subGitIgnore = path / ".gitignore";
            if (fs::exists(subGitIgnore)) {
                auto localRules = parseGitIgnore(subGitIgnore);
                subDirRules.insert(subDirRules.end(), localRules.begin(), localRules.end());
            }
            processDirectory(path, out, subDirRules);
        } else {
            // Write the file with a commented header.
            out << "# File: " << path.string() << "\n\n";
            std::ifstream inFile(path, std::ios::binary);
            if (inFile) {
                out << inFile.rdbuf();
                out << "\n\n";
            } else {
                std::cerr << "Failed to open file: " << path << "\n";
            }
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <directory_path>\n";
        return 1;
    }
    
    fs::path directoryPath(argv[1]);
    if (!fs::exists(directoryPath) || !fs::is_directory(directoryPath)) {
        std::cerr << "Invalid directory: " << directoryPath << "\n";
        return 1;
    }

    std::ofstream outFile("combined.txt", std::ios::out | std::ios::binary);
    if (!outFile) {
        std::cerr << "Failed to create output file combined.txt\n";
        return 1;
    }

    auto topLevelRules = gatherGitIgnoreRulesRecursively(directoryPath);
    processDirectory(directoryPath, outFile, topLevelRules);

    std::cout << "Files have been combined into combined.txt\n";
    return 0;
}
