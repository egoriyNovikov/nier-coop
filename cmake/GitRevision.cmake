# Writes the git revision of the sources into GitRevision.hpp. Runs on every build and only
# touches the file when the revision changes, so unchanged builds don't recompile anything.
execute_process(
	COMMAND git rev-parse --short=8 HEAD
	WORKING_DIRECTORY "${SOURCE_DIR}"
	OUTPUT_VARIABLE GIT_REVISION
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)

execute_process(
	COMMAND git status --porcelain --untracked-files=no
	WORKING_DIRECTORY "${SOURCE_DIR}"
	OUTPUT_VARIABLE GIT_CHANGES
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)

if(NOT GIT_REVISION)
	set(GIT_REVISION "unknown")
elseif(GIT_CHANGES)
	set(GIT_REVISION "${GIT_REVISION}-dirty")
endif()

set(CONTENT "#pragma once\n\n#define AUTOMATAMP_GIT_REVISION \"${GIT_REVISION}\"\n")

if(EXISTS "${OUTPUT_FILE}")
	file(READ "${OUTPUT_FILE}" OLD_CONTENT)
endif()

if(NOT CONTENT STREQUAL OLD_CONTENT)
	file(WRITE "${OUTPUT_FILE}" "${CONTENT}")
endif()
