set(CMAKE_AUTOMOC ON)

add_library(chat_gui_core STATIC
    audio_call_session.cpp
    audio_call_session.hpp
    gui_client.cpp
    gui_client.hpp
)

target_link_libraries(chat_gui_core
    PUBLIC
        Qt6::Core
        lan_chat_core_c
        lan_chat_transport_c
)
target_include_directories(chat_gui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (LAN_CHAT_BUILD_AV)
    target_compile_definitions(chat_gui_core PUBLIC LAN_CHAT_GUI_ENABLE_AV=1)
    target_link_libraries(chat_gui_core PUBLIC Qt6::Multimedia Opus::opus LibDataChannel::LibDataChannelStatic)
else()
    target_compile_definitions(chat_gui_core PUBLIC LAN_CHAT_GUI_ENABLE_AV=0)
endif()

add_executable(chat_gui
    main.cpp
)

target_link_libraries(chat_gui
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        chat_gui_core
)
