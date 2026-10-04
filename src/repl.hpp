// Annota - repl.hpp : interactive read-eval-print loop.
#pragma once
#include <string>
#include <vector>

namespace annota {

// Starts the interactive session.  `preload` holds files executed before the prompt appears.
// Returns a process exit code.
int runRepl(const std::vector<std::string>& preload);

} // namespace annota
