target_sources(moonlight-lib PRIVATE
        keyboard_webos.c
        navkey_sdl_webos.c
        pbnjson_compat.c
        game_mode.c
        stream_priority.c
        bt_sniff.c
        steam_ble.c
        dualsense_usb.c
        keyboard_evdev.c
        sdl_webos_sdl3.c)

# Occupies SONAME libSDL2-2.0.so.0 so NDL does not bind the TV's SDL 2.0.14.
# See sdl2_ndl_shim.c.
add_library(sdl2_ndl_shim SHARED sdl2_ndl_shim.c)
target_link_libraries(sdl2_ndl_shim PRIVATE dl)
set_target_properties(sdl2_ndl_shim PROPERTIES
        OUTPUT_NAME "SDL2-2.0"
        SUFFIX ".so.0"
        NO_SONAME TRUE
        BUILD_RPATH "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN")
target_link_options(sdl2_ndl_shim PRIVATE "-Wl,-soname,libSDL2-2.0.so.0")
install(TARGETS sdl2_ndl_shim LIBRARY DESTINATION lib/backports)
