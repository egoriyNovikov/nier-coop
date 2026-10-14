#pragma once

#include <GitRevision.hpp>

// Bump on every test build (see "Журнал тестовых сборок" in docs/TASKS.md).
#define AUTOMATAMP_VERSION "0.3.1"

// Shown in the Co-op menu and sent to the server in Hello, e.g. "0.2.0+1a2b3c4d".
#define AUTOMATAMP_FULL_VERSION AUTOMATAMP_VERSION "+" AUTOMATAMP_GIT_REVISION
