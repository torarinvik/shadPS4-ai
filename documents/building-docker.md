<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-FileCopyrightText: 2026 fork maintainers
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Building this shadPS4 fork with Docker and VSCode support

> [!IMPORTANT]
> These instructions are inherited from the upstream shadPS4 layout and are being
> adapted for this independent fork. Executable names and paths may still use
> `shadPS4`. Builds from this repository are not official shadPS4 builds.

This guide explains how to build this port using Docker while keeping full compatibility with **VSCode** development.

---

## Prerequisites

Before starting, ensure you have:

- **Docker Engine** or **Docker Desktop** installed  
  [Installation Guide](https://docs.docker.com/engine/install/)

- **Git** installed on your system.

---

## Step 1: Prepare the Docker Environment

Inside the container (or on your host if mounting volumes):

1. Navigate to the repository folder containing the Docker Builder folder:

```bash
cd <path-to-repo>
```

2. Start the Docker container:

```bash
docker compose up -d
```

This will spin up a container with all the necessary build dependencies, including Clang, CMake, SDL2, Vulkan, and more.

## Step 2: Clone this repository

```bash
mkdir emu
cd emu
git clone --recursive <this-repository-url> .
```

3. Initialize submodules:

```bash
git submodule update --init --recursive
```

## Step 3: Build with CMake Tools (GUI)

Generate build with CMake Tools.

1. Go `CMake Tools > Configure > '>'`
2. And `Build > '>'`

Compiled executable in `Build` folder.

## Alternative Step 3: Build with CMake

Generate the build directory and configure the project using Clang:

```bash
cmake -S . -B build/ -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
```

Then build the project:

```bash
cmake --build ./build --parallel $(nproc)
```

* Tip: To enable debug builds, add -DCMAKE_BUILD_TYPE=Debug to the CMake command.

---

After a successful build, the executable is located at:

```bash
./build/shadps4
```

## Step 4: VSCode Integration

1. Open the repository in VSCode.
2. The CMake Tools extension should automatically detect the build directory inside the container or on your host.
3. You can configure build options, build, and debug directly from the VSCode interface without extra manual setup.

# Notes

* The Docker environment contains all dependencies, so you don’t need to install anything manually.
* Using Clang inside Docker ensures consistent builds across Linux and macOS runners.
* GitHub Actions are recommended for cross-platform builds, including Windows .exe output, which is not trivial to produce locally without Visual Studio or clang-cl.
