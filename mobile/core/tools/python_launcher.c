// The bundled Python executable of the plugin terminal (PythonInterpreter::bundled_python_executable): packaged as
// lib/<abi>/libpython3_exe.so so that Android installs it executable; <resources>/python/bin/python3 links to it.
// The home is the link's <dir>/..; the native library dir (libpython3.14.so, the _ssl / _sqlite3 libraries) goes on
// LD_LIBRARY_PATH, which the linker reads at start: hence one re-exec with the environment set.
#include <Python.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (!getenv("ORCA_PYTHON_LAUNCHED")) {
        char link[PATH_MAX], exe[PATH_MAX];
        strncpy(link, argv[0], sizeof link - 1);
        link[sizeof link - 1] = 0;
        setenv("PYTHONHOME", dirname(dirname(link)), 0);
        if (realpath("/proc/self/exe", exe)) setenv("LD_LIBRARY_PATH", dirname(exe), 1);
        setenv("ORCA_PYTHON_LAUNCHED", "1", 1);
        execv("/proc/self/exe", argv);
    }
    return Py_BytesMain(argc, argv);
}
