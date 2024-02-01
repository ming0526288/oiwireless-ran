find_library(oxgrf_ss NAMES oxgrf_ss)

add_library(oai_oxgrfdevif MODULE oxgrf_lib.c)
target_link_libraries(oai_oxgrfdevif PRIVATE oxgrf_ss)
target_link_libraries(oai_oxgrfdevif PRIVATE UTIL)
set_target_properties(oai_oxgrfdevif PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_custom_command(TARGET oai_oxgrfdevif POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink liboai_oxgrfdevif.so liboai_device.so
                   WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
