#!/usr/bin/env bash

# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

# Example (assuming source directory stk is in home directory): 
# ~/stk/coverage.sh -s ~/stk -b ~/stk-build -o ~/stk-coverage -f ~/stk-build/coverage.info

SRC_DIR=""
BUILD_DIR=""
OUTPUT_DIR=""
COVERAGE_FILE=""
SHOW_IN_WEBBROWSER=false

ShowHelp()
{
   echo "Generate STK Coverage HTML report with gcov/lcov."
   echo
   echo "Syntax: coverage [-h|s|b|o|w]"
   echo "options:"
   echo "h       Show help."
   echo "s DIR   Source directory."
   echo "b DIR   Build directory."
   echo "o DIR   Output directory (location of HTML report)."
   echo "f PATH  Fullpath to the coverage info file including the filename."
   echo "w       Open generated HTML report in the FireFox web browser."
   echo
}

CheckArgs()
{
    if [ -z "$SRC_DIR" ] ; then
        echo "Failed: no source directory provided!" && exit 1
    fi
    if [ -z "$BUILD_DIR" ] ; then
        echo "Failed: no build directory provided!" && exit 1
    fi
    if [ -z "$OUTPUT_DIR" ] ; then
        echo "Failed: no output directory provided!" && exit 1
    fi
    if [ -z "$COVERAGE_FILE" ] ; then
        echo "Failed: no coverage.info file and path provided!" && exit 1
    fi
}

while getopts ":hs:b:o:f:w" option; do
    case $option in
        h) ShowHelp && exit;;
        s) SRC_DIR=$OPTARG;;
        b) BUILD_DIR=$OPTARG;;
        o) OUTPUT_DIR=$OPTARG;;
        f) COVERAGE_FILE=$OPTARG;;
        w) SHOW_IN_WEBBROWSER=true;;
        \?) echo "Failed: invalid option! Option: $option" && exit 1;;
    esac
done

COVERAGE_FILE_DRAFT="$COVERAGE_FILE.draft"

CheckArgs

# Configure
cmake -S $SRC_DIR -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release -DBUILD_LIB=ON -DBUILD_TESTS=ON -DTEST_GENERIC=ON -DENABLE_TLS=ON -DENABLE_COVERAGE=ON

# Build
cmake --build $BUILD_DIR --config Release --parallel 4

# Cleanup
lcov --directory $BUILD_DIR --zerocounters

# Run tests
ctest --test-dir $BUILD_DIR -C Release

# Accumulate results
cmake --build $BUILD_DIR --target ExperimentalCoverage

# Capture results
lcov --capture --follow --directory $BUILD_DIR --output-file $COVERAGE_FILE_DRAFT

# Cleanup (we need only STK)
lcov --remove $COVERAGE_FILE_DRAFT "*/deps/*" "*/test/*" "/usr/*" --output-file $COVERAGE_FILE

# Produce HTML
genhtml $COVERAGE_FILE --output-directory $OUTPUT_DIR

# Open in browser
if $SHOW_IN_WEBBROWSER ; then
    xdg-open $OUTPUT_DIR/index.html
fi
