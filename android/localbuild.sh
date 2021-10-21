#!/bin/bash

usage() {
  echo "usage: $0 [-p platform][--platform platform] [-f flavor][--flavor flavor]"
  echo "-p platform, --platform platform:         the platform to build for (platform options: x64 (default), x86, arm, arm64)"
  echo "-f flavor, --flavor flavor:               the build flavor (flavor options: debug (default), release)"
  echo "-h, --help:                               this help message"
}

# defining params

BUILD_PLATFORM="x64" # default
BUILD_FLAVOR="debug"
SOURCE_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
SOURCES_PATH="$SOURCE_DIR" # this won't be supplied by the user; should be android folder
OUTPUT_PATH="$SOURCE_DIR/out"

while [ "${1:-}" != "" ]; do
  case "$1" in
    "-p" | "--platform")
      shift
      case "$1" in
        "x64" | "x86" | "arm" | "arm64")
          BUILD_PLATFORM="$1"
          ;;
        *)
          echo "Invalid option: $1"
          usage
          exit 1
          ;;
      esac
      ;;
    "-f" | "--flavor")
      shift
      case "$1" in
        "debug" | "release")
          BUILD_FLAVOR="$1"
          ;;
        *)
          echo "Invalid option: $1"
          usage
          exit 1
          ;;
      esac
      ;;
    "-h" | "--help")
      usage
      exit 0
      ;;
    *)
      echo "Invalid flag: $1"
      usage
      exit 1
      ;;
  esac
  shift
done

./scripts/build.sh -p $BUILD_PLATFORM -f $BUILD_FLAVOR -s $SOURCES_PATH -o $OUTPUT_PATH
