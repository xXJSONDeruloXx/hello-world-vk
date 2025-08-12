cmake_minimum_required(VERSION 3.13)
project(test_vk_hello_layer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

add_library(VK_LAYER_LUNARG_test_vk SHARED layers/hello_layer.cpp)

target_include_directories(VK_LAYER_LUNARG_test_vk PRIVATE ${Vulkan_INCLUDE_DIRS})

target_link_libraries(VK_LAYER_LUNARG_test_vk PRIVATE ${Vulkan_LIBRARIES})

set_target_properties(VK_LAYER_LUNARG_test_vk PROPERTIES
    OUTPUT_NAME "VK_LAYER_LUNARG_test_vk"
    PREFIX ""
)

# Install layout roughly matching standard Vulkan layer locations (~/.local/share/vulkan)
include(GNUInstallDirs)
install(TARGETS VK_LAYER_LUNARG_test_vk LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES layers/VK_LAYER_LUNARG_test_vk.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d)
