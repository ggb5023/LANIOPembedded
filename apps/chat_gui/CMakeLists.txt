set(CMAKE_AUTOMOC ON)

add_executable(chat_gui
    main.cpp
)

target_link_libraries(chat_gui
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        lan_chat_core_c
        lan_chat_transport_c
)

if (LAN_CHAT_BUILD_AV)
    target_compile_definitions(chat_gui PRIVATE LAN_CHAT_GUI_ENABLE_AV=1)
    target_link_libraries(chat_gui PRIVATE Qt6::Multimedia Opus::opus LibDataChannel::LibDataChannel)
else()
    target_compile_definitions(chat_gui PRIVATE LAN_CHAT_GUI_ENABLE_AV=0)
endif()
