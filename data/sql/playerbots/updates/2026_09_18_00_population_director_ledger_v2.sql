-- CPI-194 owner-independent population-director ledger v2.
-- Deliberately omit IF NOT EXISTS: a partial or owner-coupled install must fail.

CREATE TABLE `population_director_schedule` (
  `realm_id` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `schedule_version` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `schedule_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `policy_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `manifest_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `snapshot_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `installation_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `maximum_owner_level` tinyint unsigned NOT NULL,
  `global_phase_cap` tinyint unsigned NOT NULL,
  `permanent_roster_target` int unsigned NOT NULL,
  `installed_at` timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (`realm_id`),
  UNIQUE KEY `uq_population_director_schedule_digest` (`schedule_digest`),
  CONSTRAINT `chk_population_director_schedule_level`
    CHECK (`maximum_owner_level` BETWEEN 1 AND 80),
  CONSTRAINT `chk_population_director_schedule_phase`
    CHECK (`global_phase_cap` BETWEEN 1 AND `maximum_owner_level`),
  CONSTRAINT `chk_population_director_schedule_target`
    CHECK (`permanent_roster_target` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Immutable owner-independent population schedule and artifact digests';

CREATE TABLE `population_director_owner_state` (
  `realm_id` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `binding_state` enum('unbound','bound') NOT NULL,
  `owner_account_id` int unsigned NULL,
  `owner_character_guid` int unsigned NULL,
  `highest_owner_level_seen` tinyint unsigned NOT NULL,
  `current_epoch` tinyint unsigned NOT NULL,
  `generation` bigint unsigned NOT NULL,
  `last_reconciled_at` timestamp(6) NULL DEFAULT NULL,
  PRIMARY KEY (`realm_id`),
  UNIQUE KEY `uq_population_director_owner_guid` (`realm_id`, `owner_character_guid`),
  CONSTRAINT `fk_population_director_owner_schedule`
    FOREIGN KEY (`realm_id`) REFERENCES `population_director_schedule` (`realm_id`),
  CONSTRAINT `chk_population_director_owner_binding_pair`
    CHECK (
      (`binding_state` = 'unbound' AND `owner_account_id` IS NULL AND `owner_character_guid` IS NULL)
      OR
      (`binding_state` = 'bound' AND `owner_account_id` IS NOT NULL AND `owner_character_guid` IS NOT NULL)
    ),
  CONSTRAINT `chk_population_director_owner_highwater`
    CHECK (`highest_owner_level_seen` BETWEEN 1 AND 80),
  CONSTRAINT `chk_population_director_owner_epoch`
    CHECK (`current_epoch` = `highest_owner_level_seen`),
  CONSTRAINT `chk_population_director_owner_generation` CHECK (`generation` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Mutable owner binding and monotonic high-water; initially unbound';

CREATE TABLE `population_director_owner_binding_audit` (
  `audit_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `realm_id` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `event_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `operation` enum('bind','rebind') NOT NULL,
  `expected_generation` bigint unsigned NOT NULL,
  `next_generation` bigint unsigned NOT NULL,
  `previous_account_id` int unsigned NULL,
  `previous_character_guid` int unsigned NULL,
  `selected_account_id` int unsigned NOT NULL,
  `selected_character_guid` int unsigned NOT NULL,
  `actor` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `selected_at_utc` datetime(6) NOT NULL,
  `resolution_evidence_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `transition_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `created_at` timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (`audit_id`),
  UNIQUE KEY `uq_population_director_binding_event` (`realm_id`, `event_key`),
  UNIQUE KEY `uq_population_director_binding_generation` (`realm_id`, `next_generation`),
  CONSTRAINT `fk_population_director_binding_schedule`
    FOREIGN KEY (`realm_id`) REFERENCES `population_director_schedule` (`realm_id`),
  CONSTRAINT `chk_population_director_binding_generation`
    CHECK (`expected_generation` > 0 AND `next_generation` = `expected_generation` + 1),
  CONSTRAINT `chk_population_director_binding_previous_pair`
    CHECK ((`previous_account_id` IS NULL) = (`previous_character_guid` IS NULL))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Append-only owner bind/rebind evidence; never authorizes a wave';

CREATE TABLE `population_director_wave_state` (
  `realm_id` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `owner_level` tinyint unsigned NOT NULL,
  `wave_idempotency_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `planned_count` int unsigned NOT NULL,
  `admitted_count` int unsigned NOT NULL,
  `state` enum('planned','authorized','admitting','active','blocked') NOT NULL,
  `authorized_at` timestamp(6) NULL DEFAULT NULL,
  `generation` bigint unsigned NOT NULL,
  PRIMARY KEY (`realm_id`, `owner_level`),
  UNIQUE KEY `uq_population_director_wave_key` (`realm_id`, `wave_idempotency_key`),
  CONSTRAINT `fk_population_director_wave_schedule`
    FOREIGN KEY (`realm_id`) REFERENCES `population_director_schedule` (`realm_id`),
  CONSTRAINT `chk_population_director_wave_level` CHECK (`owner_level` BETWEEN 1 AND 80),
  CONSTRAINT `chk_population_director_wave_count` CHECK (`admitted_count` <= `planned_count`),
  CONSTRAINT `chk_population_director_wave_active_count`
    CHECK (`state` <> 'active' OR `admitted_count` = `planned_count`),
  CONSTRAINT `chk_population_director_wave_generation` CHECK (`generation` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Immutable wave allocation plus durable authorization state';

CREATE TABLE `population_director_manifest_identity` (
  `realm_id` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `manifest_identity` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `manifest_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `wave_idempotency_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `owner_level` tinyint unsigned NOT NULL,
  `wave_ordinal` int unsigned NOT NULL,
  `account_name` varchar(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `account_manifest_size` tinyint unsigned NOT NULL,
  `character_name` varchar(12) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
  `faction` varchar(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `race` tinyint unsigned NOT NULL,
  `class` tinyint unsigned NOT NULL,
  `role_intent` varchar(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `spec_template` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `starting_region` varchar(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `admission_eligible` tinyint unsigned NOT NULL,
  `gender` tinyint unsigned NOT NULL,
  `skin` tinyint unsigned NOT NULL,
  `face` tinyint unsigned NOT NULL,
  `hair_style` tinyint unsigned NOT NULL,
  `hair_color` tinyint unsigned NOT NULL,
  `facial_hair` tinyint unsigned NOT NULL,
  `outfit_id` tinyint unsigned NOT NULL,
  PRIMARY KEY (`realm_id`, `manifest_identity`),
  UNIQUE KEY `uq_population_director_manifest_wave_ordinal`
    (`realm_id`, `owner_level`, `wave_ordinal`),
  UNIQUE KEY `uq_population_director_manifest_account_class`
    (`realm_id`, `account_name`, `class`),
  UNIQUE KEY `uq_population_director_manifest_character_name`
    (`realm_id`, `character_name`),
  KEY `ix_population_director_manifest_candidate`
    (`realm_id`, `owner_level`, `admission_eligible`, `wave_ordinal`),
  CONSTRAINT `fk_population_director_manifest_schedule`
    FOREIGN KEY (`realm_id`) REFERENCES `population_director_schedule` (`realm_id`),
  CONSTRAINT `fk_population_director_manifest_wave`
    FOREIGN KEY (`realm_id`, `owner_level`)
    REFERENCES `population_director_wave_state` (`realm_id`, `owner_level`),
  CONSTRAINT `chk_population_director_manifest_level` CHECK (`owner_level` BETWEEN 1 AND 80),
  CONSTRAINT `chk_population_director_manifest_ordinal` CHECK (`wave_ordinal` > 0),
  CONSTRAINT `chk_population_director_manifest_account_size`
    CHECK (`account_manifest_size` BETWEEN 1 AND 9),
  CONSTRAINT `chk_population_director_manifest_race` CHECK (`race` BETWEEN 1 AND 11),
  CONSTRAINT `chk_population_director_manifest_class`
    CHECK (`class` IN (1,2,3,4,5,7,8,9,11)),
  CONSTRAINT `chk_population_director_manifest_role`
    CHECK (`role_intent` IN ('tank','healer','damage')),
  CONSTRAINT `chk_population_director_manifest_eligible` CHECK (`admission_eligible` IN (0,1)),
  CONSTRAINT `chk_population_director_manifest_gender` CHECK (`gender` BETWEEN 0 AND 1),
  CONSTRAINT `chk_population_director_manifest_outfit` CHECK (`outfit_id` = 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Immutable hash-bound RandomBot identities created once at level one';

CREATE TABLE `population_director_identity` (
  `realm_id` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `manifest_identity` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `manifest_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `wave_idempotency_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `owner_level` tinyint unsigned NOT NULL,
  `wave_ordinal` int unsigned NOT NULL,
  `expected_account_name` varchar(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `expected_account_manifest_size` tinyint unsigned NOT NULL,
  `expected_character_name` varchar(12) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
  `expected_race` tinyint unsigned NOT NULL,
  `expected_class` tinyint unsigned NOT NULL,
  `expected_gender` tinyint unsigned NOT NULL,
  `expected_skin` tinyint unsigned NOT NULL,
  `expected_face` tinyint unsigned NOT NULL,
  `expected_hair_style` tinyint unsigned NOT NULL,
  `expected_hair_color` tinyint unsigned NOT NULL,
  `expected_facial_hair` tinyint unsigned NOT NULL,
  `expected_outfit_id` tinyint unsigned NOT NULL,
  `state` enum(
    'reserved','account_pending','account_verified','character_pending',
    'character_verified','rndbot_pending','binding_verified','active','blocked_conflict'
  ) NOT NULL,
  `generation` bigint unsigned NOT NULL,
  `account_id` int unsigned NULL,
  `character_guid` int unsigned NULL,
  `last_error` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NULL,
  `created_at` timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  `updated_at` timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  `activated_at` timestamp(6) NULL DEFAULT NULL,
  PRIMARY KEY (`realm_id`, `manifest_identity`),
  UNIQUE KEY `uq_population_director_identity_wave_ordinal`
    (`realm_id`, `owner_level`, `wave_ordinal`),
  UNIQUE KEY `uq_population_director_identity_account_class`
    (`realm_id`, `expected_account_name`, `expected_class`),
  UNIQUE KEY `uq_population_director_identity_character_name`
    (`realm_id`, `expected_character_name`),
  UNIQUE KEY `uq_population_director_identity_character_guid` (`realm_id`, `character_guid`),
  KEY `ix_population_director_identity_account_name` (`realm_id`, `expected_account_name`),
  KEY `ix_population_director_identity_account_id` (`realm_id`, `account_id`),
  KEY `ix_population_director_identity_state`
    (`realm_id`, `owner_level`, `state`, `wave_ordinal`),
  CONSTRAINT `fk_population_director_identity_schedule`
    FOREIGN KEY (`realm_id`) REFERENCES `population_director_schedule` (`realm_id`),
  CONSTRAINT `fk_population_director_identity_wave`
    FOREIGN KEY (`realm_id`, `owner_level`)
    REFERENCES `population_director_wave_state` (`realm_id`, `owner_level`),
  CONSTRAINT `fk_population_director_identity_manifest`
    FOREIGN KEY (`realm_id`, `manifest_identity`)
    REFERENCES `population_director_manifest_identity` (`realm_id`, `manifest_identity`),
  CONSTRAINT `chk_population_director_identity_generation` CHECK (`generation` > 0),
  CONSTRAINT `chk_population_director_identity_guid_pair`
    CHECK (`character_guid` IS NULL OR `account_id` IS NOT NULL),
  CONSTRAINT `chk_population_director_identity_active`
    CHECK (
      `state` <> 'active'
      OR (`account_id` IS NOT NULL AND `character_guid` IS NOT NULL AND `activated_at` IS NOT NULL)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Forward-only create-once identity recovery ledger';

CREATE TABLE `population_director_audit` (
  `audit_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `realm_id` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `event_key` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `manifest_identity` varchar(128) CHARACTER SET ascii COLLATE ascii_bin NULL,
  `event_type` varchar(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `from_state` varchar(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
  `to_state` varchar(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
  `generation` bigint unsigned NOT NULL,
  `payload_digest` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `payload_json` json NOT NULL,
  `created_at` timestamp(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (`audit_id`),
  UNIQUE KEY `uq_population_director_audit_event` (`realm_id`, `event_key`),
  KEY `ix_population_director_audit_identity`
    (`realm_id`, `manifest_identity`, `created_at`),
  CONSTRAINT `fk_population_director_audit_schedule`
    FOREIGN KEY (`realm_id`) REFERENCES `population_director_schedule` (`realm_id`),
  CONSTRAINT `chk_population_director_audit_generation` CHECK (`generation` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='Append-only idempotent population transition audit';
