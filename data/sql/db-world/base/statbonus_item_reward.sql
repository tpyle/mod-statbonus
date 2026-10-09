-- ---------------------------------------------------------------------------
-- mod-statbonus: what an item grants when it is used, and how often
--
-- The same machinery as statbonus_quest_reward, keyed by item instead of
-- quest, for the shorter route: no quest, no broker to summon, no turn-in -
-- right-click the thing and the bonus is yours. One row makes an item
-- deterministic ("+1 stamina"), several rows sharing an ItemId make it a
-- gamble picked by weight.
--
-- Kind and Id are the same vocabulary as character_stat_bonus and the quest
-- table, so a row can award a primary stat, any combat rating, a resistance or
-- a movement percentage without anything being rebuilt:
--
--   Kind 0  Stats          0 strength, 1 agility, 2 stamina, 3 intellect, 4 spirit
--   Kind 1  CombatRating   0-24, so 5 hit, 8 crit, 17 haste, 23 expertise, ...
--   Kind 2  SpellSchools   0 armor, 1 holy, 2 fire, 3 nature, 4 frost, 5 shadow, 6 arcane
--   Kind 3  UnitMoveType   1 run, 3 swim, 6 flight - and Amount is a PERCENTAGE
--
-- Weight 0 retires a row without deleting it. A pool where every weight is 0
-- grants nothing and says so in the log, rather than quietly falling back to
-- the first row.
--
-- An item listed here always needs this much in item_template:
--
--   spellid_1 = <a spell the CLIENT knows>, spelltrigger_1 = 0
--
-- That is not optional and not cosmetic. Whether an item is usable at all -
-- the "Use:" line and whether right-clicking sends anything - is decided by
-- the client out of its own Spell.dbc, so an item with no spell, or with a
-- spell that exists only in the spell_dbc world table, shows no Use: line and
-- right-clicks into nothing. The client renders that spell's DESCRIPTION as
-- the Use: line, so the spell is also where the wording comes from.
--
-- Then pick which of the two grant paths it uses:
--
--   INSTANT   item_template.ScriptName = 'item_statbonus_grant'
--             The ItemScript returns true from OnUse, which stops
--             HandleUseItemOpcode before the spell is cast. Nothing is cast,
--             so the spell only has to exist - and so there can be no cast
--             bar. The script spends the item itself; leave spellcharges_1
--             at 0 or the two will both try.
--
--   CAST BAR  no ScriptName, spellcharges_1 = -1, and a spell_script_names
--             row binding the spell to 'spell_statbonus_grant'
--             The spell is cast for real, so the client draws a bar for
--             whatever the spell's CastingTimeIndex says and its
--             InterruptFlags decide what cancels it. The bonus lands when the
--             cast completes, and the core spends the item in
--             Spell::TakeCastItem at the end of Spell::cast - after the
--             effects - so an interrupted cast costs nothing.
--
-- Both read this table and share the roll, the limit check and the wording, so
-- the choice is only about how it feels to use.
--
-- The rows themselves are realm content and live in the realm's own SQL, not
-- here: this file is the mechanism.
--
-- CREATE TABLE IF NOT EXISTS, never DROP - a MODULE file is re-applied
-- whenever its hash changes, and dropping this would silently empty every
-- reward pool on the realm.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `statbonus_item_reward` (
  `ItemId`  int unsigned     NOT NULL              COMMENT 'item_template.entry',
  `Kind`    tinyint unsigned NOT NULL DEFAULT '0'  COMMENT 'as character_stat_bonus: 0 stat, 1 rating, 2 resistance, 3 movement',
  `Id`      tinyint unsigned NOT NULL              COMMENT 'index into the enum named by Kind',
  `Amount`  int              NOT NULL DEFAULT '1'  COMMENT 'flat for kinds 0-2, percentage for kind 3; may be negative',
  `Weight`  int unsigned     NOT NULL DEFAULT '1'  COMMENT 'relative chance within the item; 0 retires the row',
  `Comment` varchar(255)     DEFAULT NULL,
  PRIMARY KEY (`ItemId`,`Kind`,`Id`,`Amount`),
  KEY `idx_item` (`ItemId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-statbonus: item reward pools';
