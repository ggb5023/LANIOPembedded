#ifndef LAN_CHAT_STORAGE_MYSQL_STORAGE_H
#define LAN_CHAT_STORAGE_MYSQL_STORAGE_H

#include "lan_chat/storage/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

lan_chat_status_t lan_chat_storage_mysql_open(
    const lan_chat_storage_config_t *config,
    lan_chat_storage_t **out_storage);

#ifdef __cplusplus
}
#endif

#endif
