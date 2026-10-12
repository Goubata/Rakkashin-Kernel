# Rakkashin Kernel build environment. Source it from anywhere:
#   . rakkashin/build-env.sh [out-dir]
# Native arm64 host (clang/lld 19, rustc 1.82 + bindgen); on x86_64 the same
# variables work with an aarch64 cross toolchain (LLVM=1 handles that).
export ARCH=arm64 LLVM=1 LLVM_IAS=1
export KSRC=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)
export KOUT=${1:-$KSRC/../out}

# Kleaf builds with gendwarfksyms --stable; without it symbol CRCs differ
# from the stock GKI and every vendor module is rejected at boot.
export KBUILD_GENDWARFKSYMS_STABLE=1

# Match the stock GKI identity (see avr_custom.fragment, "identity").
export KBUILD_BUILD_USER=kleaf KBUILD_BUILD_HOST=build-host
export KBUILD_BUILD_TIMESTAMP="Mon Dec 22 22:41:20 UTC 2025" LOCALVERSION=

alias kmake='make -C $KSRC O=$KOUT'
