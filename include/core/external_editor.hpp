#pragma once

#include <string>
#include <vector>

// =============================================================================
//  External code editors
//
//  Lithium ships a built-in script editor, which is fine for a two-line tweak and
//  miserable for anything real: no completion, no multi-file search, no debugger,
//  no version control. Anyone writing actual gameplay already has an editor they
//  prefer, so this lets them use it.
//
//  Every editor is described by the same three things: what to call it, which
//  executables might be it, and how it wants to be told to open a file at a line.
//  That last part is the reason this is a table rather than one hard-coded
//  command - no two of them agree. VS Code wants `--goto file:line`, Sublime wants
//  `file:line` as a bare argument, Kate wants `-l line file`, Notepad++ wants
//  `-nLINE`. Getting it wrong means the editor opens the right file at the wrong
//  place, or treats "file:12" as a filename and creates an empty buffer.
// =============================================================================

namespace ExternalEditor {

struct Definition {
    // Shown in the settings dropdown.
    const char* display_name;
    // Executables that might be this editor, tried in order. More than one because
    // the same editor is packaged under different names - VS Code is `code` from
    // Microsoft's build and `code-oss` or `vscode` from a distribution's.
    std::vector<std::string> candidates;
    // Argument template. {file} and {line} are substituted; an entry that collapses
    // to nothing after substitution is dropped, which is how the line-number
    // argument disappears when no line is known.
    std::vector<std::string> argument_template;
    // True if this editor understands a line number at all. The ones that do not
    // simply open the file, rather than being handed an argument they would treat
    // as a second filename.
    bool supports_line = true;
};

// Every editor the engine knows how to drive, in a stable order. Index 0 is always
// the built-in editor and index 1 the system default handler, so a saved preference
// referring to either stays valid even if the table grows.
const std::vector<Definition>& registry();

// Indices of the two entries that are always present and always available.
constexpr int kBuiltIn = 0;
constexpr int kSystemDefault = 1;

// First candidate of `definition` that is actually installed, or empty if none is.
// This is what decides whether an entry is offered as selectable.
std::string resolve_executable(const Definition& definition);

// Opens `file_path` at `line` (1-based; 0 or less means "no particular line") in the
// editor at `registry_index`. A `custom_command` is used instead when the index is
// past the end of the registry, which is how "Custom" is represented.
//
// Returns false if the editor could not be launched at all, so the caller can say so
// rather than leaving the user staring at nothing happening.
bool open_file(int registry_index, const std::string& custom_command,
               const std::string& file_path, int line);

} // namespace ExternalEditor
