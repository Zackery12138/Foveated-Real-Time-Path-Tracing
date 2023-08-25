cmake_minimum_required(VERSION 3.9.6 FATAL_ERROR)


get_filename_component(PROJNAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
set(PROJNAME ${PROJNAME})
project(${PROJNAME} LANGUAGES C CXX)


set(CMAKE_CXX_STANDARD 17)
add_executable(${PROJNAME})

if(MSVC)
    add_definitions(/wd26812)  
    add_definitions(/wd26451)  
endif()



if(NOT BASE_DIRECTORY)
  find_path(BASE_DIRECTORY
    NAMES nvpro_core/cmake/setup.cmake
    PATHS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../.. 
    REQUIRED
    DOC "Directory containing nvpro_core"
    )
endif()

if(EXISTS ${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake)
  include(${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake)
  include(${BASE_DIRECTORY}/nvpro_core/cmake/utilities.cmake)
else()
  message(FATAL_ERROR "could not find base directory, please set BASE_DIRECTORY to folder containing nvpro_core")
endif() 


_add_package_VulkanSDK()
_add_package_ImGUI()
_add_package_FreeImage()
_add_package_NVML()
_add_nvpro_core_lib()


#target_compile_definitions(${PROJNAME} PUBLIC ALLOC_DMA)


_add_project_definitions(${PROJNAME})


if(MSVC)
  set_target_properties(${PROJNAME} PROPERTIES LINK_FLAGS "/DELAYLOAD:nvml.dll")
endif()


file(GLOB SOURCE_FILES src/*.cpp src/*.c)
file(GLOB HEADER_FILES src/*.hpp src/*.h )



compile_glsl_directory(
    SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders" 
    DST "${CMAKE_CURRENT_SOURCE_DIR}/autogen" 
    VULKAN_TARGET "vulkan1.2"
    HEADER ON
    DEPENDENCY ${VULKAN_BUILD_DEPENDENCIES}
    )



#--------------------------------------------------------------------------------------------------
# Sources
target_sources(${PROJNAME} PUBLIC ${SOURCE_FILES} ${HEADER_FILES})
target_sources(${PROJNAME} PUBLIC ${COMMON_SOURCE_FILES})
target_sources(${PROJNAME} PUBLIC ${PACKAGE_SOURCE_FILES})
target_sources(${PROJNAME} PUBLIC ${GLSL_SOURCES})
target_sources(${PROJNAME} PUBLIC ${GLSL_HEADERS})


source_group("Common"         FILES ${COMMON_SOURCE_FILES} ${PACKAGE_SOURCE_FILES})
source_group("Sources"        FILES ${SOURCE_FILES})
source_group("Header Files"   FILES ${HEADER_FILES})
source_group("Shader Sources" FILES ${GLSL_SOURCES})
source_group("Shader Headers" FILES ${GLSL_HEADERS})


target_link_libraries(${PROJNAME} ${PLATFORM_LIBRARIES} nvpro_core)

foreach(DEBUGLIB ${LIBRARIES_DEBUG})
  target_link_libraries(${PROJNAME} debug ${DEBUGLIB})
endforeach(DEBUGLIB)

foreach(RELEASELIB ${LIBRARIES_OPTIMIZED})
  target_link_libraries(${PROJNAME} optimized ${RELEASELIB})
endforeach(RELEASELIB)


_finalize_target( ${PROJNAME} )




