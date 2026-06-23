add_executable(chat_server
    main.cpp
)
target_link_libraries(chat_server
    PRIVATE
        lan_chat_server_c
        lan_chat_storage_memory_c
        lan_chat_storage_mysql_c
)
if (LAN_CHAT_MYSQL_RUNTIME_DLL)
    add_custom_command(TARGET chat_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LAN_CHAT_MYSQL_RUNTIME_DLL}"
            $<TARGET_FILE_DIR:chat_server>
    )
endif()
