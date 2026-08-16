cmake_minimum_required(VERSION 3.15)

# Use the target's MSVC_RUNTIME_LIBRARY property (instead of the old
# CMAKE_MSVC_RUNTIME_LIBRARY_* flags baked into CMAKE_CXX_FLAGS_*).
cmake_policy(SET CMP0091 NEW)

project(ExtIO_Pluto LANGUAGES CXX)

if (NOT MSVC)
    message(FATAL_ERROR
        "ExtIO_Pluto only builds with the MSVC toolchain: the code relies on "
        "MSVC/WinAPI-specific things (IMAGE_DOS_HEADER __ImageBase trick, "
        "__declspec(dllexport), _snprintf/_atoi64, a .def export file, "
        "resource compiler, etc). Select an MSVC kit in CMake Tools.")
endif()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "This must be built as a 32-bit (Win32/x86) DLL: the bundled "
        "libiio.lib and its runtime DLLs (libiio.dll, libusb-1.0.dll, "
        "libserialport-0.dll, libxml2.dll) are all 32-bit, and HDSDR "
        "itself is a 32-bit host application. In CMake Tools, pick an "
        "x86 kit (NOT amd64/x64).")
endif()

# Match the original project: /MDd for Debug, /MD for Release
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_library(ExtIO_Pluto SHARED
    ExtIO_Pluto.cpp
    ExtIO_PlutoSDR.rc
)

target_include_directories(ExtIO_Pluto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(ExtIO_Pluto PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    EXTIO_EXPORTS
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

target_link_libraries(ExtIO_Pluto PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libiio.lib"
)

# .def file controls the exported (undecorated) symbol names that HDSDR
# looks up by name (InitHW, OpenHW, StartHW, ...). /MACHINE:X86 and
# /SUBSYSTEM:WINDOWS mirror the original .vcxproj settings.
set_target_properties(ExtIO_Pluto PROPERTIES
    OUTPUT_NAME "ExtIO_Pluto"
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/ExtIO_Pluto.def\" /MACHINE:X86 /SUBSYSTEM:WINDOWS"
)

# Convenience: after building, remind where the DLL ended up and what
# runtime DLLs need to sit next to it inside the HDSDR folder.
add_custom_command(TARGET ExtIO_Pluto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Built: $<TARGET_FILE:ExtIO_Pluto>"
    COMMAND ${CMAKE_COMMAND} -E echo "Copy it into the HDSDR folder together with libiio.dll, libusb-1.0.dll, libserialport-0.dll, libxml2.dll (see ../Debug or ../Release for known-good copies)."
)
