// Annota - commands.hpp : CLI entry points provided by ide_cmd.cpp / lsp.cpp.
#pragma once
namespace annota {

int cmdAnalyze(int argc, char** argv);        // annota analyze <file> [...]
int cmdAnalyzeSuite(int argc, char** argv);   // annota analyze-suite <dir>
int cmdIde(int argc, char** argv);            // annota ide <query> ...
int cmdBench(int argc, char** argv);          // annota bench [--funcs=N] [--repeat=N]
int cmdStudio(int argc, char** argv);         // annota studio [file]  (the graphical IDE)
int cmdLsp(int argc, char** argv);            // annota lsp

} // namespace annota
