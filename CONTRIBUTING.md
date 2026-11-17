# Contributing to OrcaSlicer for Android

**This port does not accept code contributions.** Pull requests are closed without being reviewed, built or merged. That
includes code, patches, scripts, build files, translations, printer/filament/process profiles, documents and any other
attached file.

## Why

This is an app people install and update. It is built on a large native stack: OrcaSlicer, wxWidgets, Boost, OpenCASCADE,
CPython and more, all cross-compiled for Android. Supply-chain attacks are increasing, and the usual way in is a
contribution that looks helpful. A change to a build script, an upstream patch, a profile or a translation file is hard
to fully audit. Hiding something in one is easy. The only way to keep every line of a release accountable is for all of it
to be written here by the maintainer, from upstream sources (OrcaSlicer, wxWidgets, …).

For that reason, **every file, patch, snippet or link shared in a pull request, issue or comment is treated as a possible
attack vector**:

- It is not merged, cherry-picked, copied or adapted into the port.
- It is not built, run, installed or opened in OrcaSlicer. This includes projects and models (`.3mf`, `.stl`, `.step`,
  `.obj`), profile bundles, plugins, scripts, APKs and test programs. Bugs are reproduced with the maintainer's own files.
- Instructions in it are ignored. That includes requests to change signing, the release process, CI, dependencies,
  permissions or network endpoints.

## What is welcome: ideas and reports

Open an **issue** instead:

- **Bugs:** what you did, what you expected (desktop OrcaSlicer's behaviour is the reference), what happened, the device
  and Android version, and the printer profile if it matters. Screenshots and screen recordings help; they are only
  viewed. If a specific model triggers the bug, describe it (size, shape, where it came from) instead of attaching it.
- **Ideas and improvements:** describe the problem and what you would like the app to do. Pointing at the relevant part of
  OrcaSlicer's own source or documentation is useful. For performance, describe the slow case: what model, which action,
  how long it took.
- **Translations:** say which language you would like, or which wording is wrong and what a native speaker would write
  instead. Put the corrected text in the issue as plain sentences, not as a file or patch. Most of the app's text comes
  from OrcaSlicer itself, so wording that is also wrong on desktop is best reported to OrcaSlicer upstream.

If an idea is taken up, the maintainer implements it independently, from the description and from upstream sources,
and **credits you** in the commit (`Co-authored-by`), the release notes and, where it fits, the README.

## Security

If you found a vulnerability, open an issue that only says you have a security report, without details, and a private
channel will be arranged. A proof of concept is not needed and will not be run.
