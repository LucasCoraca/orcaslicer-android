// The plugin system's package installer (PythonInterpreter::bundled_uv_path): upstream runs a bundled uv binary,
//   uv pip install --python <python> --no-python-downloads --target <dir> <requirement>...
// which has no Android build. This stand-in, packaged as lib/<abi>/libuv_exe.so and linked from
// <resources>/tools/uv/uv, runs the same install with pip instead: the bundled python with the pip wheel CPython
// ships (ensurepip), placed in the python home as pip.whl. Only wheels are installed (--only-binary): there is no
// compiler on a phone, and no package's build script runs.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "pip") != 0 || strcmp(argv[2], "install") != 0) {
        fprintf(stderr, "uv (OrcaSlicer for Android): only 'uv pip install' is supported\n");
        return 2;
    }
    const char *python = NULL, *target = NULL;
    char **reqs = calloc((size_t) argc, sizeof(char *));
    int nreqs = 0;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--python") == 0 && i + 1 < argc) python = argv[++i];
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = argv[++i];
        else if (strcmp(argv[i], "--no-python-downloads") == 0) continue;
        else if (argv[i][0] == '-') { fprintf(stderr, "uv (OrcaSlicer for Android): unsupported option %s\n", argv[i]); return 2; }
        else reqs[nreqs++] = argv[i];
    }
    if (!python || !target || nreqs == 0) {
        fprintf(stderr, "uv (OrcaSlicer for Android): --python, --target and a requirement are needed\n");
        return 2;
    }

    // <home>/bin/python3 -> <home>/pip.whl; python runs pip straight from its wheel
    char home[PATH_MAX], pip[PATH_MAX + 16], tmp[PATH_MAX + 16];
    strncpy(home, python, sizeof home - 1);
    home[sizeof home - 1] = 0;
    for (int up = 0; up < 2; ++up) {
        char *slash = strrchr(home, '/');
        if (slash) *slash = 0;
    }
    snprintf(pip, sizeof pip, "%s/pip.whl/pip", home);
    // pip's temporary build and download files: the app has no writable /tmp
    if (!getenv("TMPDIR")) {
        snprintf(tmp, sizeof tmp, "%s/.pip-tmp", target);
        mkdir(tmp, 0700);
        setenv("TMPDIR", tmp, 1);
    }

    const char *fixed[] = {python, pip, "install", "--disable-pip-version-check", "--no-input", "--no-cache-dir",
                           "--only-binary=:all:", "--target", target};
    const int nfixed = (int) (sizeof fixed / sizeof fixed[0]);
    char **args = calloc((size_t) (nfixed + nreqs + 1), sizeof(char *));
    for (int i = 0; i < nfixed; ++i) args[i] = (char *) fixed[i];
    for (int i = 0; i < nreqs; ++i) args[nfixed + i] = reqs[i];
    execv(python, args);
    perror("uv (OrcaSlicer for Android): python");
    return 127;
}
