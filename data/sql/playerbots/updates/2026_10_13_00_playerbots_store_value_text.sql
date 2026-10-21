-- Widen only the original nullable VARCHAR(255); never narrow an existing TEXT family.
SET @playerbots_store_value_ddl = COALESCE((
    SELECT IF(DATA_TYPE = 'varchar' AND CHARACTER_MAXIMUM_LENGTH = 255
        AND IS_NULLABLE = 'YES' AND EXTRA = ''
        AND (COLUMN_DEFAULT IS NULL OR (LOCATE('MariaDB', VERSION()) > 0 AND COLUMN_DEFAULT = 'NULL')),
        CONCAT('ALTER TABLE `playerbots_db_store` MODIFY COLUMN `value` TEXT CHARACTER SET `',
            CHARACTER_SET_NAME, '` COLLATE `', COLLATION_NAME, '` NULL COMMENT ', CHAR(39),
            REPLACE(REPLACE(COLUMN_COMMENT, CHAR(92),
                IF(FIND_IN_SET('NO_BACKSLASH_ESCAPES', @@sql_mode), CHAR(92), REPEAT(CHAR(92), 2))),
                CHAR(39), REPEAT(CHAR(39), 2)), CHAR(39)),
        'SELECT 1')
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'playerbots_db_store' AND COLUMN_NAME = 'value'
), 'SELECT 1');
PREPARE playerbots_store_value_stmt FROM @playerbots_store_value_ddl;
EXECUTE playerbots_store_value_stmt;
DEALLOCATE PREPARE playerbots_store_value_stmt;
