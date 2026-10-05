-- ---------------------------------------------------------------------------
-- mod-statbonus: what a quest grants, and how often
--
-- One row per possible outcome. Several rows sharing a QuestId make the quest
-- random: one is picked by weight when it is turned in. A single row makes it
-- deterministic, which is how "pick your stat" would be built on the same
-- machinery - several quests, one row each, sharing a token.
--
-- The point of the table is that the set of things a quest can grant is data
-- rather than code. Kind and Id are the same vocabulary as
-- character_stat_bonus, so a row can award a primary stat, any combat rating,
-- a resistance or a movement percentage without anything being rebuilt:
--
--   Kind 0  Stats          0 strength, 1 agility, 2 stamina, 3 intellect, 4 spirit
--   Kind 1  CombatRating   0-24, so 5 hit, 8 crit, 17 haste, 23 expertise, ...
--   Kind 2  SpellSchools   0 armor, 1 holy, 2 fire, 3 nature, 4 frost, 5 shadow, 6 arcane
--   Kind 3  UnitMoveType   1 run, 3 swim, 6 flight - and Amount is a PERCENTAGE
--
-- Weight 0 retires a row without deleting it, which is the civilised way to
-- turn an outcome off while tuning. A pool where every weight is 0 grants
-- nothing and says so in the log, rather than quietly falling back to the
-- first row.
--
-- The rows themselves are realm content and live in the realm's own SQL
-- (sql/26_stat_token_quest.sql), not here: this file is the mechanism.
--
-- CREATE TABLE IF NOT EXISTS, never DROP - a MODULE file is re-applied
-- whenever its hash changes, and dropping this would silently empty every
-- reward pool on the realm.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `statbonus_quest_reward` (
  `QuestId` int unsigned     NOT NULL              COMMENT 'quest_template.ID',
  `Kind`    tinyint unsigned NOT NULL DEFAULT '0'  COMMENT 'as character_stat_bonus: 0 stat, 1 rating, 2 resistance, 3 movement',
  `Id`      tinyint unsigned NOT NULL              COMMENT 'index into the enum named by Kind',
  `Amount`  int              NOT NULL DEFAULT '1'  COMMENT 'flat for kinds 0-2, percentage for kind 3; may be negative',
  `Weight`  int unsigned     NOT NULL DEFAULT '1'  COMMENT 'relative chance within the quest; 0 retires the row',
  `Comment` varchar(255)     DEFAULT NULL,
  PRIMARY KEY (`QuestId`,`Kind`,`Id`,`Amount`),
  KEY `idx_quest` (`QuestId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-statbonus: quest reward pools';
