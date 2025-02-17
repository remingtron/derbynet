<?php
// $_POST['roundid']

//===============================
//
// The search space for injecting a new racer:
//
// First, pick a possibly-empty set of lanes ("skip lanes") for which new heats
// will be added to incorporate a run of the new racer in the designated lane.
//
// For a given set of "skip lanes", then generate proposals for how to fill the
// lanes for a first new heat.
//
// If there are more than one "skip lanes", it's presumably because the new
// racer was added in too late to be accommodated well in existing heats.
//
// A total of no more than search_budget proposals will be considered, across
// all considered skip_lanes.  * That's a bug: there's always a max_skip_lanes
// possibility; injection shouldn't fail.
//
//===============================


// TODO Strongly assumes 1 run per car per lane.  Enforce this constraint if it
// can't be removed.

// TODO intval(read_raceinfo('unused-lane-mask', 0)) -- apply when reading or
// writing to the database; continue to use a "dense" race_chart.

// TODO score of 0 for injections that create an extra heat when none is really
// needed.  (Esp. k racers with k-1 heats.)

// TODO master schedule

require_once('inc/schedule_one_round.inc');

define('DEBUG', true);
define('DEBUG_SEARCH', true);
define('DEBUG_SCORING', true);

class ScheduleAdjuster {
  private $roundid;
  private $nlanes;
  private $nheats;



  // An array of indices into $race_chart of the heats that have been completed.
  private $completed_heats;

  // Number of heats that don't yet have results
  private $nheats_to_run;

  // Number of distinct heats with byes.
  private $nheats_with_byes;

  // An array of heats, each heat being an array of racerids.
  // Each heat has $nlanes elements, with byes represented as nulls.
  private $race_chart;

  // An array of arrays mapping pairs of racers to a count of their encounters
  // in the chart.  Indexing for both arrays is by racerid.
  private $matchups;

  // A goal is to keep the number of matchups between any two racers in a narrow
  // range.
  private $min_matchups;
  private $max_matchups;

  // Consider no more than $search_budget more proposals for how to construct a
  // new heat and swap the new racer into old heats.
  private $search_budget;

  private $best_score;
  private $best_proposal;
  // The new heat to be added will place the new racer in the skip_lanes[0], and
  // leave any additional skip_lanes as byes.
  private $best_skip_lanes;


  public function __construct($roundid) {
    $this->roundid = $roundid;
    $this->nlanes = get_lane_count();
    $this->compute_completed_heats();
    $this->reload_from_database();
  }

  public function number_of_completed_heats() {
    return count($this->completed_heats);
  }

  public function number_of_usable_lanes() {
    // TODO lane mask
    return $this->nlanes;
  }

  private function reload_from_database() {
    $this->read_race_chart();
    $this->compute_matchups();
    $this->compute_matchup_range();
  }

  // Argument is in [0 .. $this->nlanes).
  // Result is a value suitable for RaceChart.lane.
  private function lane_number($lane_index) {
    // TODO lane masking
    return $lane_index + 1;
  }

  // Returns true if racerid $new_racer can be "injected" into the schedule: one
  // new heat will be added, and opponents in each lane will be swapped in from
  // existing heats, replaced by $new_heat in each of the existing heats.
  public function inject($new_racer) {
    $start = microtime(true);
    $this->generate_injection_proposals();
    $stop = microtime(true);
    // echo "<injection-outcome>\n";  // TODO
    if ($this->best_proposal !== null) {
      // echo "<best-score>".$this->best_score."</best-score>\n";  // TODO
      // echo "<skip-lanes>".json_encode($this->best_skip_lanes)."</skip-lanes>";  // TODO
      // echo "<proposal>".json_encode($this->best_proposal)."</proposal>\n";  // TODO
    }
    // echo "<timing>".number_format($stop - $start, 4)."</timing>\n";  // TODO
    // echo "<unspent-budget>".$this->search_budget."</unspent-budget>\n";  // TODO
    // echo "</injection-outcome>\n";  // TODO

    if ($this->best_proposal === null) {
      return false;
    }

    $this->apply_proposal($this->best_proposal, $this->best_skip_lanes,
                          $new_racer);
    $this->reload_from_database();
    return true;
  }

  private function read_race_chart() {
    global $db;
    $this->race_chart = array();
    $stmt = $db->prepare('SELECT heat, lane, racerid FROM RaceChart'
                         .' WHERE roundid = :roundid'
                         .' ORDER BY heat, lane');
    $stmt->execute(array(':roundid' => $this->roundid));
    foreach ($stmt as $row) {
      while ($row['heat'] > count($this->race_chart)) {
        $this->race_chart[] = array_fill(0, $this->nlanes, null);
      }
      $this->race_chart[$row['heat'] - 1][$row['lane'] - 1] = intval($row['racerid']);
    }

    $this->nheats = count($this->race_chart);
    $this->nheats_to_run = $this->nheats - count($this->completed_heats);
    $this->nheats_with_byes = 0;
    for ($heat = 0; $heat < $this->nheats; ++$heat) {
      if (in_array(null, $this->race_chart[$heat], true) &&
          !in_array($heat, $this->completed_heats, true)) {
        ++$this->nheats_with_byes;
      }
    }

    if (DEBUG) {
      json_out('race-chart', $this->race_chart);
    }
  }

  private function compute_matchups() {
    $this->matchups = array();
    foreach ($this->race_chart as $heat) {
      foreach ($heat as $racer1) {
        if (!$racer1) continue;
        if (!isset($this->matchups[$racer1])) {
          $this->matchups[$racer1] = array();
        }
        foreach ($heat as $racer2) {
          if (!$racer2 || $racer1 == $racer2) {
            continue;
          }
          $prev = 0;
          if (isset($this->matchups[$racer1][$racer2])) {
            $prev = $this->matchups[$racer1][$racer2];
          }
          $this->matchups[$racer1][$racer2] = $prev + 1;
        }
      }
    }

    if (DEBUG_SCORING) {
      json_out('matchups', $this->matchups);
    }
  }

  private function compute_matchup_range() {
    $this->max_matchups = 0;
    $this->min_matchups = 1000;
    foreach ($this->matchups as $racer1 => $pairings) {
      foreach ($pairings as $racer2 => $p) {
        if ($p > $this->max_matchups) {
          $this->max_matchups = $p;
        }
        if ($p < $this->min_matchups) {
          $this->min_matchups = $p;
        }
      }
      if ($this->min_matchups > 0) {
        foreach ($this->matchups as $racer2 => $ignore) {
          if ($racer1 != $racer2 && !isset($pairings[$racer2])) {
            $this->min_matchups = 0;
          }
        }
      }
    }

    if (DEBUG) {
      json_out('matchups-summary', array('min' => $this->min_matchups,
                                         'max' => $this->max_matchups,
                                         'range' => $this->max_matchups - $this->min_matchups));
    }
  }

  function compute_completed_heats() {
    global $db;
    $this->completed_heats = array();
    $stmt = $db->prepare('SELECT DISTINCT heat FROM RaceChart'
                         .' WHERE roundid = :roundid'
                         .' AND (finishtime IS NOT NULL'
                         .'      OR finishplace IS NOT NULL)');
    $stmt->execute(array(':roundid' => $this->roundid));
    foreach ($stmt as $row) {
      $this->completed_heats[] = $row['heat'] - 1;
    }
  }

  // $proposal is an array of $nlanes elements, each of which is
  // either null (no swap) or a heat index.
  function score_proposal(&$proposal, $proposed_new_heat) {
    $score = -min($this->nheats_with_byes, $this->nlanes);
    if (DEBUG_SCORING) json_out('initial-score', $score);
    for ($lane = 0; $lane < $this->nlanes; ++$lane) {
      if ($proposal[$lane] !== null) {
        $candidate_heat_index = $proposal[$lane];
        $candidate_heat = &$this->race_chart[$candidate_heat_index];
        $candidate_racer = $candidate_heat[$lane];
        if ($candidate_racer === null) {
          $score += 1;
          if (DEBUG_SCORING) json_out('filled-bye', "Lane $lane");
          continue;
        }
        // Scan the old heat looking for matchups lost
        for ($opponent_lane = 0; $opponent_lane < $this->nlanes; ++$opponent_lane) {
          if ($lane != $opponent_lane) {
            $old_opponent = $candidate_heat[$opponent_lane];
            if ($old_opponent == null) continue;
            // if $old_opponent also appears in $proposed_new_heat, then we're neither
            // increasing nor decreasing the number of matchups between
            // $candidate_racer and that opponent.
            if (DEBUG_SCORING) {
              //echo "<score>In lane ".$lane.": Lost matchup between ".$candidate_racer
              //  ." and ".$old_opponent
              //  .", decreasing matchup count from "
              //  .$this->matchups[$candidate_racer][$old_opponent]."</score>\n";
            }
            if ($this->matchups[$candidate_racer][$old_opponent] ==
                   $this->min_matchups) {
              if (!in_array($old_opponent, $proposed_new_heat)) {
                // We're losing a matchup with $old_opponent here, and not making
                // it up in the new heat, so earns a negative score.
                $score -= 10;
                // if (DEBUG_SCORING) echo "<ding/>\n";
              }
            }
          }
        }
        // Scan the new heat looking for matchups added
        for ($o = 0; $o < $lane; ++$o) {
          $new_opponent = $proposed_new_heat[$o];
          if ($new_opponent == null) continue;
          if (DEBUG_SCORING) {
            //echo "<score>In lane ".$lane.": Added matchup between "
            //    .$candidate_racer." and ".$new_opponent
            //    .", increasing matchup count from "
            //    .$this->matchups[$candidate_racer][$new_opponent]
            //    ."</score>\n";
          }
          if ($this->matchups[$candidate_racer][$new_opponent] == $this->max_matchups) {
            if (!in_array($new_opponent, $candidate_heat)) {
              // We're adding another matchup with $new_opponent when we've already
              // had as many as we should, and we're not eliminating one of those
              // matchups from the old heat.
              $score -= 10;
              // if (DEBUG_SCORING) echo "<ding/>\n";
            }
          }
        }
      }
    }
    return $score;
  }

  private function generate_injection_proposals() {
    $this->best_score = -10000;
    $this->search_budget = 10000;
    $proposal = array_fill(0, $this->nlanes, null);
    $proposed_new_heat = array_fill(0, $this->nlanes, null);

    $max_heats_to_add = max(1, $this->nlanes - $this->nheats_to_run);
    for ($nskip = 0; $nskip <= $max_heats_to_add; ++$nskip) {
      // $skip_lanes is an array of lane indices to leave blank in the proposal
      // -- we expect to add a heat with the new racer in the $skip_lane[0]'th
      // lane.  We want as few skip-lane entries as possible; each represents a
      // tacked-on heat with the new racer racing alone.
      $skip_lanes = array_fill(0, $nskip, null);
      $this->generate_lanes_to_skip_for_injection_proposals(/* skip_i */ 0, $skip_lanes,
                                                            $proposal, $proposed_new_heat);
    }
  }

  // Recursively fills out the $skip_lanes array with distinct lane indices,
  // then starts generate_injection_proposals_recur with the result.
  //
  // $skip_i is an index into $skip_lanes
  private function generate_lanes_to_skip_for_injection_proposals(
        $skip_i, &$skip_lanes, &$proposal, &$proposed_new_heat) {
    if ($skip_i >= count($skip_lanes)) {
      $this->generate_injection_proposals_recur(/* lane */ 0, $skip_lanes,
                                                $proposal, $proposed_new_heat);
    } else {
      // Pick a lane to fill the skip_i'th element of skip_lanes
      for ($lane = 0; $lane < $this->nlanes; ++$lane) {
        if ($this->search_budget <= 0) break;
        if (in_array($lane, $skip_lanes, true)) continue;
        $skip_lanes[$skip_i] = $lane;
        $this->generate_lanes_to_skip_for_injection_proposals(
          $skip_i + 1, $skip_lanes, $proposal, $proposed_new_heat);
      }
      $skip_lanes[$skip_i] = null;  // Clean up for next time
    }
  }
  
  // Generates proposals for injecting a new racer into the schedule.
  //
  // $skip_lanes is an array of lane indices to leave blank in the proposal --
  // we expect to add a heat with the new racer in the $skip_lane[0]'th lane.
  private function generate_injection_proposals_recur(
        $lane, $skip_lanes, &$proposal, &$proposed_new_heat) {
    if ($this->best_score >= 0) return;  // Short-circuit on optimal choice
    if ($this->search_budget <= 0) {
      // echo "<over-budget/>\n";
      return;
    }

    if ($lane >= $this->nlanes) {
      --$this->search_budget;
      // if ($this->search_budget <= 0) echo "<budget-expired/>\n";
      if (DEBUG_SEARCH) {
        // echo "<proposal>\n";
        // echo "<heats>".json_encode($proposal)."</heats>\n";
      }
      $score = $this->score_proposal($proposal, $proposed_new_heat);
      if (DEBUG_SEARCH) {
        // echo "<score>".$score."</score>\n";
      }
      if ($score > $this->best_score) {
        $this->best_score = $score;
        $this->best_skip_lanes = $skip_lanes;
        $this->best_proposal = $proposal;
        if (DEBUG_SEARCH) {
          // echo "<best-score>".$score." for ".json_encode($proposal)
          //     ." with skip_lanes ".json_encode($skip_lanes)."</best-score>\n";
        }
      }
      if (DEBUG_SEARCH) {
        // echo "</proposal>\n";
      }
    } else if (in_array($lane, $skip_lanes, true)) {
      $this->generate_injection_proposals_recur($lane + 1, $skip_lanes,
                                                $proposal, $proposed_new_heat);
    } else {
      for ($heat = 0; $heat < $this->nheats; ++$heat) {
        if ($this->best_score >= 0 || $this->search_budget <= 0) return;
        if (!in_array($heat, $this->completed_heats, true) &&
            !in_array($heat, $proposal, true)) {
          $racer = $this->race_chart[$heat][$lane];
          // We have to make sure we don't have the same car showing up twice in
          // the new heat.  There isn't supposed to be a new heat if
          // count($skip_lanes) == 0.
          if ($racer === null ||
              (count($skip_lanes) > 0 && !in_array($racer, $proposed_new_heat))) {
            $proposal[$lane] = $heat;
            $proposed_new_heat[$lane] = $racer;
            $this->generate_injection_proposals_recur($lane + 1, $skip_lanes,
                                                      $proposal, $proposed_new_heat);
            $proposal[$lane] = null;
            $proposed_new_heat[$lane] = null;
          }
        }
      }
    }
  }

  // For each lane mentioned in $skip_lanes, a new heat will be added with new
  // racer running in the specific lane.
  //
  // For lanes not mentioned in $skip_lanes, $proposal[$lane] gives a heat
  // index, and the new racer gets swapped in that existing heat in that lane.
  // The racer that previously occupied that spot gets moved to the first added
  // heat.
  //
  private function apply_proposal(&$proposal, &$skip_lanes, $new_racer) {
    global $db;
    $update_stmt = $db->prepare('UPDATE RaceChart SET racerid = :racerid'
                                .' WHERE roundid = :roundid'
                                .' AND heat = :heat'
                                .' AND lane = :lane');

    $insert_stmt = $db->prepare('INSERT INTO RaceChart(roundid, heat, lane, racerid)'
                                .' VALUES(:roundid, :heat, :lane, :racerid)');

    for ($lane = 0; $lane < $this->nlanes; ++$lane) {
      if (!in_array($lane, $skip_lanes, true)) {
        // Replace existing racers in the proposal with the new racer
        $update_stmt->execute(array(':racerid' => $new_racer,
                                    ':roundid' => $this->roundid,
                                    ':heat' => $proposal[$lane] + 1,
                                    ':lane' => $this->lane_number($lane)));

        if ($proposal[$lane] !== null) {
          if ($this->race_chart[$proposal[$lane]][$lane] === null) {
            // Replace byes in the proposal with the new racer, by inserting RaceChart entries
            $insert_stmt->execute(array(':racerid' => $new_racer,
                                        ':roundid' => $this->roundid,
                                        ':heat' => $proposal[$lane] + 1,
                                        ':lane' => $this->lane_number($lane)));

          } else {
            // Otherwise move the old racer to the (first) new heat.
            $insert_stmt->execute(array(':racerid' => $this->race_chart[$proposal[$lane]][$lane],
                                        ':roundid' => $this->roundid,
                                        ':heat' => $this->nheats + 1,
                                        ':lane' => $this->lane_number($lane)));
          }
        }
      }
    }

    // Add the new racer to any new heats.  The number of new heats = size of $skip_lanes
    for ($skip_i = 0; $skip_i < count($skip_lanes); ++$skip_i) {
      $lane = $skip_lanes[$skip_i];
      $insert_stmt->execute(array(':racerid' => $new_racer,
                           ':roundid' => $this->roundid,
                           ':heat' => $this->nheats + $skip_i + 1,
                           ':lane' => $this->lane_number($lane)));
    }
  }
}

function n_runs_per_lane($roundid) {
  global $db;

  $nracers = read_single_value('SELECT COUNT(DISTINCT racerid) FROM RaceChart'
                               .' WHERE roundid = :roundid',
                               array(':roundid' => $roundid));
  if ($nracers == 0) {
    return 0;
  }
  $nheats = read_single_value('SELECT COUNT(DISTINCT heat) FROM RaceChart'
                              .' WHERE roundid = :roundid',
                              array(':roundid' => $roundid));
  $n_runs_per_lane = $nheats / $nracers;
  if (!is_int($n_runs_per_lane)) {
    return 0;
  }
  return $n_runs_per_lane;
}

function deleted_racers($roundid) {
  global $db;
  $result = array();
  $stmt = $db->prepare('SELECT DISTINCT RaceChart.racerid FROM RaceChart'
                       .' INNER JOIN RegistrationInfo'
                       .' ON RaceChart.racerid = RegistrationInfo.racerid'
                       .' WHERE roundid = :roundid'
                       .' AND passedinspection = 0');
  $stmt->execute(array(':roundid' => $roundid));
  foreach ($stmt as $row) {
    $result[] = $row['racerid'];
  }
  return $result;
}

function added_racers($roundid) {
  global $db;
  $result = array();
  $stmt = $db->prepare('SELECT ri.racerid FROM RegistrationInfo ri'
                       .' WHERE classid = '
                       .'    (SELECT classid FROM Rounds'
                       .'      WHERE roundid = :roundid)'
                       .'  AND passedinspection = 1'
                       .'  AND NOT EXISTS(SELECT 1 FROM RaceChart'
                       .'    WHERE roundid = :roundid2'
                       .'      AND racerid = ri.racerid)');
  $stmt->execute(array(':roundid' => $roundid,
                       ':roundid2' => $roundid));
  foreach ($stmt as $row) {
    $result[] = $row['racerid'];
  }
  return $result;
}

if (have_permission(CONTROL_RACE_PERMISSION)) {
  $roundid = $_POST['roundid'];

  $deleted = deleted_racers($roundid);
  if (count($deleted) > 0) json_out('deleted-racers', $deleted);
  $added = added_racers($roundid);
  if (count($added) > 0) json_out('added-racers', $added);

  $ok = true;

  if (count($deleted) == 0 && count($added) == 0) {
    json_failure('nothingtodo', "Nothing to do");
    $ok = false;
  }

  $n_runs_per_lane = n_runs_per_lane($roundid);
  if ($n_runs_per_lane == 0) {
    json_failure('unrecognizable', "Schedule not recognizable");
    $ok = false;
  }

  $adjuster = new ScheduleAdjuster($roundid);
  
  if ($adjuster->number_of_completed_heats() == 0) {
    json_out('regenerating', true);
    execute_once('DELETE FROM RaceChart WHERE roundid = :roundid',
                 array(':roundid' => $roundid));

	if (schedule_one_round($roundid, $adjuster->number_of_usable_lanes(),
                           $n_runs_per_lane, /* trace */ false)) {
      json_success();
    } else {
      json_failure('too-few-racers', 
                   "There need to be at least two racers checked in to generate a schedule.");
    }

    $ok = false;
  }
  
  if ($ok && count($deleted) > 0) {
    json_failure('deleted', "Not yet implemented: deleted racers");
    $ok = false;
  }

  if ($ok) {
    foreach ($added as $new_racer) {
      // TODO for ($run = 0; $run < $n_runs_per_lane; ++$run) ?
      if ($adjuster->inject($new_racer)) {
        json_out('successful-injection', true);
      } else {
        json_failure('no-injection', "Unsuccessful racer injection");
        $ok = false;
      }
    }
  }

  if ($ok) {
    json_success();
  }
} else {
  json_not_authorized();
}

?>
