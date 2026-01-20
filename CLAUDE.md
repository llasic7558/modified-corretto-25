# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

Amazon Corretto 25 - a no-cost, production-ready distribution of OpenJDK 25. The `develop` branch tracks upstream [openjdk/jdk:jdk25](https://github.com/openjdk/jdk) with Amazon-specific patches.

## Build Commands

```bash
# Configure and build
bash configure                    # Run configuration (add --enable-debug for debug build)
make images                       # Build JDK image (primary target)

# Verify build
./build/*/images/jdk/bin/java -version

# Common configure options
bash configure --enable-debug                    # Debug build
bash configure --with-boot-jdk=/path/to/jdk24   # Specify boot JDK
bash configure --with-conf-name=my-build        # Named configuration
```

Build output: `build/<platform>/images/jdk/`

## Test Commands

```bash
# Tiered testing (tier1 = quick validation, tier4 = stress tests)
make test-tier1                   # Basic tests (~5-10 min)
make test-tier2                   # Extended tests
make test-tier3                   # Comprehensive tests

# Specific test targets
make test TEST="hotspot:hotspot_gc"              # GC tests only
make test TEST="jdk_lang"                        # Java language tests
make test-hotspot-gtest                          # HotSpot C++ unit tests

# Run single test file
make test TEST="jtreg:test/hotspot/jtreg/gc/g1/TestG1ConcurrentStart.java"

# Test with options
make test TEST=tier1 JTREG="JOBS=8"              # Parallel execution
make test TEST=hotspot_gc JTREG="TIMEOUT_FACTOR=8"

# Skip rebuild (faster iteration)
make test-tier1-only
```

## Source Architecture

### HotSpot VM (`src/hotspot/`)
```
share/          # Platform-independent code
├── gc/         # Garbage collectors (G1, ZGC, Shenandoah, Serial, Parallel)
├── oops/       # Object representation (ordinary object pointers)
├── runtime/    # VM runtime (threads, synchronization, safepoints)
├── memory/     # Memory management infrastructure
├── compiler/   # JIT compiler interface
├── opto/       # C2 optimizing compiler
├── c1/         # C1 client compiler
├── interpreter/# Bytecode interpreter
├── classfile/  # Class file parsing and verification
├── prims/      # JNI, JVMTI, unsafe operations
├── jfr/        # Java Flight Recorder
├── cds/        # Class Data Sharing
├── services/   # Management and monitoring
cpu/            # CPU-specific code (x86, aarch64, etc.)
os/             # OS-specific code (linux, macos, windows, etc.)
os_cpu/         # OS+CPU combinations
```

### Java Modules (`src/`)
- `java.base/` - Core runtime (java.lang, java.util, java.io)
- `java.compiler/` - Compiler API
- `jdk.compiler/` - javac implementation
- `jdk.graal.compiler/` - Graal JIT compiler
- `jdk.hotspot.agent/` - Serviceability Agent (debugging)
- `jdk.jfr/` - Flight Recorder runtime

### Test Structure (`test/`)
```
hotspot/jtreg/  # HotSpot regression tests (JTReg framework)
hotspot/gtest/  # HotSpot C++ unit tests (Google Test)
jdk/            # JDK library tests
langtools/      # Compiler and tools tests
micro/          # JMH microbenchmarks
```

## Build System

- **Entry point**: `Makefile` → `make/Main.gmk`
- **Configuration**: `configure` (autoconf-based)
- **Key Make targets**: `images`, `jdk`, `test-tier1`, `docs`, `clean`
- **Build phases**: `gensrc` → `java` → `libs` → `launchers` → `jmods` → `images`

Incremental rebuilds: Most targets support `-only` suffix (e.g., `make test-tier1-only`)

## Requirements

- Boot JDK: Java 24 (N-1 for building JDK N)
- GNU Make 3.81+, Bash, Autoconf 2.69+
- Compiler: gcc 10+, clang 13+, or MSVC 2019+
- JTReg for running tests (configure with `--with-jtreg=<path>`)

## Key Documentation

- `doc/building.md` - Comprehensive build instructions
- `doc/testing.md` - Test framework documentation
- `doc/hotspot-style.md` - HotSpot C++ coding standards
- `CHANGELOG.md` - Corretto-specific changes

## Development Notes

- Work against the `develop` branch for contributions
- Run `make test-tier1` before submitting PRs
- Problem tests tracked in `test/ProblemList*.txt`
- Security issues: report via AWS vulnerability reporting, not GitHub issues
