import json
import math
import unittest
from pathlib import Path


RECIPE_PATH = (
    Path(__file__).resolve().parents[2]
    / "UnitAuthoringRecipes"
    / "vat_skeletal_unit.massbattle_recipe.json"
)


class UnitAuthoringDefaultsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        recipe = json.loads(RECIPE_PATH.read_text(encoding="utf-8"))
        cls.recipe = recipe
        cls.defaults = recipe["default_unit_patch"]["Data"]

    def test_generated_units_disable_outgoing_launch_and_resist_live_knockback(self):
        self.assertFalse(
            self.defaults["Debuff"]["LaunchParams"]["bCanLaunch"]
        )
        self.assertEqual(self.defaults["Defence"]["LaunchImmuneAlive"], 1)

    def test_crowd_collision_defaults_use_two_simple_compaction_presets(self):
        presets = self.recipe["collider_mult_presets"]
        self.assertEqual(presets["default"], "slight_15_percent")
        self.assertEqual(presets["slight_15_percent"], 0.85)
        self.assertEqual(presets["noticeable_30_percent"], 0.7)

        physics = self.defaults["PhysicsShared"]
        self.assertEqual(
            physics["ColliderMult"],
            presets[presets["default"]],
        )
        self.assertNotIn("PositionRelaxation_Agent", physics)
        self.assertNotIn("VelocityRelaxation_Agent", physics)
        self.assertNotIn("PressureField", physics)
        self.assertNotIn("Avoidance", self.defaults)

    def test_movement_defaults_are_template_independent(self):
        xy = self.defaults["Move"]["XY"]
        self.assertEqual(xy["MoveSpeed"], 1000)
        self.assertEqual(xy["MoveAcceleration"], xy["MoveSpeed"] * 24)
        self.assertEqual(xy["MoveDeceleration"], xy["MoveSpeed"] * 24)
        self.assertAlmostEqual(xy["MoveSpeed"] / xy["MoveAcceleration"], 1 / 24)
        self.assertAlmostEqual(xy["MoveSpeed"] / xy["MoveDeceleration"], 1 / 24)
        self.assertEqual(xy["AcceptanceRadius"], 256)
        self.assertEqual(
            xy["MoveSpeedRangeMapByDist"]["Y"],
            xy["MoveSpeedRangeMapByDist"]["W"],
        )

    def test_movement_response_is_derived_from_move_speed(self):
        derivations = self.recipe["default_unit_patch_derivations"]
        self.assertEqual(len(derivations), 1)
        rule = derivations[0]
        self.assertEqual(rule["source"], "Data.Move.XY.MoveSpeed")
        self.assertEqual(
            rule["targets"],
            [
                "Data.Move.XY.MoveAcceleration",
                "Data.Move.XY.MoveDeceleration",
            ],
        )
        self.assertEqual(rule["multiplier"], 24)
        self.assertTrue(rule["unless_explicit"])

    def test_yaw_profile_completes_a_rest_to_rest_half_turn_in_one_third_second(self):
        yaw = self.defaults["Move"]["Yaw"]
        turn_time = 2 * math.sqrt(180 / yaw["TurnAcceleration"])
        peak_speed = yaw["TurnAcceleration"] * turn_time / 2

        self.assertAlmostEqual(turn_time, 1 / 3)
        self.assertAlmostEqual(peak_speed, yaw["TurnSpeed"])
        self.assertEqual(
            yaw["TurnSpeedRangeMapByMoveSpeed"]["Y"],
            yaw["TurnSpeedRangeMapByMoveSpeed"]["W"],
        )

    def test_rotate_before_move_profile_keeps_a_nonzero_direction_intent(self):
        angle_map = self.defaults["Move"]["XY"]["MoveSpeedRangeMapByAngle"]
        self.assertEqual(angle_map["Z"], 5)
        self.assertGreater(angle_map["W"], 0)
        self.assertLessEqual(angle_map["W"], 0.001)


if __name__ == "__main__":
    unittest.main()
