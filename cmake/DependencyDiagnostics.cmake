# SPDX-License-Identifier: GPL-2.0-or-later
# Reviewed ABI extensions, restricted to the existing vendor targets/files.
# Engine diagnostics, deprecated declarations and all unrelated warnings remain on.
if(NOT (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    return()
endif()

# Protobuf serializes by compile-time offsets of polymorphic message members.
# Its own PROTOBUF_FIELD_OFFSET uses the same supported builtin with a diagnostic
# scope; generated fast tables also use offsetof directly. Changing the layout
# to satisfy standard-layout rules would break the serialization ABI.
foreach(target libprotobuf libprotobuf-lite libprotoc)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE -Wno-invalid-offsetof)
    endif()
endforeach()

# OpenAL's flexible-tail allocator intentionally uses the same Clang extension.
# Keep its allocation sizes and audio object layout unchanged.
target_compile_options(OpenAL PRIVATE -Wno-invalid-offsetof)

# Legacy public BIO callback control transports an SSL info callback through the
# older BIO_info_cb prototype. Preserve that vendor API contract; do not rewrite
# its callback types or disable type diagnostics in the rest of the library.
set_property(SOURCE "${IFSS_EXTERNALS_ROOT}/libressl/ssl/bio_ssl.c"
    TARGET_DIRECTORY ssl_obj APPEND PROPERTY COMPILE_OPTIONS
    -Wno-cast-function-type-mismatch)
