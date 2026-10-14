"""PlatformIO pre-build hook: build zsign-rust with cargo and link it.

PlatformIO's ESP-IDF integration does not run CMake custom targets, so the
staticlib is built here and handed to the linker directly. Plain idf.py
builds use components/zsign-rust/CMakeLists.txt instead.
"""
import os
import subprocess

Import("env")  # noqa: F821  (provided by SCons)

TARGET = "xtensa-esp32s3-none-elf"
crate = os.path.join(env.subst("$PROJECT_DIR"), env.GetProjectOption("custom_zsign_dir"))  # noqa: F821
crate = os.path.normpath(crate)
target_dir = os.path.join(env.subst("$BUILD_DIR"), "zsign-rust-target")  # noqa: F821

print(f"zsign-rust: cargo build ({TARGET}) in {crate}")
subprocess.run(
    ["cargo", "+esp", "build", "--release", "--target", TARGET,
     "-Zbuild-std=core,alloc", "--target-dir", target_dir],
    cwd=crate, check=True)

libdir = os.path.join(target_dir, TARGET, "release")
env.Append(LIBPATH=[libdir], LIBS=["zsign_rust"])  # noqa: F821
