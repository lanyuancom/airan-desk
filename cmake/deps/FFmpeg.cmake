set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT_DIR}/include" CACHE PATH "FFmpeg include directory")
set(FFMPEG_LIBRARY_DIR "${FFMPEG_ROOT_DIR}/lib" CACHE PATH "FFmpeg library directory")
set(FFMPEG_BINARY_DIR "${FFMPEG_ROOT_DIR}/bin" CACHE PATH "FFmpeg binary directory")

set(FFMPEG_LIBRARIES
    avcodec
    avdevice
    avfilter
    avformat
    avutil
    swresample
    swscale
)

set(FFMPEG_LINK_LIBRARIES)
foreach(_ffmpeg_lib ${FFMPEG_LIBRARIES})
    if(WIN32)
        set(_ffmpeg_lib_path "${FFMPEG_LIBRARY_DIR}/${_ffmpeg_lib}${CMAKE_IMPORT_LIBRARY_SUFFIX}")
    else()
        find_library(_ffmpeg_lib_path_${_ffmpeg_lib}
            NAMES ${_ffmpeg_lib}
            PATHS "${FFMPEG_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )
        if(_ffmpeg_lib_path_${_ffmpeg_lib})
            set(_ffmpeg_lib_path "${_ffmpeg_lib_path_${_ffmpeg_lib}}")
        else()
            find_library(_ffmpeg_lib_path_${_ffmpeg_lib}_default NAMES ${_ffmpeg_lib})
            if(_ffmpeg_lib_path_${_ffmpeg_lib}_default)
                set(_ffmpeg_lib_path "${_ffmpeg_lib_path_${_ffmpeg_lib}_default}")
            else()
                set(_ffmpeg_lib_path "${_ffmpeg_lib}")
            endif()
        endif()
    endif()

    if(EXISTS "${_ffmpeg_lib_path}")
        list(APPEND FFMPEG_LINK_LIBRARIES "${_ffmpeg_lib_path}")
    else()
        list(APPEND FFMPEG_LINK_LIBRARIES ${_ffmpeg_lib})
    endif()
endforeach()
