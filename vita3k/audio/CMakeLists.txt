set(AUDIO_SOURCES
    src/audio.cpp
    src/impl/null_audio.cpp)
if(SWITCH)
    list(APPEND AUDIO_SOURCES src/impl/switch_audio.cpp)
else()
    list(APPEND AUDIO_SOURCES
        src/impl/sdl_audio.cpp
        src/impl/cubeb_audio.cpp)
endif()

add_library(audio STATIC ${AUDIO_SOURCES})

target_include_directories(audio PUBLIC include)
target_link_libraries(audio PRIVATE util SDL3::SDL3)
if(NOT SWITCH)
    target_link_libraries(audio PRIVATE cubeb)
endif()
