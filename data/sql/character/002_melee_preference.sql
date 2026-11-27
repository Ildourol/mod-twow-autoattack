CREATE TABLE IF NOT EXISTS `mod_twow_autoattack_melee_preference` (
    `guid` INT UNSIGNED NOT NULL,
    `prefer_melee` TINYINT UNSIGNED NOT NULL,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB;
