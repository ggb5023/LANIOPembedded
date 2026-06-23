add_executable(lan_chat_skeleton_tests
    skeleton_tests.cpp
)
target_link_libraries(lan_chat_skeleton_tests
    PRIVATE
        lan_chat_core_c
        lan_chat_storage_memory_c
        lan_chat_server_c
        lan_chat_transport_c
)

add_test(NAME lan_chat_skeleton_tests COMMAND lan_chat_skeleton_tests)

add_executable(lan_chat_mysql_storage_tests
    mysql_storage_tests.cpp
)
target_link_libraries(lan_chat_mysql_storage_tests
    PRIVATE
        lan_chat_core_c
        lan_chat_storage_mysql_c
)
if (LAN_CHAT_MYSQL_RUNTIME_DLL)
    add_custom_command(TARGET lan_chat_mysql_storage_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LAN_CHAT_MYSQL_RUNTIME_DLL}"
            $<TARGET_FILE_DIR:lan_chat_mysql_storage_tests>
    )
endif()

if (LAN_CHAT_ENABLE_MYSQL_TESTS)
    add_test(NAME lan_chat_mysql_storage_tests COMMAND lan_chat_mysql_storage_tests)
endif()
