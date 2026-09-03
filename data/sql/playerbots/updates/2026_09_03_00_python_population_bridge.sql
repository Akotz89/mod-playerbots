CREATE TABLE IF NOT EXISTS `playerbots_population_account` (
  `account_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `account_name` varchar(17) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `expected_slots` tinyint unsigned NOT NULL,
  `account_id` int unsigned DEFAULT NULL,
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`account_key`),
  UNIQUE KEY `uq_population_account_name` (`account_name`),
  UNIQUE KEY `uq_population_account_id` (`account_id`),
  CONSTRAINT `chk_population_expected_slots` CHECK (`expected_slots` BETWEEN 1 AND 9)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `playerbots_population_identity` (
  `identity_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `account_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `guid_allocation_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `wave_key` varchar(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `manifest_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin DEFAULT NULL,
  `account_id` int unsigned DEFAULT NULL,
  `character_guid` int unsigned DEFAULT NULL,
  `admitted` tinyint unsigned NOT NULL DEFAULT 0,
  `session_requested` tinyint unsigned NOT NULL DEFAULT 0,
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`identity_key`),
  UNIQUE KEY `uq_population_guid_allocation` (`guid_allocation_key`),
  UNIQUE KEY `uq_population_character_guid` (`character_guid`),
  KEY `idx_population_account_key` (`account_key`),
  KEY `idx_population_admission_session` (`admitted`, `session_requested`),
  CONSTRAINT `chk_population_admitted` CHECK (`admitted` IN (0, 1)),
  CONSTRAINT `chk_population_session` CHECK (`session_requested` IN (0, 1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
