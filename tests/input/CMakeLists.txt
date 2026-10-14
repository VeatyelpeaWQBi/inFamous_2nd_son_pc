# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
# Standalone behavior checks: no Google Test, downloads, GPU, or game files.
cmake_minimum_required(VERSION 3.24)
project(shadps4_input_port_test LANGUAGES CXX)
enable_testing()
add_executable(shadps4_input_port_test test_mouse_input.cpp)
target_compile_features(shadps4_input_port_test PRIVATE cxx_std_20)
target_include_directories(shadps4_input_port_test PRIVATE ../../src)
if(WIN32)
    # Compile the actual HID parser as well; tests do not register raw input devices.
    target_compile_definitions(shadps4_input_port_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
add_test(NAME mouse_input_port COMMAND shadps4_input_port_test)
