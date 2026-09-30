-- ---------------------------------------------------------------------------
-- mod-statbonus: permanent flat additions to a character's primary stats
--
-- The core's database updater applies this at startup because it belongs to an
-- enabled module: UpdateFetcher::ReceiveIncludedDirectories walks
-- modules/<name>/data/sql/db-characters. Nothing has to be applied by hand.
--
-- CREATE TABLE IF NOT EXISTS, never DROP: a MODULE file is re-applied whenever
-- its hash changes, and this table holds granted bonuses rather than generated
-- content, so dropping it on an edit to the comments would silently revoke
-- every grant on the realm.
--
-- One row per character per stat. A row is deleted rather than stored as zero,
-- so the table is a list of what has actually been granted.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `character_stat_bonus` (
  `Guid`    int unsigned    NOT NULL                COMMENT 'characters.guid',
  `Stat`    tinyint unsigned NOT NULL               COMMENT '0 strength, 1 agility, 2 stamina, 3 intellect, 4 spirit',
  `Amount`  int             NOT NULL DEFAULT '0'    COMMENT 'flat addition, may be negative; the shown stat is floored at 0',
  `Comment` varchar(255)    DEFAULT NULL            COMMENT 'why it was granted; for the GM, never shown to the player',
  PRIMARY KEY (`Guid`,`Stat`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-statbonus: flat primary stat additions';
