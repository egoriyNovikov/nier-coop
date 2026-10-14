@echo off
rem flatc is built with the project: cmake --build build --config Release --target flatc
set FLATC=build\_deps\flatbuffers-build\Release\flatc.exe
%FLATC% --go --gen-all -o "server/automatamp" "schema\Packets.fbs"
%FLATC% --cpp --gen-all -o "src/mods/multiplayer/schema" "schema\Packets.fbs"
