add_library(sf_audio INTERFACE)

target_include_directories(sf_audio
INTERFACE
    ${PROJECT_SOURCE_DIR}/host
    ${PROJECT_SOURCE_DIR}/shared
)

target_link_libraries(sf_audio INTERFACE PkgConfig::PIPEWIRE)