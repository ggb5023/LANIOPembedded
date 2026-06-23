CREATE DATABASE IF NOT EXISTS `lan_chat`
DEFAULT CHARACTER SET utf8mb4
DEFAULT COLLATE utf8mb4_unicode_ci;

USE `lan_chat`;

SET NAMES utf8mb4;

CREATE TABLE IF NOT EXISTS `accounts` (
    `user_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `username` VARCHAR(64) NOT NULL,
    `nickname` VARCHAR(64) NOT NULL,
    `password_hash` VARCHAR(128) NOT NULL,
    `password_salt` VARCHAR(64) NOT NULL,
    `enabled` TINYINT(1) NOT NULL DEFAULT 1,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `last_login_at` DATETIME NULL,
    PRIMARY KEY (`user_id`),
    UNIQUE KEY `uk_accounts_username` (`username`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `conversations` (
    `conversation_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `conversation_type` VARCHAR(16) NOT NULL DEFAULT 'private',
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`conversation_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `conversation_members` (
    `conversation_id` BIGINT UNSIGNED NOT NULL,
    `user_id` BIGINT UNSIGNED NOT NULL,
    `active` TINYINT(1) NOT NULL DEFAULT 1,
    `joined_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `last_seen_message_id` BIGINT UNSIGNED NULL,
    PRIMARY KEY (`conversation_id`, `user_id`),
    KEY `idx_conversation_members_user` (`user_id`, `active`),
    CONSTRAINT `fk_conversation_members_conversation`
        FOREIGN KEY (`conversation_id`) REFERENCES `conversations` (`conversation_id`)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    CONSTRAINT `fk_conversation_members_user`
        FOREIGN KEY (`user_id`) REFERENCES `accounts` (`user_id`)
        ON UPDATE CASCADE ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `messages` (
    `message_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `conversation_id` BIGINT UNSIGNED NOT NULL,
    `sender_id` BIGINT UNSIGNED NOT NULL,
    `receiver_id` BIGINT UNSIGNED NOT NULL,
    `content_type` SMALLINT UNSIGNED NOT NULL DEFAULT 1,
    `content` TEXT NOT NULL,
    `client_message_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `send_time` DATETIME NOT NULL,
    `store_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`message_id`),
    KEY `idx_messages_conversation_time` (`conversation_id`, `message_id`),
    KEY `idx_messages_sender_time` (`sender_id`, `message_id`),
    KEY `idx_messages_receiver_time` (`receiver_id`, `message_id`),
    CONSTRAINT `fk_messages_conversation`
        FOREIGN KEY (`conversation_id`) REFERENCES `conversations` (`conversation_id`)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    CONSTRAINT `fk_messages_sender`
        FOREIGN KEY (`sender_id`) REFERENCES `accounts` (`user_id`)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    CONSTRAINT `fk_messages_receiver`
        FOREIGN KEY (`receiver_id`) REFERENCES `accounts` (`user_id`)
        ON UPDATE CASCADE ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `message_deliveries` (
    `delivery_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `message_id` BIGINT UNSIGNED NOT NULL,
    `receiver_id` BIGINT UNSIGNED NOT NULL,
    `delivery_state` VARCHAR(16) NOT NULL DEFAULT 'pending',
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `delivered_at` DATETIME NULL,
    `acked_at` DATETIME NULL,
    PRIMARY KEY (`delivery_id`),
    UNIQUE KEY `uk_message_deliveries_message_receiver` (`message_id`, `receiver_id`),
    KEY `idx_message_deliveries_receiver_state` (`receiver_id`, `delivery_state`, `delivery_id`),
    CONSTRAINT `fk_message_deliveries_message`
        FOREIGN KEY (`message_id`) REFERENCES `messages` (`message_id`)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    CONSTRAINT `fk_message_deliveries_receiver`
        FOREIGN KEY (`receiver_id`) REFERENCES `accounts` (`user_id`)
        ON UPDATE CASCADE ON DELETE RESTRICT,
    CONSTRAINT `ck_message_deliveries_state`
        CHECK (`delivery_state` IN ('pending', 'delivered', 'acked'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
