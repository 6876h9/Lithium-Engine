#include "core/external_editor.hpp"
#include "core/platform.hpp"

#include <filesystem>

namespace ExternalEditor {

namespace {

// Substitutes {file} and {line} into one argument. Returns false when the argument
// referenced {line} but no line was available, so the caller can drop it entirely
// rather than pass a flag with an empty value after it.
bool expand(const std::string& pattern, const std::string& file, int line,
            std::string& out) {
    if (pattern.find("{line}") != std::string::npos && line <= 0) return false;

    out.clear();
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern.compare(i, 6, "{file}") == 0) {
            out += file;
            i += 6;
        } else if (pattern.compare(i, 6, "{line}") == 0) {
            out += std::to_string(line);
            i += 6;
        } else {
            out += pattern[i++];
        }
    }
    return true;
}

// Splits a custom command into a program and its arguments on whitespace, honouring
// double quotes so a path containing a space can be given as one token. Deliberately
// not a shell: the string never reaches /bin/sh, so a stray semicolon in a path is a
// character in a filename rather than a second command.
std::vector<std::string> tokenize(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    bool have_token = false;

    for (char c : command) {
        if (c == '"') {
            in_quotes = !in_quotes;
            have_token = true;
        } else if (!in_quotes && (c == ' ' || c == '\t')) {
            if (have_token) { tokens.push_back(current); current.clear(); have_token = false; }
        } else {
            current += c;
            have_token = true;
        }
    }
    if (have_token) tokens.push_back(current);
    return tokens;
}

} // namespace

const std::vector<Definition>& registry() {
    // Built once. The detection in resolve_executable() is the part that varies by
    // machine; the table itself is fixed.
    static const std::vector<Definition> editors = {
        // Index 0 and 1 are special-cased by open_file() and carry no executable.
        { "Built-in Editor", {}, {}, false },
        { "System Default", {}, {}, false },

        // --goto is what makes VS Code jump to a line; without it the file:line
        // argument is taken as a literal filename and it opens an empty buffer.
        { "Visual Studio Code", { "code", "code-oss", "vscode", "code-insiders" },
          { "--goto", "{file}:{line}" }, true },
        { "VSCodium", { "codium", "vscodium" }, { "--goto", "{file}:{line}" }, true },

        // devenv opens the file in a running instance when one is already up, which
        // is what anyone with a solution open expects. It has no line-number
        // argument - /Command is a separate mechanism and not reliable from a cold
        // start - so the file is opened at the top.
        { "Visual Studio", { "devenv", "devenv.exe" }, { "/edit", "{file}" }, false },

        // MonoDevelop and its Visual Studio for Mac lineage take file;line.
        { "MonoDevelop", { "monodevelop", "mono-develop" }, { "{file};{line}" }, true },

        // The JetBrains launchers all share --line.
        { "Rider", { "rider", "rider.sh" }, { "--line", "{line}", "{file}" }, true },
        { "CLion", { "clion", "clion.sh" }, { "--line", "{line}", "{file}" }, true },

        { "Sublime Text", { "subl", "sublime_text" }, { "{file}:{line}" }, true },

        // Notepad++ wants the line glued to the flag: -n12, not -n 12.
        { "Notepad++", { "notepad++", "notepad++.exe" }, { "-n{line}", "{file}" }, true },

        { "Kate", { "kate" }, { "-l", "{line}", "{file}" }, true },
        { "gedit", { "gedit" }, { "+{line}", "{file}" }, true },
        { "Geany", { "geany" }, { "+{line}", "{file}" }, true },

        // Terminal editors need a terminal to live in; launched bare they would exec
        // with no controlling tty and vanish. x-terminal-emulator is the Debian
        // alternatives name and the most portable way to ask for "a terminal".
        { "Vim (in terminal)", { "x-terminal-emulator", "xterm" },
          { "-e", "vim", "+{line}", "{file}" }, true },
        { "Neovim (in terminal)", { "x-terminal-emulator", "xterm" },
          { "-e", "nvim", "+{line}", "{file}" }, true },
    };
    return editors;
}

std::string resolve_executable(const Definition& definition) {
    for (const std::string& candidate : definition.candidates) {
        if (Platform::executable_exists(candidate)) return candidate;
    }
    return {};
}

bool open_file(int registry_index, const std::string& custom_command,
               const std::string& file_path, int line) {
    if (file_path.empty()) return false;

    // Editors are given an absolute path. A relative one is resolved against the
    // editor's own working directory, not the engine's, so a file that opened
    // correctly from one launch directory would fail from another.
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(file_path, ec);
    const std::string target = ec ? file_path : absolute.string();

    const std::vector<Definition>& editors = registry();

    // Past the end of the table means Custom.
    if (registry_index < 0 || registry_index >= static_cast<int>(editors.size())) {
        const std::vector<std::string> tokens = tokenize(custom_command);
        if (tokens.empty()) return false;

        std::vector<std::string> arguments;
        bool mentions_file = false;
        for (size_t i = 1; i < tokens.size(); ++i) {
            std::string expanded;
            if (!expand(tokens[i], target, line, expanded)) continue;
            if (tokens[i].find("{file}") != std::string::npos) mentions_file = true;
            arguments.push_back(expanded);
        }
        // A command that never says where the file goes still has to open it, so
        // it is appended - which is what almost every editor wants anyway.
        if (!mentions_file) arguments.push_back(target);
        return Platform::launch_detached(tokens[0], arguments);
    }

    if (registry_index == kBuiltIn) return false;   // handled by the caller
    if (registry_index == kSystemDefault) {
        return Platform::open_with_default_application(target);
    }

    const Definition& definition = editors[static_cast<size_t>(registry_index)];
    const std::string executable = resolve_executable(definition);
    if (executable.empty()) return false;

    std::vector<std::string> arguments;
    for (const std::string& pattern : definition.argument_template) {
        std::string expanded;
        // An argument carrying {line} is dropped when there is no line, rather than
        // passing the editor a flag with nothing after it - which several of them
        // read as "the next argument is the value" and so swallow the filename.
        if (!expand(pattern, target, definition.supports_line ? line : 0, expanded)) {
            continue;
        }
        arguments.push_back(expanded);
    }

    // If every {line} argument was dropped the filename may have gone with it, so
    // make sure the editor is still told what to open.
    bool has_target = false;
    for (const std::string& argument : arguments) {
        if (argument.find(target) != std::string::npos) has_target = true;
    }
    if (!has_target) arguments.push_back(target);

    return Platform::launch_detached(executable, arguments);
}

} // namespace ExternalEditor
