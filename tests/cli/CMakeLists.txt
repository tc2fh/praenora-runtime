# CLI tests: each runs the praenora tool through run_cli.cmake and checks its exit code and output.
# CTest names cli.<command>.<case>, label cli. They use the fixture modules, the fixture service
# providers, the test catalog and the configurations of tests/fixtures/configs/cli and, when the
# examples are built, the example modules and workflow. Runs write their output directories below
# the build tree. Two more tests check the driver itself (check_driver.cmake):
# cli.driver.passes_empty_arguments, that each argument of a command reaches its program, an empty
# one included, and cli.driver.shows_failing_command, how it shows a command that fails.

set(_praenora_cli_driver "${CMAKE_CURRENT_SOURCE_DIR}/run_cli.cmake")
set(_praenora_cli_tool "$<TARGET_FILE:praenora_cli>")
set(_praenora_cli_out "${CMAKE_CURRENT_BINARY_DIR}/out")
set(_praenora_fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures")
set(_praenora_cli_configs "${_praenora_fixtures}/configs/cli")
set(_praenora_example_config
    "${PROJECT_SOURCE_DIR}/examples/workflows/estimate-predict/workflow.yaml")
# A directory separator in a path that the tool prints, for a regular expression: '/', or '\' on
# Windows, which a JSON document writes as '\\'.
set(_praenora_separator "[/\\\\]+")
# The fixture modules and the test catalog.
set(_praenora_fixture_paths
    --module-path "${PRAENORA_TEST_MODULES_DIR}"
    --catalog-path "${_praenora_fixtures}/catalog")

# praenora_add_cli_test(<name> CMD <argument>... [ENVIRONMENT <name=value>...]
#                       [INTERRUPT <count> READY_FILE <status file>]
#                       [<driver variable> <value>...]...)
#
# Adds CTest test <name> (label cli) that runs the praenora tool with the CMD arguments, an empty
# one included, through run_cli.cmake. The other keywords are the driver's variables (see there):
# SETUP_CMD and AFTER_CMD are argument lists like CMD; EXPECT_JSON_PATH, EXPECT_JSON_VALUE,
# EXPECT_FILE_JSON_PATH, EXPECT_FILE_JSON_VALUE and COPY_FILES take several values.
# ENVIRONMENT sets environment variables (name=value) for the test. With INTERRUPT (POSIX only),
# a plain POSIX shell runs the CMD command in the foreground (exec, so it keeps SIGINT at its
# default disposition) while a background waiter waits, for at most 120 s, until the status file
# READY_FILE shows the run running, then sends the tool SIGINT `count` times, half a second
# apart; a second signal only when the tool is still alive, which the waiter records by creating
# <READY_FILE>.second-signal first. A tool that never gets ready, or still runs 30 s after the
# last signal, is killed. Such a test times out after 180 s.
function(praenora_add_cli_test name)
    set(commands CMD SETUP_CMD AFTER_CMD)
    set(lists EXPECT_JSON_PATH EXPECT_JSON_VALUE EXPECT_FILE_JSON_PATH EXPECT_FILE_JSON_VALUE
        COPY_FILES)
    set(values EXPECT_EXIT EXPECT_STDOUT_REGEX EXPECT_STDERR_REGEX EXPECT_STDOUT_FILE
        EXPECT_JSON_LENGTH EXPECT_FILE CLEAN_DIR AFTER_EXPECT_EXIT INTERRUPT READY_FILE)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "${values}" "${commands};${lists};ENVIRONMENT")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "praenora_add_cli_test(${name}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT DEFINED arg_CMD)
        message(FATAL_ERROR "praenora_add_cli_test(${name}): CMD is required")
    endif()
    set(definitions "")
    foreach(key IN LISTS commands lists)
        if(DEFINED arg_${key})
            set(value "${arg_${key}}")
            if(key IN_LIST commands)
                set(value "${_praenora_cli_tool};${value}")
            endif()
            if(key STREQUAL "CMD" AND DEFINED arg_INTERRUPT)
                # A plain POSIX shell script, without job control: a background waiter (its output
                # redirected, so nothing waits on it) waits for the run to be running, signals
                # the shell's pid, which the tool takes over with exec in the foreground, so it
                # starts with SIGINT at its default disposition. The waiter ends when the tool
                # does, and kills a tool that never gets ready or never ends. Lines are separated
                # by newlines: a ';' would split the list.
                set(script "ready=$1\nsignals=$2\nshift 2\n(\n  waited=0\n")
                string(APPEND script
                    "  until grep -q '\"state\":\"running\"' \"$ready\" 2>/dev/null\n  do\n"
                    "    kill -0 $$ 2>/dev/null || exit 0\n"
                    "    if [ $waited -ge 1200 ]\n    then\n      kill -KILL $$\n      exit 0\n"
                    "    fi\n    sleep 0.1\n    waited=$((waited + 1))\n  done\n"
                    "  sleep 0.5\n  kill -INT $$\n"
                    "  if [ \"$signals\" -ge 2 ]\n  then\n    sleep 0.5\n"
                    "    if kill -0 $$ 2>/dev/null\n    then\n"
                    "      : > \"$ready.second-signal\"\n      kill -INT $$\n    fi\n  fi\n"
                    "  waited=0\n  while kill -0 $$ 2>/dev/null\n  do\n"
                    "    if [ $waited -ge 300 ]\n    then\n      kill -KILL $$\n      exit 0\n"
                    "    fi\n    sleep 0.1\n    waited=$((waited + 1))\n  done\n"
                    ") >/dev/null 2>&1 &\nexec \"$@\"\n")
                set(value "/bin/sh;-c;${script};sh;${arg_READY_FILE};${arg_INTERRUPT};${value}")
            endif()
            # A list reaches the driver as one argument: its separators are escaped here.
            string(REPLACE ";" "\\;" value "${value}")
            list(APPEND definitions "-D${key}=${value}")
        endif()
    endforeach()
    list(REMOVE_ITEM values INTERRUPT READY_FILE)
    foreach(key IN LISTS values)
        if(DEFINED arg_${key})
            list(APPEND definitions "-D${key}=${arg_${key}}")
        endif()
    endforeach()
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}" ${definitions} -P "${_praenora_cli_driver}")
    set_tests_properties(${name} PROPERTIES LABELS cli)
    set(environment ${arg_ENVIRONMENT})
    # As for the GoogleTest tests: AddressSanitizer builds on macOS keep module libraries loaded
    # (tests/CMakeLists.txt says why).
    if(APPLE AND "address" IN_LIST PRAENORA_SANITIZERS)
        list(APPEND environment "PRAENORA_DEBUG_KEEP_LIBRARIES=1")
    endif()
    if(environment)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${environment}")
    endif()
    if(DEFINED arg_INTERRUPT)
        set_tests_properties(${name} PROPERTIES TIMEOUT 180)
    endif()
endfunction()

praenora_add_cli_test(cli.version.json
    CMD version --json
    EXPECT_JSON_PATH ok result.runtime
    EXPECT_JSON_VALUE true ${PRAENORA_RUNTIME_VERSION})

praenora_add_cli_test(cli.validate.bad_parameters
    CMD validate "${_praenora_cli_configs}/bad-parameters/workflow.yaml" ${_praenora_fixture_paths}
    EXPECT_EXIT 3
    EXPECT_STDERR_REGEX "workflow\\.yaml:[0-9]+:[0-9]+: error\\[PARAMETERS_INVALID\\]")
praenora_add_cli_test(cli.validate.document_manifest
    CMD validate --document "${_praenora_fixtures}/documents/valid/module-manifest/full.pmm.json"
    EXPECT_STDOUT_REGEX "valid")

praenora_add_cli_test(cli.modules.check_identity_mismatch
    CMD modules check
        "${PRAENORA_TEST_MODULES_DIR}/fx-identity-mismatch/fx-identity-mismatch.pmm.json"
        ${_praenora_fixture_paths}
    EXPECT_EXIT 1
    EXPECT_STDOUT_REGEX "fail +describe\\.identity"
    EXPECT_STDERR_REGEX "error\\[FAILED\\]")

set(_praenora_committed_asyncapi
    "${PROJECT_SOURCE_DIR}/spec/asyncapi/example.estimate-predict-0.1.0.asyncapi.json")
# Test vectors given with --vectors.
praenora_add_cli_test(cli.modules.check_vectors
    CMD modules check "${PRAENORA_TEST_MODULES_DIR}/fx-echo/fx-echo.pmm.json"
        --vectors "${_praenora_cli_configs}/echo-vectors.json" ${_praenora_fixture_paths}
    EXPECT_STDOUT_REGEX "pass +invoke\\.expected_outputs +the outputs of 1 tick")

# Invalid test vectors fail the checks (exit 1), naming the problem, even when no instance of the
# module can be created.
praenora_add_cli_test(cli.modules.check_invalid_vectors
    CMD modules check
        "${PRAENORA_TEST_MODULES_DIR}/fx-needs-parameter/fx-needs-parameter.pmm.json"
        ${_praenora_fixture_paths}
    EXPECT_EXIT 1
    EXPECT_STDOUT_REGEX
        "fail +invoke\\.result_schema +the test vectors [^\n]*test\\.nothing@0\\.1\\.0")

praenora_add_cli_test(cli.graph.asyncapi_matches_committed
    CMD graph --workflow example.estimate-predict@0.1.0 --format asyncapi
    EXPECT_STDOUT_FILE "${_praenora_committed_asyncapi}")
# A definition whose connection carries one message type at two versions is refused with
# MESSAGE_TYPE_INCOMPATIBLE at the connection (exit code 3), before anything is exported.
praenora_add_cli_test(cli.graph.version_mismatch
    CMD graph --workflow bad.version-mismatch@0.1.0 --format asyncapi --json
        --catalog-path "${_praenora_fixtures}/catalog"
        --catalog-path "${_praenora_fixtures}/catalog-bad/version-mismatch"
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name error.code error.location.pointer
    EXPECT_JSON_VALUE false MESSAGE_TYPE_INCOMPATIBLE 404 /connections/0)

praenora_add_cli_test(cli.run.module_failure
    CMD run "${_praenora_cli_configs}/module-failure/workflow.yaml" ${_praenora_fixture_paths}
    EXPECT_EXIT 1
    EXPECT_STDERR_REGEX "error\\[STEP_FAILED\\]")
praenora_add_cli_test(cli.run.timeout
    CMD run "${_praenora_cli_configs}/timeout/workflow.yaml" ${_praenora_fixture_paths}
    EXPECT_EXIT 5
    EXPECT_STDERR_REGEX "error\\[STEP_TIMEOUT\\]")
# SIGINT stops a run: it ends as stopped, prints its summary and exits with 130.
if(NOT WIN32)
    praenora_add_cli_test(cli.run.interrupted
        CLEAN_DIR "${_praenora_cli_out}/interrupted"
        CMD run "${_praenora_cli_configs}/interrupted/workflow.yaml" ${_praenora_fixture_paths}
            --status-file "${_praenora_cli_out}/interrupted/status.json"
        INTERRUPT 1
        READY_FILE "${_praenora_cli_out}/interrupted/status.json"
        EXPECT_EXIT 130
        EXPECT_STDOUT_REGEX "state +stopped"
        EXPECT_STDERR_REGEX "error\\[RUN_STOPPED\\]")
    # A second SIGINT ends the process at once, while the run is held in a call that ignores the
    # first one: exit 130 and nothing printed, the tool still alive when the second signal went
    # out (the marker file), and no process left behind.
    praenora_add_cli_test(cli.run.second_signal_exits_at_once
        CLEAN_DIR "${_praenora_cli_out}/hung"
        CMD run "${_praenora_cli_configs}/hung/workflow.yaml" ${_praenora_fixture_paths}
            --status-file "${_praenora_cli_out}/hung/status.json"
        INTERRUPT 2
        READY_FILE "${_praenora_cli_out}/hung/status.json"
        EXPECT_EXIT 130
        EXPECT_STDOUT_REGEX "^$"
        EXPECT_FILE "${_praenora_cli_out}/hung/status.json.second-signal")
endif()

# Steps in module hosts (process isolation): --isolation process puts every step of the run in a
# module host of its own, and the run completes as in the runtime's process, with a valid run record
# that says so; a module host that crashes (the configuration's run.isolation) fails the run with
# MODULE_CRASHED and how it ended (exit 1), and its status file can still be read; SIGINT during a
# call in a module host stops the run (exit 130), never as a crash. Module hosts are built on POSIX
# only.
if(NOT WIN32)
    set(_praenora_isolation_out "${_praenora_cli_out}/isolation")
    praenora_add_cli_test(cli.run.isolation_process
        CLEAN_DIR "${_praenora_isolation_out}/process"
        CMD run "${_praenora_cli_configs}/isolation/workflow.yaml" ${_praenora_fixture_paths}
            --isolation process --output-dir "${_praenora_isolation_out}/process" --json
        EXPECT_JSON_PATH ok result.state result.ticks_completed
        EXPECT_JSON_VALUE true completed 3
        EXPECT_FILE "${_praenora_isolation_out}/process/run-record.json"
        EXPECT_FILE_JSON_PATH execution.isolation.a.mode execution.isolation.b.mode
            execution.overrides.isolation
        EXPECT_FILE_JSON_VALUE process process process
        AFTER_CMD validate --document "${_praenora_isolation_out}/process/run-record.json")
    praenora_add_cli_test(cli.run.isolation_crash
        CLEAN_DIR "${_praenora_isolation_out}/crash"
        CMD run "${_praenora_cli_configs}/isolation-crash/workflow.yaml" ${_praenora_fixture_paths}
            --status-file "${_praenora_isolation_out}/crash/status.json" --json
        ENVIRONMENT "ASAN_OPTIONS=handle_segv=0:handle_abort=0:detect_leaks=0"
            "TSAN_OPTIONS=handle_segv=0:handle_abort=0"
        EXPECT_EXIT 1
        EXPECT_JSON_PATH ok error.name error.details.step error.details.phase
            error.details.signal_name
        EXPECT_JSON_VALUE false MODULE_CRASHED a invoke SIGSEGV
        AFTER_CMD status "${_praenora_isolation_out}/crash/status.json")
    praenora_add_cli_test(cli.run.isolation_interrupted
        CLEAN_DIR "${_praenora_isolation_out}/interrupted"
        CMD run "${_praenora_cli_configs}/interrupted/workflow.yaml" ${_praenora_fixture_paths}
            --isolation process
            --status-file "${_praenora_isolation_out}/interrupted/status.json"
        INTERRUPT 1
        READY_FILE "${_praenora_isolation_out}/interrupted/status.json"
        EXPECT_EXIT 130
        EXPECT_STDOUT_REGEX "state +stopped"
        EXPECT_STDERR_REGEX "error\\[RUN_STOPPED\\]")
endif()

# Configuration, module and output directories whose names hold spaces and non-ASCII characters,
# laid out by each test in a directory of its own.
foreach(_praenora_command IN ITEMS validate run)
    set(_praenora_unicode "${_praenora_cli_out}/päths with spaces (${_praenora_command})")
    set(_praenora_unicode_files
        "${_praenora_cli_configs}/paths/workflow.yaml"
        "${_praenora_unicode}/cönfig dir/workflow.yaml"
        "${_praenora_cli_configs}/source.json" "${_praenora_unicode}/cönfig dir/source.json"
        "${PRAENORA_TEST_MODULES_DIR}/fx-echo/fx-echo.pmm.json"
        "${_praenora_unicode}/mödules ü/fx-echo/fx-echo.pmm.json"
        "$<TARGET_FILE:fx_echo>"
        "${_praenora_unicode}/mödules ü/fx-echo/$<TARGET_FILE_NAME:fx_echo>")
    set(_praenora_unicode_paths
        "${_praenora_unicode}/cönfig dir/workflow.yaml"
        --module-path "${_praenora_unicode}/mödules ü"
        --catalog-path "${_praenora_fixtures}/catalog")
    if(_praenora_command STREQUAL "validate")
        praenora_add_cli_test(cli.validate.unicode_paths
            CLEAN_DIR "${_praenora_unicode}"
            COPY_FILES ${_praenora_unicode_files}
            CMD validate ${_praenora_unicode_paths}
            EXPECT_STDOUT_REGEX "cönfig dir/workflow\\.yaml: valid")
    else()
        praenora_add_cli_test(cli.run.unicode_paths
            CLEAN_DIR "${_praenora_unicode}"
            COPY_FILES ${_praenora_unicode_files}
            CMD run ${_praenora_unicode_paths} --output-dir "${_praenora_unicode}/öutput ñ"
            EXPECT_STDOUT_REGEX
                "output directory +[^\n]*${_praenora_separator}öutput ñ\nstate +completed"
            EXPECT_FILE "${_praenora_unicode}/öutput ñ/run-record.json")
    endif()
endforeach()

# A missing input file exits 74 from every command.
set(_praenora_missing "${_praenora_cli_out}/missing")
praenora_add_cli_test(cli.validate.missing_file
    CMD validate "${_praenora_missing}/workflow.yaml"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.run.missing_file
    CMD run "${_praenora_missing}/workflow.yaml"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.graph.missing_file
    CMD graph "${_praenora_missing}/workflow.yaml"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.status.missing_file
    CMD status "${_praenora_missing}/status.json"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.modules.check_missing_manifest
    CMD modules check "${_praenora_missing}/module.pmm.json"
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]")
praenora_add_cli_test(cli.modules.check_missing_vectors
    CMD modules check "${PRAENORA_TEST_MODULES_DIR}/fx-echo/fx-echo.pmm.json"
        --vectors "${_praenora_missing}/vectors.json" ${_praenora_fixture_paths}
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX "cannot read the test vectors")
# An empty path exits 74 too: it reaches the tool as an argument of its own. The failure names it
# '', as the diagnostic does, in the text and in the JSON.
praenora_add_cli_test(cli.validate.empty_path
    CMD validate ""
    EXPECT_EXIT 74
    EXPECT_STDERR_REGEX
        "error\\[IO_ERROR\\]: cannot read ''.*\npraenora: '': validation failed: 1 error\\(s\\)\n")
praenora_add_cli_test(cli.validate.empty_document_path
    CMD validate --document "" --json
    EXPECT_EXIT 74
    EXPECT_JSON_PATH ok error.name error.message
    EXPECT_JSON_VALUE false IO_ERROR "'': validation failed: 1 error(s)")
# A missing data file is a diagnostic of the validation report, TARGET_DATA_UNAVAILABLE: the tool
# exits with 3, as for every report that holds an error other than IO_ERROR.
set(_praenora_missing_data "${_praenora_cli_configs}/missing-data/workflow.yaml")
praenora_add_cli_test(cli.validate.missing_data
    CMD validate "${_praenora_missing_data}" ${_praenora_fixture_paths} --json
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name error.code diagnostics.0.name diagnostics.0.location.pointer
    EXPECT_JSON_VALUE false TARGET_DATA_UNAVAILABLE 801 TARGET_DATA_UNAVAILABLE
        /inputs/source/path)
praenora_add_cli_test(cli.run.missing_data
    CMD run "${_praenora_missing_data}" ${_praenora_fixture_paths}
    EXPECT_EXIT 3
    EXPECT_STDERR_REGEX "workflow\\.yaml:9:[0-9]+: error\\[TARGET_DATA_UNAVAILABLE\\]")

# Warnings of the validation: two rows of the data file are before the run's start, so validation
# reports one warning (TARGET_DATA_INVALID, severity warning, at the first of them) and the
# configuration is still valid. validate shows it, run shows it too (standard error, and
# "diagnostics" in the JSON result), and the run record keeps the counts outside the hashed
# section (execution.inputs) and is itself valid.
set(_praenora_early_rows "${_praenora_cli_configs}/early-rows/workflow.yaml")
praenora_add_cli_test(cli.validate.rows_before_start
    CMD validate "${_praenora_early_rows}" ${_praenora_fixture_paths}
    EXPECT_STDOUT_REGEX "valid \\(1 warning\\(s\\)\\)"
    EXPECT_STDERR_REGEX
        "obs\\.csv:3: warning\\[TARGET_DATA_INVALID\\].*2 of 4 rows have a time before run\\.time\\.start")
praenora_add_cli_test(cli.validate.rows_before_start_json
    CMD validate "${_praenora_early_rows}" ${_praenora_fixture_paths} --json
    EXPECT_JSON_PATH ok result.ok result.diagnostics.0.severity result.diagnostics.0.name
        result.diagnostics.0.location.line
    EXPECT_JSON_VALUE true true warning TARGET_DATA_INVALID 3)
# Every row before the start: still one warning, "2 of 2 rows", and the exit code is 0.
praenora_add_cli_test(cli.validate.all_rows_before_start
    CMD validate "${_praenora_cli_configs}/early-rows-all/workflow.yaml" ${_praenora_fixture_paths}
        --json
    EXPECT_JSON_PATH ok result.ok result.diagnostics.0.severity result.diagnostics.0.message
    EXPECT_JSON_VALUE true true warning *
    EXPECT_STDERR_REGEX "2 of 2 rows have a time before run\\.time\\.start")
set(_praenora_early_rows_out "${_praenora_cli_out}/early-rows")
praenora_add_cli_test(cli.run.rows_before_start
    CLEAN_DIR "${_praenora_early_rows_out}"
    CMD run "${_praenora_early_rows}" ${_praenora_fixture_paths}
        --output-dir "${_praenora_early_rows_out}" --json
    EXPECT_JSON_PATH ok result.state result.diagnostics.0.severity result.diagnostics.0.name
        result.diagnostics.0.location.line
    EXPECT_JSON_VALUE true completed warning TARGET_DATA_INVALID 3
    EXPECT_STDERR_REGEX "warning\\[TARGET_DATA_INVALID\\]"
    EXPECT_FILE "${_praenora_early_rows_out}/run-record.json"
    EXPECT_FILE_JSON_PATH execution.inputs.source.rows_total
        execution.inputs.source.rows_before_start
    EXPECT_FILE_JSON_VALUE 4 2
    AFTER_CMD validate --document "${_praenora_early_rows_out}/run-record.json")

# A target descriptor (ADR-0042) names the CSV: no path in the configuration. The row of the
# covariate age is warned about once (TARGET_DATA_INVALID, severity warning, at its CSV line) and
# never delivered; the run record has the descriptor's id and the counts, validates, and the
# reproducibility section has the descriptor's digest.
set(_praenora_descriptor "${_praenora_cli_configs}/descriptor/workflow.yaml")
praenora_add_cli_test(cli.validate.descriptor
    CMD validate "${_praenora_descriptor}" ${_praenora_fixture_paths}
    EXPECT_STDOUT_REGEX "valid \\(1 warning\\(s\\)\\)"
    EXPECT_STDERR_REGEX
        "obs\\.csv:3: warning\\[TARGET_DATA_INVALID\\].*1 of 3 rows belong to variables whose role is not response")
praenora_add_cli_test(cli.validate.descriptor_json
    CMD validate "${_praenora_descriptor}" ${_praenora_fixture_paths} --json
    EXPECT_JSON_PATH ok result.ok result.diagnostics.0.severity result.diagnostics.0.name
        result.diagnostics.0.location.line
    EXPECT_JSON_VALUE true true warning TARGET_DATA_INVALID 3)
set(_praenora_descriptor_out "${_praenora_cli_out}/descriptor")
praenora_add_cli_test(cli.run.descriptor
    CLEAN_DIR "${_praenora_descriptor_out}"
    CMD run "${_praenora_descriptor}" ${_praenora_fixture_paths}
        --output-dir "${_praenora_descriptor_out}" --json
    EXPECT_JSON_PATH ok result.state result.diagnostics.0.severity
    EXPECT_JSON_VALUE true completed warning
    EXPECT_FILE "${_praenora_descriptor_out}/run-record.json"
    EXPECT_FILE_JSON_PATH execution.inputs.source.rows_total
        execution.inputs.source.rows_not_delivered_by_role execution.inputs.source.descriptor_id
        reproducibility.inputs.source.descriptor.schema_version
    EXPECT_FILE_JSON_VALUE 3 1 cli-descriptor 0.0.0
    AFTER_CMD validate --document "${_praenora_descriptor_out}/run-record.json")
set(_praenora_descriptor_bad_sha "${_praenora_cli_configs}/descriptor-bad-sha/workflow.yaml")
praenora_add_cli_test(cli.validate.descriptor_bad_sha
    CMD validate "${_praenora_descriptor_bad_sha}" ${_praenora_fixture_paths} --json
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name diagnostics.0.name diagnostics.0.location.pointer
    EXPECT_JSON_VALUE false TARGET_DATA_INVALID TARGET_DATA_INVALID /data/sha256)

# A warning about the descriptor itself has no line: it is located by a JSON pointer, which the text
# shows after the code and the JSON has as location.pointer (and no location.line).
set(_praenora_descriptor_no_unit "${_praenora_cli_configs}/descriptor-no-time-unit/workflow.yaml")
praenora_add_cli_test(cli.validate.descriptor_no_time_unit
    CMD validate "${_praenora_descriptor_no_unit}" ${_praenora_fixture_paths}
    EXPECT_STDOUT_REGEX "valid \\(1 warning\\(s\\)\\)"
    EXPECT_STDERR_REGEX
        "target\\.json: warning\\[TARGET_DATA_INVALID\\]: /time_unit: input 'source': the descriptor has no time_unit")
praenora_add_cli_test(cli.validate.descriptor_no_time_unit_json
    CMD validate "${_praenora_descriptor_no_unit}" ${_praenora_fixture_paths} --json
    EXPECT_JSON_PATH ok result.diagnostics.0.severity result.diagnostics.0.location.pointer
    EXPECT_JSON_VALUE true warning /time_unit)

# Service providers. fx-service requires test.echo@0, which the fixture provider test-echo serves
# (configuration service/workflow.yaml). providers/old-runtime.provider.json is the manifest of a
# provider built for runtime 0.0.1 and linux-x86_64, which no runtime of this repository can use,
# and providers/shadowed/ holds another version of it, which the first shadows; their libraries do
# not exist, as listing loads nothing. The tests that need no provider to be found set
# PRAENORA_PROVIDER_PATH empty, which counts as unset.
set(_praenora_echo_provider --provider-path "${PRAENORA_TEST_PROVIDERS_DIR}/test-echo")
set(_praenora_service_config "${_praenora_cli_configs}/service/workflow.yaml")
set(_praenora_service_manifest "${PRAENORA_TEST_MODULES_DIR}/fx-service/fx-service.pmm.json")
set(_praenora_no_providers "PRAENORA_PROVIDER_PATH=")

# The text table: name, version, platform, services, manifest, and "usable" or why not.
set(_praenora_listed_echo "^test-echo +0\\.1\\.0 +[a-z]+-[a-z0-9_]+ +test\\.echo@0\\.0 +")
string(APPEND _praenora_listed_echo
    "[^\n]*${_praenora_separator}test-echo\\.provider\\.json +usable\n")
praenora_add_cli_test(cli.providers.list
    CMD providers list ${_praenora_echo_provider}
    EXPECT_STDOUT_REGEX "${_praenora_listed_echo}")
praenora_add_cli_test(cli.providers.list_json
    CMD providers list ${_praenora_echo_provider} --json
    EXPECT_JSON_PATH ok result.0.name result.0.version result.0.platform.os result.0.manifest
        result.0.library result.0.services.0.name result.0.services.0.major
        result.0.services.0.minor result.0.engine.name result.0.usable
    EXPECT_JSON_VALUE true test-echo 0.1.0 * * * test.echo 0 0 test-echo-engine true)
# Without --provider-path, PRAENORA_PROVIDER_PATH names the provider search path.
praenora_add_cli_test(cli.providers.list_environment
    CMD providers list --json
    ENVIRONMENT "PRAENORA_PROVIDER_PATH=${PRAENORA_TEST_PROVIDERS_DIR}/test-echo"
    EXPECT_JSON_PATH result.0.name
    EXPECT_JSON_VALUE test-echo)
# An unusable provider is listed with the reason.
praenora_add_cli_test(cli.providers.list_unusable
    CMD providers list --provider-path "${_praenora_cli_configs}/providers" --json
    EXPECT_JSON_PATH result.0.name result.0.usable result.0.problem
    EXPECT_JSON_VALUE test-old-runtime false *)
praenora_add_cli_test(cli.providers.list_unusable_text
    CMD providers list --provider-path "${_praenora_cli_configs}/providers"
    EXPECT_STDOUT_REGEX "^test-old-runtime +0\\.1\\.0 +[^\n]* unusable: built for ")
# A manifest shadowed by an earlier one of its name and platform is listed, unusable, naming it.
set(_praenora_shadowing
    "${_praenora_separator}providers${_praenora_separator}old-runtime\\.provider\\.json")
set(_praenora_listed_shadowed "\ntest-old-runtime +0\\.2\\.0 +[^\n]* unusable: shadowed by ")
string(APPEND _praenora_listed_shadowed "'[^'\n]*${_praenora_shadowing}'")
praenora_add_cli_test(cli.providers.list_shadowed
    CMD providers list --provider-path "${_praenora_cli_configs}/providers"
    EXPECT_STDOUT_REGEX "${_praenora_listed_shadowed}")
praenora_add_cli_test(cli.providers.list_shadowed_json
    CMD providers list --provider-path "${_praenora_cli_configs}/providers" --json
    EXPECT_JSON_PATH result.1.name result.1.version result.1.usable result.1.problem
    EXPECT_JSON_VALUE test-old-runtime 0.2.0 false *
    EXPECT_STDOUT_REGEX "\"problem\":\"shadowed by '[^']*${_praenora_shadowing}'")
# Nothing found: a note on standard error, an empty list and exit code 0. The build tree's
# installation provider directory holds no provider, unless the build has the roadrunner provider
# (PRAENORA_WITH_ROADRUNNER) or the antimony provider (PRAENORA_WITH_ANTIMONY).
if(NOT PRAENORA_WITH_ROADRUNNER AND NOT PRAENORA_WITH_ANTIMONY)
    praenora_add_cli_test(cli.providers.list_empty
        CMD providers list --json
        ENVIRONMENT "${_praenora_no_providers}"
        EXPECT_STDOUT_REGEX "^\\{\"ok\":true,\"result\":\\[\\]\\}\n$"
        EXPECT_STDERR_REGEX "no provider manifests on the provider search paths")
endif()
praenora_add_cli_test(cli.providers.list_takes_no_arguments
    CMD providers list extra
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "providers list takes no arguments")
praenora_add_cli_test(cli.providers.help
    CMD help providers
    EXPECT_STDOUT_REGEX "^usage: praenora providers list\n")
praenora_add_cli_test(cli.usage.tool_help_lists_providers
    CMD --help
    EXPECT_STDOUT_REGEX "\n  providers list +the service providers")
praenora_add_cli_test(cli.usage.provider_path_option
    CMD validate --help
    EXPECT_STDOUT_REGEX "\n  --provider-path P +search P for service providers first")
praenora_add_cli_test(cli.usage.provider_path_needs_value
    CMD providers list --provider-path
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "option --provider-path needs a value")

# A service that no provider serves fails validation (exit code 3), and so a run; with --json,
# each diagnostic carries its details.
praenora_add_cli_test(cli.validate.service_unavailable
    CMD validate "${_praenora_service_config}" ${_praenora_fixture_paths} --json
    ENVIRONMENT "${_praenora_no_providers}"
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name error.code diagnostics.0.name diagnostics.0.location.pointer
        diagnostics.0.details.service diagnostics.0.details.major diagnostics.0.details.providers
    EXPECT_JSON_VALUE false SERVICE_UNAVAILABLE 407 SERVICE_UNAVAILABLE /requires/services/0
        test.echo 0 *)
praenora_add_cli_test(cli.run.service_unavailable
    CMD run "${_praenora_service_config}" ${_praenora_fixture_paths} --json
    ENVIRONMENT "${_praenora_no_providers}"
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name diagnostics.0.name diagnostics.0.details.service
    EXPECT_JSON_VALUE false SERVICE_UNAVAILABLE SERVICE_UNAVAILABLE test.echo)
praenora_add_cli_test(cli.validate.provider_path
    CMD validate "${_praenora_service_config}" ${_praenora_fixture_paths} ${_praenora_echo_provider}
        --load
    ENVIRONMENT "${_praenora_no_providers}"
    EXPECT_STDOUT_REGEX "service/workflow\\.yaml: valid")
praenora_add_cli_test(cli.run.provider_path
    CMD run "${_praenora_service_config}" ${_praenora_fixture_paths} ${_praenora_echo_provider}
    ENVIRONMENT "${_praenora_no_providers}"
    EXPECT_STDOUT_REGEX "state +completed")
# The contract checks resolve the module's services on the provider search paths.
praenora_add_cli_test(cli.modules.check_service
    CMD modules check "${_praenora_service_manifest}" ${_praenora_fixture_paths}
        ${_praenora_echo_provider}
    ENVIRONMENT "${_praenora_no_providers}"
    EXPECT_STDOUT_REGEX
        "pass +library\\.load +[^\n]*test\\.echo@0 from provider test-echo@0\\.1\\.0")
praenora_add_cli_test(cli.modules.check_service_unavailable
    CMD modules check "${_praenora_service_manifest}" ${_praenora_fixture_paths}
    ENVIRONMENT "${_praenora_no_providers}"
    EXPECT_EXIT 1
    EXPECT_STDOUT_REGEX "fail +library\\.load +SERVICE_UNAVAILABLE: ")

praenora_add_cli_test(cli.catalog.list
    CMD catalog list
    EXPECT_STDOUT_REGEX "workflow-definition +example\\.estimate-predict +0\\.1\\.0")
praenora_add_cli_test(cli.catalog.show_example
    CMD catalog show example.estimate-predict@0.1.0 --json
    EXPECT_JSON_PATH result.kind result.name
    EXPECT_JSON_VALUE workflow-definition example.estimate-predict)

praenora_add_cli_test(cli.schema.list
    CMD schema list --json
    EXPECT_JSON_PATH result
    EXPECT_JSON_LENGTH 19)
praenora_add_cli_test(cli.schema.show_manifest
    CMD schema show module-manifest
    EXPECT_STDOUT_REGEX "\"\\$id\": \"urn:praenora:schema:module-manifest:0\\.2\"")

praenora_add_cli_test(cli.usage.unknown_command
    CMD frobnicate
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "unknown command 'frobnicate'")
praenora_add_cli_test(cli.usage.command_help
    CMD run --help
    EXPECT_STDOUT_REGEX "usage: praenora run <config.yaml>")
praenora_add_cli_test(cli.usage.invalid_log_format
    CMD catalog list --log-format xml
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "--log-format 'xml' is not a log format: text or json\n")
praenora_add_cli_test(cli.usage.invalid_log_level
    CMD catalog list --log-level loud
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX
        "--log-level 'loud' is not a log level: trace, debug, info, warn, error, critical or off\n")

# The usage texts and the messages list the names that the tool takes from the tables that it
# shares with the Python extension (src/frontend), in the tables' order.
praenora_add_cli_test(cli.usage.tool_help_lists_graph_formats
    CMD --help
    EXPECT_STDOUT_REGEX "\n +a workflow definition's graph: text, json, dot or asyncapi\n")
praenora_add_cli_test(cli.usage.global_options_list_log_names
    CMD status --help
    EXPECT_STDOUT_REGEX
        "L +trace, debug, info, warn, error, critical or off\n  --log-format F +text or json\n")
praenora_add_cli_test(cli.catalog.help_lists_kinds
    CMD catalog --help
    EXPECT_STDOUT_REGEX
        "^usage: praenora catalog list \\[--kind workflows\\|capabilities\\|messages\\]\n")
praenora_add_cli_test(cli.catalog.unknown_kind
    CMD catalog list --kind workflow
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "--kind 'workflow' is not workflows, capabilities or messages\n")
praenora_add_cli_test(cli.graph.help_lists_formats
    CMD graph --help
    EXPECT_STDOUT_REGEX
        "\\[--format text\\|json\\|dot\\|asyncapi\\].*  --format F +text, json, dot or asyncapi\n")
praenora_add_cli_test(cli.graph.unknown_format
    CMD graph --workflow example.estimate-predict@0.1.0 --format svg
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "--format 'svg' is not text, json, dot or asyncapi\n")
praenora_add_cli_test(cli.run.help_lists_log_formats
    CMD run --help
    EXPECT_STDOUT_REGEX " \\[--log-format text\\|json\\]\n")
praenora_add_cli_test(cli.run.help_lists_isolation_modes
    CMD run --help
    EXPECT_STDOUT_REGEX
        "\\[--isolation in-process\\|process\\].*  --isolation M +in-process or process")
praenora_add_cli_test(cli.run.isolation_unknown_mode
    CMD run "${_praenora_cli_configs}/module-failure/workflow.yaml" --isolation thread
    EXPECT_EXIT 2
    EXPECT_STDERR_REGEX "--isolation 'thread' is not in-process or process\n")

# --help with --json: the usage is the result of the usual success document.
praenora_add_cli_test(cli.json.help
    CMD run --help --json
    EXPECT_JSON_PATH ok result
    EXPECT_JSON_VALUE true *
    EXPECT_STDOUT_REGEX "^\\{\"ok\":true,\"result\":\"usage: praenora run <config.yaml>")
praenora_add_cli_test(cli.json.help_command
    CMD help modules --json
    EXPECT_JSON_PATH ok result
    EXPECT_JSON_VALUE true *
    EXPECT_STDOUT_REGEX "usage: praenora modules list")

praenora_add_cli_test(cli.json.error_shape
    CMD catalog show no.such-entry@0.1.0 --json
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name error.code diagnostics
    EXPECT_JSON_VALUE false CATALOG_ENTRY_NOT_FOUND 405 *)
praenora_add_cli_test(cli.json.validation_failure
    CMD validate "${_praenora_cli_configs}/bad-parameters/workflow.yaml" ${_praenora_fixture_paths}
        --json
    EXPECT_EXIT 3
    EXPECT_JSON_PATH ok error.name diagnostics.0.name diagnostics.0.location.pointer
    EXPECT_JSON_VALUE false PARAMETERS_INVALID PARAMETERS_INVALID /steps/a/parameters/sleep_ms)

# The logging flags beat the environment: the tool puts them into its own environment before the
# runtime reads it, so an invalid PRAENORA_LOG_LEVEL does not matter when --log-level is given.
praenora_add_cli_test(cli.logging.flag_beats_environment
    CMD catalog list --log-level warn
    ENVIRONMENT "PRAENORA_LOG_LEVEL=loud")
praenora_add_cli_test(cli.logging.invalid_environment
    CMD catalog list
    ENVIRONMENT "PRAENORA_LOG_LEVEL=loud"
    EXPECT_EXIT 3
    EXPECT_STDERR_REGEX "PRAENORA_LOG_LEVEL")

if(PRAENORA_BUILD_EXAMPLES)
    set(_praenora_example_modules --module-path "${PRAENORA_EXAMPLE_MODULES_DIR}")

    praenora_add_cli_test(cli.validate.example_ok
        CMD validate "${_praenora_example_config}" ${_praenora_example_modules}
        EXPECT_STDOUT_REGEX "valid")

    praenora_add_cli_test(cli.modules.list_json
        CMD modules list ${_praenora_example_modules} --json
        EXPECT_JSON_PATH ok
        EXPECT_JSON_VALUE true
        EXPECT_STDOUT_REGEX "\"name\":\"example-estimator\"")
    praenora_add_cli_test(cli.modules.inspect_load
        CMD modules inspect example-estimator --load ${_praenora_example_modules} --json
        EXPECT_JSON_PATH result.describe.capabilities result.library_sha256)
    praenora_add_cli_test(cli.modules.check_example
        CMD modules check
            "${PRAENORA_EXAMPLE_MODULES_DIR}/example-estimator/example-estimator.pmm.json"
        EXPECT_STDOUT_REGEX "pass +invoke\\.expected_outputs")

    praenora_add_cli_test(cli.graph.dot
        CMD graph "${_praenora_example_config}" --format dot
        EXPECT_STDOUT_REGEX "estimate.*->.*predict")

    praenora_add_cli_test(cli.run.example
        CLEAN_DIR "${_praenora_cli_out}/example"
        CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/example"
        EXPECT_STDOUT_REGEX "fingerprint +[0-9a-f]+\nticks +12\noutput directory +[^\n]*example"
        EXPECT_FILE "${_praenora_cli_out}/example/run-record.json")
    # An output directory that holds an earlier run is refused (IO_ERROR, exit 74), unless the
    # run may overwrite it.
    praenora_add_cli_test(cli.run.existing_output_dir
        CLEAN_DIR "${_praenora_cli_out}/existing"
        SETUP_CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/existing"
        CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/existing"
        EXPECT_EXIT 74
        EXPECT_STDERR_REGEX "error\\[IO_ERROR\\]"
        AFTER_CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/existing" --overwrite
        AFTER_EXPECT_EXIT 0)

    praenora_add_cli_test(cli.status.reads_file
        CLEAN_DIR "${_praenora_cli_out}/status"
        SETUP_CMD run "${_praenora_example_config}" ${_praenora_example_modules}
            --output-dir "${_praenora_cli_out}/status"
            --status-file "${_praenora_cli_out}/status/status.json"
        CMD status "${_praenora_cli_out}/status/status.json" --json
        EXPECT_JSON_PATH result.kind result.workflow.name result.workflow.state
        EXPECT_JSON_VALUE status-snapshot example.estimate-predict completed)
endif()

# The driver itself (check_driver.cmake): cli.driver.passes_empty_arguments, each argument of a
# command reaches its program as it is, an empty one included; cli.driver.shows_failing_command,
# when a command fails, it shows the command on one line, with each argument that is empty or
# holds whitespace or a quote character shown in double quotes, and its outputs line by line as
# they were printed.
foreach(check IN ITEMS passes_empty_arguments shows_failing_command)
    add_test(NAME cli.driver.${check}
        COMMAND "${CMAKE_COMMAND}" -DCHECK=${check}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_driver.cmake")
    set_tests_properties(cli.driver.${check} PROPERTIES LABELS cli)
endforeach()

# Unit tests of the tool's own helpers (src/cli) and of what it shares with the Python extension
# (praenora_frontend, src/frontend), all header-only.
praenora_add_gtest(praenora_cli_tests LABEL cli
    SOURCES table_test.cpp frontend_test.cpp
    LINK praenora_frontend)
target_include_directories(praenora_cli_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/cli")
