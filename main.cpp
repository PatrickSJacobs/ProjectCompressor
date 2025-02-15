#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <regex>
#include <sstream>
#include <optional>
#include <algorithm>

namespace fs = std::filesystem;

// A structure representing a single .gitignore rule.
struct GitIgnoreRule {
    std::regex patternRegex; // The pattern converted to a regex.
    bool negate = false;     // True if the rule starts with '!'
    bool directoryOnly = false; // True if the pattern ends with '/'
    bool anchored = false;      // True if the pattern starts with '/'
    std::string originalPattern; // The original pattern text.
};

// Utility: trim whitespace.
std::string trim(const std::string &s) {
    const char* ws = " \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

/**
 * Convert a .gitignore pattern into a regex string.
 * This implementation handles:
 *   - Leading '/' (anchored) by prefixing the regex with '^'
 *   - Trailing '/' (directoryOnly) by forcing a match to the end.
 *   - '*' matches any characters except '/'
 *   - '**' matches any characters (including '/')
 *   - '?' matches any single character except '/'
 *
 * This conversion is a best‐effort approximation and does not cover all edge cases.
 */
std::string patternToRegex(const std::string &pattern, bool anchored) {
    std::ostringstream oss;
    if (anchored)
        oss << "^";

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '*') {
            // Check for a double star.
            if (i + 1 < pattern.size() && pattern[i+1] == '*') {
                oss << ".*";
                ++i;
            } else {
                oss << "[^/]*";
            }
        } else if (c == '?') {
            oss << "[^/]";
        } else if (c == '.') {
            oss << "\\.";
        } else if (c == '+') {
            oss << "\\+";
        } else if (c == '(' || c == ')' || c == '|' || c == '^' || c == '$' || c == '{' || c == '}' || c == '[' || c == ']') {
            oss << "\\" << c;
        } else {
            oss << c;
        }
    }
    // If the pattern was for a directory, ensure it matches a trailing slash or end.
    // (Git’s behavior is more nuanced, but here we force end-of-string.)
    // You might want to modify this if needed.
    if (!pattern.empty() && pattern.back() == '/')
        oss << ".*";

    return oss.str();
}

/**
 * Parse a single line of a .gitignore file into a GitIgnoreRule.
 * Returns std::nullopt for blank lines or comments.
 */
std::optional<GitIgnoreRule> parseGitIgnoreLine(const std::string &line) {
    std::string trimmedLine = trim(line);
    if (trimmedLine.empty() || trimmedLine[0] == '#')
        return std::nullopt;

    GitIgnoreRule rule;
    rule.originalPattern = trimmedLine;

    // Check for negation.
    if (trimmedLine[0] == '!') {
        rule.negate = true;
        trimmedLine = trim(trimmedLine.substr(1));
    }

    // Check for a trailing slash.
    if (!trimmedLine.empty() && trimmedLine.back() == '/') {
        rule.directoryOnly = true;
    }

    // Check for anchored rule (pattern starts with '/')
    if (!trimmedLine.empty() && trimmedLine[0] == '/') {
        rule.anchored = true;
        trimmedLine = trimmedLine.substr(1); // remove the leading slash
    }

    std::string regexStr = patternToRegex(trimmedLine, rule.anchored);
    // For our purposes, match the entire string.
    regexStr += "$";
    try {
        rule.patternRegex = std::regex(regexStr, std::regex::ECMAScript);
    } catch (std::regex_error &e) {
        std::cerr << "Regex error for pattern \"" << trimmedLine << "\": " << e.what() << "\n";
        return std::nullopt;
    }
    return rule;
}

/**
 * Parse a .gitignore file and return a vector of rules.
 */
std::vector<GitIgnoreRule> parseGitIgnore(const fs::path &gitignorePath) {
    std::vector<GitIgnoreRule> rules;
    std::ifstream file(gitignorePath);
    if (!file.is_open())
        return rules;
    std::string line;
    while (std::getline(file, line)) {
        auto ruleOpt = parseGitIgnoreLine(line);
        if (ruleOpt.has_value()) {
            rules.push_back(ruleOpt.value());
        }
    }
    return rules;
}

/**
 * Determine if the given relative path (with '/' as separator) matches a single rule.
 */
bool matchesRule(const GitIgnoreRule &rule, const std::string &relPath, bool isDir) {
    // If rule is directory-only but this is not a directory, it does not match.
    if (rule.directoryOnly && !isDir)
        return false;
    return std::regex_match(relPath, rule.patternRegex);
}

/**
 * Given a list of GitIgnoreRule objects (ordered in the order they appear),
 * determine if the file with the given relative path should be ignored.
 *
 * Git’s behavior is that the last matching rule wins.
 */
bool isIgnored(const std::vector<GitIgnoreRule> &rules, const fs::path &baseDir, const fs::path &filePath) {
    // Compute the relative path from baseDir, using '/' as separator.
    fs::path rel = fs::relative(filePath, baseDir);
    std::string relPath = rel.generic_string(); // always uses '/'
    bool ignored = false;
    for (const auto &rule : rules) {
        if (matchesRule(rule, relPath, fs::is_directory(filePath))) {
            ignored = !rule.negate; // last match wins
        }
    }
    return ignored;
}

/**
 * Recursively gather .gitignore rules from the given directory and its parents.
 * (For a fully correct implementation you would also need to merge rules from nested .gitignore files.)
 */
std::vector<GitIgnoreRule> gatherGitIgnoreRules(const fs::path &startDir) {
    std::vector<GitIgnoreRule> rules;
    fs::path current = fs::canonical(startDir);
    while (true) {
        fs::path gitignoreFile = current / ".gitignore";
        if (fs::exists(gitignoreFile) && fs::is_regular_file(gitignoreFile)) {
            auto fileRules = parseGitIgnore(gitignoreFile);
            // Append the rules (Git applies .gitignore files in order from the root downward)
            rules.insert(rules.end(), fileRules.begin(), fileRules.end());
        }
        if (!current.has_parent_path() || current == current.parent_path())
            break;
        current = current.parent_path();
    }
    return rules;
}

/**
 * A heuristic to check if a file is binary.
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
        return false;
    int nonPrintable = 0;
    for (int i = 0; i < bytesRead; ++i) {
        unsigned char c = static_cast<unsigned char>(buffer[i]);
        if (!((c >= 32 && c <= 126) || c == 9 || c == 10 || c == 13))
            nonPrintable++;
    }
    return (static_cast<double>(nonPrintable) / bytesRead) > 0.30;
}

/**
 * Recursively process the directory and write text file contents into combined.txt.
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
        if (path.filename() == ".gitignore" || path.filename() == "combined.txt")
            continue;
        if (isIgnored(rules, baseDir, path))
            continue;
        if (fs::is_directory(path)) {
            processDirectory(path, out, rules, baseDir);
        } else {
            if (isBinaryFile(path)) {
                std::cerr << "Skipping binary file: " << path << "\n";
                continue;
            }
            out << "# File: " << path.string() << "\n\n";
            std::ifstream inFile(path);
            if (inFile)
                out << inFile.rdbuf() << "\n\n";
            else
                std::cerr << "Failed to open file: " << path << "\n";
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <directory_path>\n";
        return 1;
    }
    
    fs::path targetDir(argv[1]);
    if (!fs::exists(targetDir) || !fs::is_directory(targetDir)) {
        std::cerr << "Invalid directory: " << targetDir << "\n";
        return 1;
    }
    
    std::ofstream outFile("combined.txt", std::ios::out | std::ios::binary);
    if (!outFile) {
        std::cerr << "Failed to create output file combined.txt\n";
        return 1;
    }
    
    // Gather .gitignore rules from the directory and its parents.
    auto rules = gatherGitIgnoreRules(targetDir);
    processDirectory(targetDir, outFile, rules, targetDir);
    
    std::cout << "Files have been combined into combined.txt\n";
    return 0;
}
