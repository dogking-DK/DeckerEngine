add_test(NAME dk.bootstrap.version COMMAND dk_run --version)
string(REPLACE "." "\\." _dk_version_regex "${PROJECT_VERSION}")
set_tests_properties(dk.bootstrap.version PROPERTIES
    PASS_REGULAR_EXPRESSION "DeckerEngine ${_dk_version_regex}"
)

