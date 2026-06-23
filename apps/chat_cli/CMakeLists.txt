add_executable(chat_cli
    main.cpp
)
target_link_libraries(chat_cli
    PRIVATE
        lan_chat_cpp
)
