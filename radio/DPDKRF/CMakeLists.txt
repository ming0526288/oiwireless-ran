find_library(dpdkrf NAMES dpdkrf PATHS /usr/local/lib)

add_library(oai_dpdkrfdevif MODULE dpdkrf_oai.c)
target_link_libraries(oai_dpdkrfdevif PRIVATE dpdkrf)
target_link_libraries(oai_dpdkrfdevif PRIVATE UTIL)
set_target_properties(oai_dpdkrfdevif PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_custom_command(TARGET oai_dpdkrfdevif POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink liboai_dpdkrfdevif.so liboai_device.so
                   WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

message(STATUS "CMAKE_LIBRARY_PATH: ${CMAKE_BINARY_DIR}")
message(STATUS "Libraries linked to your_target_name: ${oai_dpdkrfdevif}")


