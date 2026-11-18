# Contributing to Folvio

## Reporting a problem

Open an issue with:

- the Folvio version (*Settings → About*) and your system (Windows version or Linux distribution);
- what you did, what you expected and what happened;
- the self-test report if a conversion or engine is involved: `folvio --selftest` (Linux) or
  `Folvio.exe --selftest --selftest-report selftest.txt` (Windows).

**Do not attach private documents.** If a file is needed, recreate the problem with a document you made up.

## Development

- Build and test as described in the README (*Build from source*). Linux builds run in the container from
  `packaging/linux/Dockerfile`; Windows builds use Qt 6.10 with MinGW 13.1.
- C++20, Qt style naming, 4-space indentation, 120 columns. Code, comments and identifiers in English.
- Every behaviour change comes with a test in `tests/` (GoogleTest; GUI tests drive real widgets with QTest).
  Test fixtures must be fictional and generated or committed without personal data.
- Run `python3 tools/check_repo.py` before committing (secrets, personal paths, versions, pinned actions).
- Pull requests must pass the CI workflow on Windows and Linux.

## Releasing (maintainers)

1. Update the version in `CMakeLists.txt`, `packaging/windows/folvio.iss` and
   `packaging/linux/io.github.twivo1.Folvio.metainfo.xml`, and add `docs/release-notes/vX.Y.Z.md`.
2. Push a tag `vX.Y.Z` (or `vX.Y.Z-beta.N` for a pre-release). The release workflow builds, tests, verifies
   the packages on clean systems and publishes them with SHA-256 checksums.
