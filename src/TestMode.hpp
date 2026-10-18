#pragma once

#include <filesystem>

// Test mode is on in a test copy of the game made by tools/qa/make_test_install.ps1 (coop_test.ini next to the exe).
// Implemented in DllMain.cpp, where the test copy is isolated from the owner's saves.
namespace testmode {
bool enabled();
// Folder of the test copy (next to the exe).
std::filesystem::path root();
}
