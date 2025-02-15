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
    if (trimmed.empty() || trimmed[0] == '#')
        return std::nullopt;
    
    GitIgnoreRule rule;
    rule.pattern = trimmed;

    if (!trimmed.empty() && trimmed[0] == '!') {
        rule.negate = true;
        rule.pattern.erase(rule.pattern.begin());
        rule.pattern = trim(rule.pattern);
    }

    if (!rule.pattern.empty() && rule.pattern.back() == '/') {
        rule.directoryOnly = true;
        rule.pattern.pop_back();
    }

    if (!rule.pattern.empty() && rule.pattern.front() == '/') {
        rule.anchored = true;
        rule.pattern.erase(rule.pattern.begin());
    }

    rule.tokens = split(rule.pattern, '/');
    return rule;
}

/**
 * Parse the given .gitignore file, returning a vector of GitIgnoreRule.
 */
std::vector<GitIgnoreRule> parseGitIgnore(const fs::path &gitignorePath) {
    std::vector<GitIgnoreRule> rules;
    std::ifstream in(gitignorePath);
    if (!in.is_open())
        return rules;
    
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
 */
bool matchToken(const std::string &token, const std::string &component) {
    if (token == "**") {
        return false;
    }

    size_t t = 0, c = 0;
    size_t starPos = std::string::npos;
    size_t starBackup = 0;

    while (c < component.size()) {
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
            t = starPos + 1;
            c = ++starBackup;
        }
        else {
            return false;
        }
    }
    while (t < token.size() && token[t] == '*') {
        t++;
    }
    return t == token.size();
}

/**
 * Given a file/directory path (represented as a vector of components) and a single GitIgnoreRule,
 * determine if the path would match that rule.
 */
bool pathMatchesRule(const std::vector<std::string> &pathComponents,
                     bool isDir,
                     const GitIgnoreRule &rule)
{
    if (rule.directoryOnly && !isDir)
        return false;
    if (rule.tokens.empty())
        return false;

    auto matchFromIndex = [&](auto self, size_t pcStart) -> bool {
        size_t i = 0;
        size_t j = pcStart;
        while (i < rule.tokens.size() && j < pathComponents.size()) {
            const auto &token = rule.tokens[i];
            if (token == "**") {
                if (i == rule.tokens.size() - 1)
                    return true;
                for (size_t skip = 0; j + skip <= pathComponents.size(); skip++) {
                    if (self(self, j + skip))
                        return true;
                }
                return false;
            } else {
                if (!matchToken(token, pathComponents[j]))
                    return false;
                i++;
                j++;
            }
        }
        if (i < rule.tokens.size()) {
            for (; i < rule.tokens.size(); i++) {
                if (rule.tokens[i] != "**")
                    return false;
            }
            return true;
        } else {
            return (j == pathComponents.size());
        }
    };

    if (rule.anchored)
        return matchFromIndex(matchFromIndex, 0);
    else {
        for (size_t start = 0; start < pathComponents.size(); start++) {
            if (matchFromIndex(matchFromIndex, start))
                return true;
        }
        return false;
    }
}

/**
 * Determine if a path should be ignored by checking against accumulated GitIgnoreRules.
 * The relative path is computed from 'baseDir' (which remains constant for the whole run).
 */
bool isIgnored(const fs::path &fullPath,
               const fs::path &baseDir,
               const std::vector<GitIgnoreRule> &rules,
               bool isDir)
{
    fs::path rel = fs::relative(fullPath, baseDir);
    std::vector<std::string> components;
    for (const auto &part : rel) {
        components.push_back(part.string());
    }

    bool ignored = false;
    for (auto &rule : rules) {
        if (pathMatchesRule(components, isDir, rule)) {
            ignored = !rule.negate;
        }
    }
    return ignored;
}

/**
 * Recursively gather rules from .gitignore files in parent directories.
 * (Rules are gathered from the original base directory.)
 */
std::vector<GitIgnoreRule> gatherGitIgnoreRulesRecursively(const fs::path &dir) {
    std::vector<fs::path> dirs;
    fs::path current = fs::canonical(dir);
    while (true) {
        dirs.push_back(current);
        if (current.has_parent_path() && current.parent_path() != current)
            current = current.parent_path();
        else
            break;
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
 * Heuristic function to determine if a file is binary.
 * Reads the first 512 bytes and checks for a high ratio of non-printable characters.
 */
bool isBinaryFile(const fs::path &filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in)
        return false;
    const size_t sampleSize = 512;
    char buffer[sampleSize];
    in.read(buffer, sampleSize);
    std::streamsize bytesRead = in.gcount();
    if (bytesRead == 0)
        return false; // Empty files are treated as text.
    
    int nonPrintable = 0;
    for (int i = 0; i < bytesRead; i++) {
        unsigned char c = static_cast<unsigned char>(buffer[i]);
        if (!((c >= 32 && c <= 126) || c == 9 || c == 10 || c == 13)) {
            nonPrintable++;
        }
    }
    double ratio = static_cast<double>(nonPrintable) / bytesRead;
    return ratio > 0.30;
}

/**
 * Recursively process the directory, respecting .gitignore rules.
 * The parameter 'baseDir' remains the original directory against which all relative
 * paths (for .gitignore matching) are computed.
 */
void processDirectory(const fs::path &dir,
                      std::ofstream &out,
                      const std::vector<GitIgnoreRule> &rules,
                      const fs::path &baseDir)
{
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (!entry.exists())
            continue;
        fs::path path = entry.path();
        bool directory = fs::is_directory(path);

        // Skip .gitignore files and the output file "combined.txt".
        if (path.filename() == ".gitignore")
            continue;
        if (path.filename() == "combined.txt")
            continue;

        // Use the fixed baseDir for ignore matching.
        if (isIgnored(path, baseDir, rules, directory))
            continue;

        if (directory) {
            // Create a new set of rules that includes any .gitignore from this subdirectory.
            auto subDirRules = rules;
            fs::path subGitIgnore = path / ".gitignore";
            if (fs::exists(subGitIgnore)) {
                auto localRules = parseGitIgnore(subGitIgnore);
                subDirRules.insert(subDirRules.end(), localRules.begin(), localRules.end());
            }
            // Recurse but always pass along the original baseDir.
            processDirectory(path, out, subDirRules, baseDir);
        } else {
            if (isBinaryFile(path)) {
                std::cerr << "Skipping binary file: " << path << "\n";
                continue;
            }
            out << "# File: " << path.string() << "\n\n";
            std::ifstream inFile(path);
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

    // Gather rules from the entire directory tree (using the top-level directory as base).
    auto topLevelRules = gatherGitIgnoreRulesRecursively(directoryPath);
    processDirectory(directoryPath, outFile, topLevelRules, directoryPath);

    std::cout << "Files have been combined into combined.txt\n";
    return 0;
}
