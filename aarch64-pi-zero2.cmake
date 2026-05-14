# CMake Toolchain File
# Target: Raspberry Pi Zero 2 W (aarch64 / Cortex-A53)
# Host:   Ubuntu 22.04 x86_64

# ── System identification ──────────────────────────────
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ── Sysroot ────────────────────────────────────────────
set(SYSROOT "$ENV{HOME}/rpi-sysroot")
set(CMAKE_SYSROOT ${SYSROOT})

# ── Cross-compiler paths ───────────────────────────────
set(CROSS_TRIPLE "aarch64-none-linux-gnu")
set(CROSS_PREFIX "/opt/arm-gnu-toolchain-13.3/bin/${CROSS_TRIPLE}-")

set(CMAKE_C_COMPILER   ${CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_PREFIX}g++)
set(CMAKE_AR           ${CROSS_PREFIX}ar)
set(CMAKE_RANLIB       ${CROSS_PREFIX}ranlib)
set(CMAKE_STRIP        ${CROSS_PREFIX}strip)
set(CMAKE_NM           ${CROSS_PREFIX}nm)
set(CMAKE_OBJCOPY      ${CROSS_PREFIX}objcopy)
set(CMAKE_OBJDUMP      ${CROSS_PREFIX}objdump)

# ── Compiler flags for Cortex-A53 ─────────────────────
# -B: startup files (crt1.o etc.); -I: multiarch system headers
set(CMAKE_C_FLAGS_INIT
    "-march=armv8-a -mtune=cortex-a53 --sysroot=${SYSROOT} -B${SYSROOT}/usr/lib/aarch64-linux-gnu -I${SYSROOT}/usr/include/aarch64-linux-gnu")

set(CMAKE_CXX_FLAGS_INIT
    "-march=armv8-a -mtune=cortex-a53 --sysroot=${SYSROOT} -B${SYSROOT}/usr/lib/aarch64-linux-gnu -I${SYSROOT}/usr/include/aarch64-linux-gnu")

# ── Linker flags ───────────────────────────────────────
# GCC 14 path comes first so the sysroot's libstdc++ (CXXABI_1.3.15) wins
# over the cross-compiler's GCC 13 libstdc++ (max CXXABI_1.3.14).
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "--sysroot=${SYSROOT} \
     -L${SYSROOT}/usr/lib/gcc/aarch64-linux-gnu/14 \
     -L${SYSROOT}/lib/gcc/aarch64-linux-gnu/14 \
     -L${SYSROOT}/usr/lib/aarch64-linux-gnu \
     -L${SYSROOT}/lib/aarch64-linux-gnu \
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
