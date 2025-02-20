# Project Compressor

A C++ utility that combines multiple source files into a single text file while respecting `.gitignore` rules and handling binary file detection.

## Features

- Recursively processes directories and combines text files
- Respects `.gitignore` patterns including:
  - Leading `/` for anchored paths
  - Trailing `/` for directory-only matches
  - `*` and `**` wildcards
  - Negation with `!`
- Automatically detects and skips binary files
- Preserves file paths in the combined output
- Handles nested `.gitignore` files

## Building

### Prerequisites

- CMake 3.10 or higher
- C++20 compatible compiler
- Visual Studio Build Tools (for Windows)

### Windows Build

Run the provided PowerShell build script:

```powershell
.\build.ps1
```

The script will:
1. Initialize VS Developer Shell
2. Configure CMake for x64
3. Build the project
4. Copy the executable to the project root

### Manual Build

```bash
cmake -B build
cmake --build build --config Release
```

## Usage

```bash
ProjectCompressor <directory_path>
```

The program will:
1. Scan the specified directory and its subdirectories
2. Process all text files while respecting `.gitignore` rules
3. Create a `combined.txt` file containing all the processed files

### Example Output Format

```
# File: /path/to/source/file1.cpp

[contents of file1.cpp]

# File: /path/to/source/file2.hpp

[contents of file2.hpp]
```

## Implementation Details

### GitIgnore Rule Processing
The program implements sophisticated `.gitignore` rule parsing and matching:

```cpp
// A structure representing a single .gitignore rule.
struct GitIgnoreRule {
    std::regex patternRegex; // The pattern converted to a regex.
    bool negate = false;     // True if the rule starts with '!'
    bool directoryOnly = false; // True if the pattern ends with '/'
    bool anchored = false;      // True if the pattern starts with '/'
    std::string originalPattern; // The original pattern text.
};
```

### Binary File Detection
Uses a heuristic approach to detect binary files by sampling content:

```cpp
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
```

### Directory Processing
Recursively processes directories while respecting ignore rules:

```cpp
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
```

## Contributing

1. Fork the repository
2. Suggest/Make some improvements
3. Create a new Pull Request

## Known Limitations

- Does not handle all edge cases of `.gitignore` pattern matching
- Binary file detection is heuristic-based and may have false positives/negatives
- Large directories with many files may consume significant memory
