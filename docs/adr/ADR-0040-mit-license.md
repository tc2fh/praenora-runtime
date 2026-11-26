# ADR-0040: MIT license

- Status: Accepted
- Date: 2026-11-21
- Supersedes: none
- Superseded by: none

## Context

The repository had no license: which one it should have was an open question (OQ-1), and until it
was decided no license file and no `license` field were allowed. Distribution and contribution
terms depend on it, and so does every installation or archive of the runtime that leaves the
project.

The repository praenora-runtime-deps, which builds the third-party libraries that the runtime and
its providers link (LLVM, libRoadRunner with libroadrunner-deps' libraries, libAntimony,
nlohmann/json, and the json-schema-validator that the runtime used before
[ADR-0043](ADR-0043-json-schema-dialects-and-validator.md)) into dependency bundles, is under the MIT license, copyright
The Praenora developers. Each bundle holds the license files of its libraries in `licenses/`.

An installation of the runtime holds third-party code linked statically or compiled in from its
headers: jsoncons and nlohmann/json, which are header-only (jsoncons' headers also carry the MIT and
Apache 2.0 texts of code they include: a5hash, Grisu3, MongoDB's BSON headers); yaml-cpp and
spdlog when they are built in the tree; and, with the providers, libRoadRunner, LLVM, libSBML and
libAntimony. Each keeps the license of its upstream project, and those licenses ask for their text
to travel with copies of the code.

## Decision

- **MIT.** This repository's files are under the MIT license, copyright The Praenora developers.
  `LICENSE` at the root holds the text of praenora-runtime-deps' `LICENSE`, byte for byte. Decided
  by the maintainer on 2026-11-21; it resolves OQ-1.
- **License files in every installation**, in `${CMAKE_INSTALL_DATADIR}/praenora/licenses/`
  (`share/praenora/licenses/`), with CMake's default install component, so that a plain
  `cmake --install` and an archive made from the installation hold them
  ([`cmake/PraenoraInstall.cmake`](../../cmake/PraenoraInstall.cmake)):
  - `praenora-runtime/LICENSE`: this repository's license.
  - `<name>/` for each dependency built in the tree with FetchContent (`nlohmann_json`,
    `jsoncons`, `yaml-cpp`, `spdlog`; never GoogleTest, which only the tests
    link): the files at the top of its sources whose names begin with `LICENSE`, `COPYING` or
    `NOTICE`, for jsoncons the four headers that carry other licenses' texts, and for spdlog the license of the fmt library it bundles, each under its path in the
    sources. A dependency built in the tree without a `LICENSE` or `COPYING` file stops the
    configuration.
  - The contents of each directory of the cache variable `PRAENORA_THIRD_PARTY_LICENSE_DIRS`
    (`STRING`, a `;`-list of absolute paths, empty by default), such as a dependency bundle's
    `licenses/`. An entry that is not the absolute path of a directory stops the configuration.
- **The Python distribution.** `pyproject.toml` says `license = { text = "MIT" }`: the wheel's
  metadata says `License: MIT`, and scikit-build-core copies `LICENSE` into the wheel's
  `.dist-info` (into `licenses/` there with scikit-build-core 1.1.2; 0.10 not yet verified). A
  wheel build has no install rules, so the wheel holds no other license file.
- **The repository rules.** AGENTS.md drops the prohibition of a license file and a `license`
  field and gains a convention: this repository's files are MIT, and a statically linked
  third-party dependency brings its license files into the installation.

### Builder notes (Proposed)

- The table form `{ text = "MIT" }` rather than PEP 639's SPDX string `license = "MIT"`:
  `build-system.requires` accepts scikit-build-core 0.10, and PEP 639 came to its vendored
  pyproject-metadata with 0.9 (scikit-build-core 0.11); not checked with 0.10 itself, since only
  1.1.2 was at hand offline. With 1.1.2 and `minimum-version = "0.10"` the wheel's metadata stays
  `Metadata-Version: 2.1` and says `License: MIT`, without a warning (pyproject-metadata warns about
  the table form only for metadata 2.4 and newer), and `LICENSE` lands in
  `.dist-info/licenses/LICENSE` by scikit-build-core's default `wheel.license-files` patterns
  (`LICEN[CS]E*`, `COPYING*`, `NOTICE*`, `AUTHORS*`). Moving to the SPDX string means raising the
  floor of `build-system.requires` to 0.11, and `minimum-version`, which `pyproject.toml` keeps at
  that floor, with it.
- A dependency counts as built in the tree when this configuration made it available with
  FetchContent and its target is not `IMPORTED`, the rule that the dependency status lines follow;
  FetchContent satisfied by `find_package()` gives an `IMPORTED` target, and so does a package
  found without the fetch options. `praenora_find_dependencies()`
  ([`cmake/PraenoraDependencies.cmake`](../../cmake/PraenoraDependencies.cmake)) records each such
  dependency in `PRAENORA_FETCHED_DEPENDENCIES`, with its source directory in
  `PRAENORA_FETCHED_SOURCE_DIR_<name>`.
- The names of license files are matched by a regular expression, ignoring case, among the files
  at the top of the sources, not by `file(GLOB)` patterns, whose case sensitivity differs between
  platforms (it ignores case on macOS and Windows): every platform installs the same files.
  `NOTICE` files are installed too: the four dependencies are under the MIT license, but one under
  the Apache License that is built in the tree must pass its `NOTICE` on (section 4(d)); a `NOTICE`
  alone does not satisfy the check for a license file.
  Directories are left out: of nlohmann/json 3.12.0, `LICENSE.MIT`, the library's license, is
  installed, as in a dependency bundle, and its `LICENSES/` directory is not. That directory holds
  the texts of the licenses used anywhere in its repository (Apache-2.0, BSD-3-Clause,
  GPL-3.0-only, MIT), which its `.reuse/dep5` assigns to files of its tests and tools; its README
  also says that the header holds parts of Google Abseil (Apache 2.0) and a copy of Hedley (CC0),
  which no license review has looked at yet. spdlog's fmt license is any file whose name holds
  "license", in any case, in `include/spdlog/fmt/bundled/`, where spdlog keeps `fmt.license.rst`
  (not checked against spdlog 1.17.0, whose sources were not at hand offline: a substitute source
  tree checked the rule); none is required.
- The build cannot tell where an installed package keeps its license files, so a static library
  found with `find_package()`, such as a bundle's jsoncons (once it holds one) or the providers' engines,
  brings them only through `PRAENORA_THIRD_PARTY_LICENSE_DIRS`. Its entries are checked when the
  install rules are generated, the only time they are used; a relative path is refused rather than
  read against a directory that the configure line does not show.
- Checked on macOS arm64: `install.consumer` requires `praenora-runtime/LICENSE`, equal to `LICENSE`
  by SHA-256, in the installation and in the moved one; in a tree with a dependency built
  from local sources (at the time of writing json-schema-validator, which ADR-0043 replaced with
  jsoncons) and the bundle's `licenses/` in `PRAENORA_THIRD_PARTY_LICENSE_DIRS` it also
  requires that dependency's `licenses/<name>/` and the bundle's 19 files, and fails when the install
  rule of either is taken out of the generated install script; a configuration with
  nlohmann/json 3.12.0 from local sources installs `nlohmann_json/LICENSE.MIT` alone; a substitute
  source tree for spdlog (configured, not built) gets an install rule for its fmt license under its
  path, and one without a license file stops the configuration; a wheel built offline holds
  `.dist-info/licenses/LICENSE`.

## Alternatives considered

- **Apache-2.0** (named in OQ-1), with its patent grant and `NOTICE` rules: the dependency bundles'
  repository is MIT, and one license for both keeps the terms of the runtime and of the bundles it
  is built from alike.
- **No license until the first release**: installations and archives made in the meantime would
  leave the project without terms.
- **The SPDX string of PEP 639 in `pyproject.toml`**: it needs scikit-build-core 0.11 or newer,
  which would raise the build requirement's floor for one field; the table form gives the wheel the
  same license and license file.
- **Finding the license files of installed packages** next to their CMake files, or through
  `CMAKE_PREFIX_PATH`: package layouts differ, and a guess would install files that do not belong
  to the library or miss those that do; a directory named in the configuration is explicit.

## Consequences

- The terms of this repository's files are settled; contributions come under the same license.
- Every installation, and an archive made from one, holds `share/praenora/licenses/`, and
  `install.consumer` checks it there and after a move: this project's `LICENSE`, byte for byte, a
  file in `<name>/` for each dependency built in the tree, and every file of the directories that
  `PRAENORA_THIRD_PARTY_LICENSE_DIRS` names, byte for byte. A build without either checks only the
  `LICENSE`.
- A new third-party dependency linked statically must bring its license files into the
  installation: built in the tree, through the rule above; installed, through
  `PRAENORA_THIRD_PARTY_LICENSE_DIRS` (AGENTS.md).
- The providers' engines keep their own licenses, and the questions of distributing them remain
  open: the terms of the NLEQ solvers and the statically linked LGPL-2.1 libSBML (OQ-27), and
  libAntimony's license stated two ways, libSBML, SBMLNetwork and expat (OQ-29).
- The wheel holds this repository's `LICENSE` only, not the license files of the code linked into
  its runtime library; how wheels are distributed is OQ-13.

## References

- [LICENSE](../../LICENSE); [developer-guide.md](../developer-guide.md#license-files);
  [open-questions.md](../open-questions.md) (OQ-1, OQ-13, OQ-27, OQ-29).
- [ADR-0017](ADR-0017-dependency-management.md): dependency management.
- The MIT license: <https://opensource.org/license/mit>.
- PEP 639, license expressions in package metadata: <https://peps.python.org/pep-0639/>.
- praenora-runtime-deps: its `LICENSE`, and its `docs/outputs.md` (Licenses), which lists the
  license files of a bundle.
