#include "cli_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace lan_chat_cli;

void print_usage()
{
    std::cout
        << "Usage:\n"
        << "  chat_cli register --server host:port --username name --password pass [--nickname name]\n"
        << "  chat_cli login --server host:port --username name --password pass\n"
        << "  chat_cli send --server host:port --username name --password pass --to-user-id id --message text\n"
        << "  chat_cli listen --server host:port --username name --password pass --expect-count n --timeout-ms ms [--output-dir dir]\n"
        << "  chat_cli group-create --server host:port --username name --password pass --name group --member-user-ids 2,3\n"
        << "  chat_cli group-send --server host:port --username name --password pass --conversation-id id --message text\n"
        << "  chat_cli send-file --server host:port --username name --password pass --to-user-id id --path file\n"
        << "  chat_cli e2e [--server host:port] [--message text]\n";
}

int command_register(int argc, char **argv)
{
    CommonOptions options;
    std::optional<std::string> nickname;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--nickname") {
            const char *value = require_value(argc, argv, &i, "--nickname");
            if (value == nullptr) {
                return 1;
            }
            nickname = std::string(value);
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    if (options.username.empty() || options.password.empty()) {
        return 1;
    }

    Client client;
    std::vector<std::uint8_t> packet;
    AuthResult result;
    if (!client.connect(options.server) ||
        !queue_auth_request(&client.connection, "register", options.username, options.password, nickname ? &*nickname : nullptr, 1) ||
        !wait_for_packet(&client.connection, &packet, 5000, "register response") ||
        !parse_auth_response(packet, 1, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "register failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "REGISTER_OK user_id=" << result.user_id << " session_id=" << result.session_id << '\n';
    return 0;
}

int command_login(int argc, char **argv)
{
    CommonOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    Client client;
    AuthResult result;
    if (!login_connected(&client, options, &result)) {
        return 1;
    }
    std::cout << "LOGIN_OK user_id=" << result.user_id << " session_id=" << result.session_id << '\n';
    return 0;
}

int command_send(int argc, char **argv)
{
    CommonOptions options;
    std::uint64_t receiver_id = 0;
    std::string message;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--to-user-id") {
            const char *value = require_value(argc, argv, &i, "--to-user-id");
            if (value == nullptr || !parse_u64(value, &receiver_id)) {
                return 1;
            }
        } else if (arg == "--message") {
            const char *value = require_value(argc, argv, &i, "--message");
            if (value == nullptr) {
                return 1;
            }
            message = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    if (message.empty() || receiver_id == 0) {
        return 1;
    }

    Client client;
    AuthResult auth;
    std::vector<std::uint8_t> packet;
    StatusResult result;
    if (!login_connected(&client, options, &auth) ||
        !queue_private_send(&client.connection, auth, receiver_id, message, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "private send response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_CHAT, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "send failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "PRIVATE_SEND_OK message_id=" << result.id_a << " delivery_id=" << result.id_b << '\n';
    return 0;
}

int command_listen(int argc, char **argv)
{
    CommonOptions options;
    int expect_count = 1;
    int timeout_ms = 10000;
    std::filesystem::path output_dir;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--expect-count") {
            const char *value = require_value(argc, argv, &i, "--expect-count");
            if (value == nullptr) {
                return 1;
            }
            expect_count = std::max(0, std::atoi(value));
        } else if (arg == "--timeout-ms") {
            const char *value = require_value(argc, argv, &i, "--timeout-ms");
            if (value == nullptr) {
                return 1;
            }
            timeout_ms = std::max(1, std::atoi(value));
        } else if (arg == "--output-dir") {
            const char *value = require_value(argc, argv, &i, "--output-dir");
            if (value == nullptr) {
                return 1;
            }
            output_dir = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    Client client;
    AuthResult auth;
    if (!login_connected(&client, options, &auth)) {
        return 1;
    }
    std::cout << "LOGIN_OK user_id=" << auth.user_id << " session_id=" << auth.session_id << '\n';

    int received = 0;
    ReceiveFileState file_state;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (received < expect_count && std::chrono::steady_clock::now() < deadline) {
        std::vector<std::uint8_t> packet;
        const int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (!wait_for_packet(&client.connection, &packet, std::max(1, remaining_ms), "listen packet")) {
            discard_partial_file(&file_state);
            return 1;
        }
        lan_chat_header_t header{};
        const std::uint8_t *body = nullptr;
        size_t body_len = 0;
        if (lan_chat_packet_unpack(&header, packet.data(), packet.size(), &body, &body_len) != LAN_CHAT_STATUS_OK) {
            return 1;
        }
        if (header.message_group == LAN_CHAT_GROUP_CHAT) {
            PrivateNotify notify;
            if (!parse_private_notify(packet, &notify)) {
                discard_partial_file(&file_state);
                return 1;
            }
            std::cout << "PRIVATE_TEXT sender_id=" << notify.sender_id
                      << " message_id=" << notify.message_id
                      << " delivery_id=" << notify.delivery_id
                      << " content=\"" << notify.content << "\"\n";
            ++received;
        } else if (header.message_group == LAN_CHAT_GROUP_CONVERSATION) {
            GroupNotify notify;
            if (!parse_group_notify(packet, &notify)) {
                discard_partial_file(&file_state);
                return 1;
            }
            std::cout << "GROUP_TEXT conversation_id=" << notify.conversation_id
                      << " sender_id=" << notify.sender_id
                      << " message_id=" << notify.message_id
                      << " content=\"" << notify.content << "\"\n";
            ++received;
        } else if (header.message_group == LAN_CHAT_GROUP_FILE) {
            FileNotify notify;
            if (!parse_file_notify(packet, &notify) || !handle_file_notify(notify, output_dir, &file_state)) {
                discard_partial_file(&file_state);
                return 1;
            }
            if (notify.operation == "complete") {
                ++received;
            }
        }
    }
    if (received != expect_count) {
        std::cerr << "listen expected " << expect_count << " event(s), received " << received << '\n';
        discard_partial_file(&file_state);
        return 1;
    }
    return 0;
}

int command_group_create(int argc, char **argv)
{
    CommonOptions options;
    std::string name;
    std::vector<std::uint64_t> members;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--name") {
            const char *value = require_value(argc, argv, &i, "--name");
            if (value == nullptr) {
                return 1;
            }
            name = value;
        } else if (arg == "--member-user-ids") {
            const char *value = require_value(argc, argv, &i, "--member-user-ids");
            if (value == nullptr || !parse_member_ids(value, &members)) {
                return 1;
            }
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    Client client;
    AuthResult auth;
    StatusResult result;
    std::vector<std::uint8_t> packet;
    if (!login_connected(&client, options, &auth) ||
        !queue_group_create(&client.connection, auth, name, members, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "group create response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_CONVERSATION, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "group create failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "GROUP_CREATE_OK conversation_id=" << result.id_a << '\n';
    return 0;
}

int command_group_send(int argc, char **argv)
{
    CommonOptions options;
    std::uint64_t conversation_id = 0;
    std::string message;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--conversation-id") {
            const char *value = require_value(argc, argv, &i, "--conversation-id");
            if (value == nullptr || !parse_u64(value, &conversation_id)) {
                return 1;
            }
        } else if (arg == "--message") {
            const char *value = require_value(argc, argv, &i, "--message");
            if (value == nullptr) {
                return 1;
            }
            message = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    Client client;
    AuthResult auth;
    StatusResult result;
    std::vector<std::uint8_t> packet;
    if (!login_connected(&client, options, &auth) ||
        !queue_group_send(&client.connection, auth, conversation_id, message, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "group send response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_CONVERSATION, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "group send failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    std::cout << "GROUP_SEND_OK conversation_id=" << result.id_a << " message_id=" << result.id_b << '\n';
    return 0;
}

int command_send_file(int argc, char **argv)
{
    CommonOptions options;
    std::uint64_t receiver_id = 0;
    std::filesystem::path path;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &options.server)) {
                return 1;
            }
        } else if (arg == "--username") {
            const char *value = require_value(argc, argv, &i, "--username");
            if (value == nullptr) {
                return 1;
            }
            options.username = value;
        } else if (arg == "--password") {
            const char *value = require_value(argc, argv, &i, "--password");
            if (value == nullptr) {
                return 1;
            }
            options.password = value;
        } else if (arg == "--to-user-id") {
            const char *value = require_value(argc, argv, &i, "--to-user-id");
            if (value == nullptr || !parse_u64(value, &receiver_id)) {
                return 1;
            }
        } else if (arg == "--path") {
            const char *value = require_value(argc, argv, &i, "--path");
            if (value == nullptr) {
                return 1;
            }
            path = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }

    std::vector<std::uint8_t> bytes;
    if (receiver_id == 0 || path.empty() || !read_file(path, &bytes)) {
        return 1;
    }
    const std::string name = base_name(path);
    const std::uint32_t crc = crc32_bytes(bytes);
    Client client;
    AuthResult auth;
    StatusResult result;
    std::vector<std::uint8_t> packet;
    if (!login_connected(&client, options, &auth) ||
        !queue_file_operation(&client.connection, auth, receiver_id, "start", 0, 0, &name, bytes.size(), crc, nullptr, 0, 2) ||
        !wait_for_packet(&client.connection, &packet, 5000, "file start response") ||
        !parse_status_response(packet, 2, LAN_CHAT_GROUP_FILE, &result)) {
        return 1;
    }
    if (result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "file start failed: " << lan_chat_status_name(result.status) << '\n';
        return 1;
    }
    const std::uint64_t transfer_id = result.id_a;
    std::uint32_t seq = 3;
    for (size_t offset = 0, chunk_index = 0; offset < bytes.size(); offset += LAN_CHAT_FILE_CHUNK_SIZE, ++chunk_index, ++seq) {
        const size_t chunk_len = std::min<size_t>(LAN_CHAT_FILE_CHUNK_SIZE, bytes.size() - offset);
        StatusResult chunk_result;
        if (!queue_file_operation(&client.connection, auth, receiver_id, "chunk", transfer_id, chunk_index, nullptr, 0, 0, bytes.data() + offset, chunk_len, seq) ||
            !wait_for_packet(&client.connection, &packet, 5000, "file chunk response") ||
            !parse_status_response(packet, seq, LAN_CHAT_GROUP_FILE, &chunk_result) ||
            chunk_result.status != LAN_CHAT_STATUS_OK) {
            std::cerr << "file chunk failed\n";
            return 1;
        }
    }
    StatusResult complete_result;
    if (!queue_file_operation(&client.connection, auth, receiver_id, "complete", transfer_id, 0, nullptr, 0, 0, nullptr, 0, seq) ||
        !wait_for_packet(&client.connection, &packet, 5000, "file complete response") ||
        !parse_status_response(packet, seq, LAN_CHAT_GROUP_FILE, &complete_result) ||
        complete_result.status != LAN_CHAT_STATUS_OK) {
        std::cerr << "file complete failed\n";
        return 1;
    }
    std::cout << "FILE_SEND_OK transfer_id=" << transfer_id
              << " message_id=" << result.id_b
              << " delivery_id=" << result.id_c
              << " size=" << bytes.size()
              << " crc32=" << crc << '\n';
    return 0;
}

std::string make_unique_suffix()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool run_e2e(const Endpoint &server, const std::string &message)
{
    const std::string suffix = make_unique_suffix();
    const std::string alice_name = "alice_app_" + suffix;
    const std::string bob_name = "bob_app_" + suffix;
    const std::string alice_password = "alice-password-" + suffix;
    const std::string bob_password = "bob-password-" + suffix;

    {
        const char *alice_argv[] = {"chat_cli", "register", "--server", "", "--username", "", "--password", "", "--nickname", "Alice App"};
        (void)alice_argv;
    }

    Client alice;
    Client bob;
    std::vector<std::uint8_t> packet;
    AuthResult alice_register;
    AuthResult bob_register;
    AuthResult alice_login;
    AuthResult bob_login;
    StatusResult send_result;
    PrivateNotify notify;

    if (!alice.connect(server) || !bob.connect(server)) {
        return false;
    }
    if (!queue_auth_request(&alice.connection, "register", alice_name, alice_password, nullptr, 1) ||
        !wait_for_packet(&alice.connection, &packet, 5000, "alice register") ||
        !parse_auth_response(packet, 1, &alice_register) ||
        alice_register.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_auth_request(&bob.connection, "register", bob_name, bob_password, nullptr, 2) ||
        !wait_for_packet(&bob.connection, &packet, 5000, "bob register") ||
        !parse_auth_response(packet, 2, &bob_register) ||
        bob_register.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_auth_request(&alice.connection, "login", alice_name, alice_password, nullptr, 3) ||
        !wait_for_packet(&alice.connection, &packet, 5000, "alice login") ||
        !parse_auth_response(packet, 3, &alice_login) ||
        alice_login.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_auth_request(&bob.connection, "login", bob_name, bob_password, nullptr, 4) ||
        !wait_for_packet(&bob.connection, &packet, 5000, "bob login") ||
        !parse_auth_response(packet, 4, &bob_login) ||
        bob_login.status != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!queue_private_send(&alice.connection, alice_login, bob_login.user_id, message, 5) ||
        !wait_for_packet(&alice.connection, &packet, 5000, "private e2e send") ||
        !parse_status_response(packet, 5, LAN_CHAT_GROUP_CHAT, &send_result) ||
        send_result.status != LAN_CHAT_STATUS_OK ||
        !wait_for_packet(&bob.connection, &packet, 5000, "private e2e notify") ||
        !parse_private_notify(packet, &notify) ||
        notify.sender_id != alice_login.user_id ||
        notify.content != message) {
        return false;
    }
    std::cout << "alice user_id=" << alice_login.user_id << " session_id=" << alice_login.session_id << '\n';
    std::cout << "bob user_id=" << bob_login.user_id << " session_id=" << bob_login.session_id << '\n';
    std::cout << "chat message_id=" << send_result.id_a << " delivery_id=" << send_result.id_b << '\n';
    std::cout << "notify sender_id=" << notify.sender_id << " content=\"" << notify.content << "\"\n";
    std::cout << "APP_E2E_OK\n";
    return true;
}

int command_e2e(int argc, char **argv)
{
    Endpoint server;
    std::string message = "hello from chat_cli e2e";
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            const char *value = require_value(argc, argv, &i, "--server");
            if (value == nullptr || !parse_endpoint(value, &server)) {
                return 1;
            }
        } else if (arg == "--message") {
            const char *value = require_value(argc, argv, &i, "--message");
            if (value == nullptr) {
                return 1;
            }
            message = value;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return 1;
        }
    }
    return run_e2e(server, message) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    const std::string command = argv[1];
    if (command == "register") {
        return command_register(argc, argv);
    }
    if (command == "login") {
        return command_login(argc, argv);
    }
    if (command == "send") {
        return command_send(argc, argv);
    }
    if (command == "listen") {
        return command_listen(argc, argv);
    }
    if (command == "group-create") {
        return command_group_create(argc, argv);
    }
    if (command == "group-send") {
        return command_group_send(argc, argv);
    }
    if (command == "send-file") {
        return command_send_file(argc, argv);
    }
    if (command == "e2e") {
        return command_e2e(argc, argv);
    }

    std::cerr << "unknown command: " << command << '\n';
    print_usage();
    return 1;
}
