# vcpkg's x64-windows-static-md (vcpkg.cmake's: static libraries, the dynamic C runtime) with release builds
# only, for CI and release builds, which never link debug protobuf: half the time to build protobuf and
# abseil. A developer's Debug build needs the debug ones, so vcpkg.cmake keeps the usual triplet.
#   cmake ... -DVCPKG_OVERLAY_TRIPLETS=host/src/netplay/net/triplets -DVCPKG_TARGET_TRIPLET=x64-windows-static-md-release
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
