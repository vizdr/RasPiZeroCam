# CMake Toolchain File
# Target: Raspberry Pi Zero 2 W (aarch64 / Cortex-A53)
# Host:   Ubuntu 22.04 x86_64
# Toolchain: Ubuntu aarch64-linux-gnu-g++ (apt: gcc-aarch64-linux-gnu)

# ── System identification ──────────────────────────────
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ── Sysroot ────────────────────────────────────────────
set(SYSROOT "$ENV{HOME}/rpi-sysroot")
set(CMAKE_SYSROOT ${SYSROOT})

# ── Cross-compiler paths ───────────────────────────────
# Ubuntu's aarch64-linux-gnu toolchain is built against Debian/Ubuntu glibc —
# the same family as Raspberry Pi OS.  This means:
#   • It knows the Debian multiarch layout (aarch64-linux-gnu/) natively,
#     so no -B or -I workarounds are needed.
#   • Its include-fixed/pthread.h was patched from a compatible glibc version,
#     so PTHREAD_COND_INITIALIZER matches the sysroot's struct layout.
#   • libcamera's CXXABI_1.3.15 requirement is satisfied without the GCC 14
#     library-path workaround required by the ARM official toolchain.
set(CROSS_TRIPLE "aarch64-linux-gnu")
set(CROSS_PREFIX "/usr/bin/${CROSS_TRIPLE}-")

set(CMAKE_C_COMPILER   ${CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_PREFIX}g++)
set(CMAKE_AR           ${CROSS_PREFIX}ar)
set(CMAKE_RANLIB       ${CROSS_PREFIX}ranlib)
set(CMAKE_STRIP        ${CROSS_PREFIX}strip)
set(CMAKE_NM           ${CROSS_PREFIX}nm)
set(CMAKE_OBJCOPY      ${CROSS_PREFIX}objcopy)
set(CMAKE_OBJDUMP      ${CROSS_PREFIX}objdump)

# ── Compiler flags for Cortex-A53 ─────────────────────
set(CMAKE_C_FLAGS_INIT
    "-march=armv8-a -mtune=cortex-a53 --sysroot=${SYSROOT}")

set(CMAKE_CXX_FLAGS_INIT
    "-march=armv8-a -mtune=cortex-a53 --sysroot=${SYSROOT}")

# ── Linker flags ───────────────────────────────────────
# -rpath-link: satisfies transitive shared-library dependencies (libpisp,
#              libudev, etc. needed by libcamera) at link time without
#              embedding runtime paths in the binary.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "--sysroot=${SYSROOT} \
     -Wl,-rpath-link,${SYSROOT}/lib/aarch64-linux-gnu \
     -Wl,-rpath-link,${SYSROOT}/usr/lib/aarch64-linux-gnu")

# ── Search paths ───────────────────────────────────────
set(CMAKE_FIND_ROOT_PATH  ${SYSROOT})

# Programs (cmake, python...) → use HOST versions
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Libraries → only in sysroot (ARM64 versions)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# Headers → only in sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# CMake packages → only in sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ── pkg-config setup ───────────────────────────────────
set(ENV{PKG_CONFIG_DIR}         "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:\
${SYSROOT}/usr/lib/pkgconfig:\
${SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SYSROOT}")
