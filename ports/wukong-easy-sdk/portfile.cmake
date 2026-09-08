# The SDK exports a static library; dependency/CRT linkage follows the selected triplet.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO WuKongIM/WuKongEasySDK-CPP
    REF 3e367a908f42385ab9306f9708b7456399cace7d
    SHA512 afb445cdd5e7dd1ce6b21949b5bdbc0874a5160bcc4d48da48e3bb7864f2f057145e8fbd1302a94dc51d79990ee5d780518cf5291909aed130ed5c03750329aa
)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DWUKONG_BUILD_TESTS=OFF
        -DWUKONG_BUILD_EXAMPLES=OFF
        -DWUKONG_FETCH_JSON=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME WuKongEasySDK CONFIG_PATH lib/cmake/WuKongEasySDK)
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
