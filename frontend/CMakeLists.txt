# What every front-end does the same way: where a person's data goes (user_dirs.h), so a model's
# measurements kept by one are read by the others, and what the compute backends are told before they
# load (backend_env.h), so every front-end starts them alike, and what the Vulkan stack says about
# itself (vulkan_report.h). bmoe-cli, bmoe-server and the apps link it.
add_library(bmoe_frontend STATIC user_dirs.cpp backend_env.cpp vulkan_report.cpp)
# vulkan_report opens the Vulkan loader at run time (dlopen): no link against it, only its headers.
if(NOT WIN32)
    target_link_libraries(bmoe_frontend PUBLIC ${CMAKE_DL_LIBS})
endif()
if(DEFINED Vulkan_INCLUDE_DIR)
    target_include_directories(bmoe_frontend PRIVATE ${Vulkan_INCLUDE_DIR})
endif()
target_include_directories(bmoe_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bmoe_frontend PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(bmoe_frontend PUBLIC shell32 ole32)
endif()
