// Annota - gui.hpp : declarative views.
//
// Views are compiled into component trees (`UiNode`).  When the interpreter is built with Qt the
// tree is shown in a real window; otherwise only the textual tree dump is available.
#pragma once
#include "vm.hpp"
#include <utility>
#include <vector>

namespace annota {

// textual rendering of a component tree (used by --gui-tree and by the test suite)
std::string renderTree(const Value& root);

// true when this build can open a window
bool guiAvailable();

// a multi-line explanation of how to get window support (empty when Qt is available)
std::string noQtAdvice();

// open a window with the given view class and run the event loop; blocks until the window closes
int guiRun(VM& vm, const std::string& viewName, int argc, char** argv);

// Two phase variant used when the script must run *before* the window is shown but still wants
// Qt services (input dialogs, for instance): guiInit() creates the QApplication, guiRunView()
// shows the view and enters the event loop.
int guiInit(int argc, char** argv);
int guiRunView(VM& vm, const std::string& viewName);

// Make `input` ask the user with a dialog instead of reading stdin, so that interactive
// programs work inside `--gui` and inside the IDE. No-op without Qt.
void guiInstallInput(VM& vm);

// Show a view in a window that is *not* modal and does not start an event loop of its own:
// used by the IDE, which already owns a QApplication and its event loop. The window deletes
// itself when closed, and `vm` must stay alive while it is open.
// Returns 0 on success, 2 when the view is unknown, 3 when there is no Qt application.
int guiShowView(VM& vm, const std::string& viewName);

// render the view into a PNG file without opening a window.  `clicks` are synthetic
// (x, y) button presses and `keys` synthetic keystrokes dispatched before rendering,
// which makes event handling testable.
int guiRenderPng(VM& vm, const std::string& viewName, const std::string& path,
                 const std::vector<std::pair<int, int>>& clicks = {},
                 const std::vector<std::string>& keys = {});

// the size the window should have for a given view class (0x0 when unknown)
void guiViewSize(VM& vm, const std::string& viewName, int& w, int& h, std::string& title);

} // namespace annota
