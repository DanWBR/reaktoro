# The flat C API that lets a non-C++ caller reach Reaktoro.
#
# DWSIM used to drive Reaktoro 1 through its Python package, which pinned the whole application to a
# Python between 3.7 and 3.9. This library is what replaces that: one shared object per platform,
# loaded by P/Invoke, no interpreter involved.

add_library(ReaktoroC reaktoro_c.cpp reaktoro_c.h)

add_library(Reaktoro::ReaktoroC ALIAS ReaktoroC)

target_include_directories(ReaktoroC
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>)

target_link_libraries(ReaktoroC PRIVATE Reaktoro::Reaktoro)

target_compile_definitions(ReaktoroC PRIVATE REAKTORO_C_VERSION="${PROJECT_VERSION}")

target_compile_features(ReaktoroC PRIVATE cxx_std_17)

# The exports are the ones the header marks, and nothing else: this is a small flat surface and it
# should look like one from the outside.
set_target_properties(ReaktoroC PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)

install(TARGETS ReaktoroC
    EXPORT ReaktoroTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES reaktoro_c.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
