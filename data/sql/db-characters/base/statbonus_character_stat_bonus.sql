-- ---------------------------------------------------------------------------
-- mod-statbonus: permanent flat additions to a character's stats and ratings
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
-- One row per character per bonus. A row is deleted rather than stored as zero,
-- so the table is a list of what has actually been granted.
--
-- Kind 0 is the Stats enum, kind 1 the CombatRating enum, and Id is an index
-- into whichever of those Kind names. They are held as the core's own two
-- enums rather than flattened into one numbering so the rows stay readable
-- against the core if the size of either ever changes:
--
--   Kind 0:  0 strength, 1 agility, 2 stamina, 3 intellect, 4 spirit
--   Kind 1:  0 weapon skill, 1 defense skill, 2 dodge, 3 parry, 4 block,
--            5 hit, 6 ranged hit, 7 spell hit, 8 crit, 9 ranged crit,
--            10 spell crit, 11 hit taken, 12 ranged hit taken,
--            13 spell hit taken, 14 crit taken, 15 ranged crit taken,
--            16 spell crit taken, 17 haste, 18 ranged haste, 19 spell haste,
--            20 mainhand weapon skill, 21 offhand weapon skill,
--            22 ranged weapon skill, 23 expertise, 24 armor penetration
--
-- ".statbonus list" prints these by name; the numbers are here for anyone
-- reading the table directly.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `character_stat_bonus` (
  `Guid`    int unsigned     NOT NULL              COMMENT 'characters.guid',
  `Kind`    tinyint unsigned NOT NULL DEFAULT '0'  COMMENT '0 primary stat (Stats), 1 combat rating (CombatRating)',
  `Id`      tinyint unsigned NOT NULL              COMMENT 'index into the enum named by Kind',
  `Amount`  int              NOT NULL DEFAULT '0'  COMMENT 'flat addition, may be negative; a shown stat is floored at 0',
  `Comment` varchar(255)     DEFAULT NULL          COMMENT 'why it was granted; for the GM, never shown to the player',
  PRIMARY KEY (`Guid`,`Kind`,`Id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-statbonus: flat stat and combat rating additions';
