// Annota - sys_api.hpp : the low level primitive ABI.
//
// Design rule: C++ provides only *primitives* - the thinnest possible wrappers over the
// operating system, named `_sys_*` (like the file primitives `_file_*` / `_dir_*` / `_path_*`).
// Everything with policy, formatting or convenience lives in the standard library (`lib/*.mod`),
// written in Annota, so adding a feature normally means writing a `.mod` file, not rebuilding
// the interpreter.
//
//   time     _sys_clock _sys_time _sys_sleep _sys_local_time _sys_make_time
//   process  _sys_info _sys_env _sys_env_set _sys_env_all _sys_exec
//   threads  _sys_spawn _sys_join _sys_task_done
//   network  _sys_socket(op, ...)        connect / listen / accept / send / recv / close / resolve
//
// The `system` facade exposes the same primitives plus a dynamic dispatcher (`system.call`),
// so a program can reach the ABI by name without the interpreter knowing the capability.
#pragma once
#include "vm.hpp"
#include <string>

namespace annota {

// Installs the `_sys_*` primitives, the small native `time` module and the `system` facade.
void registerSysPrimitives(VM& vm);

// Markdown description of the ABI, used by `annota ide docs`.
std::string sysPrimitivesMarkdown();

} // namespace annota
