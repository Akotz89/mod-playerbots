-- Preserve raw owner high-water while constraining the authorized epoch.
ALTER TABLE `population_director_owner_state`
  DROP CHECK `chk_population_director_owner_epoch`,
  ADD CONSTRAINT `chk_population_director_owner_epoch`
    CHECK (`current_epoch` BETWEEN 1 AND `highest_owner_level_seen`);
