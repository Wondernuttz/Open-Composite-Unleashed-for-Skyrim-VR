"""Test the opt-in native menu-laser contract without loading runtime DLLs."""
import importlib.util
from pathlib import Path
import unittest

source = Path(__file__).resolve().parents[1] / "scripts/VerifyUserPackage.py"
spec = importlib.util.spec_from_file_location("verify_user_package", source)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class MenuLaserPackageTests(unittest.TestCase):
    quad_modes = "always_show_quad=0\nshow_calibration_quad=0\nshow_profile_quad=0\n"

    def verify(self, quad="enable_laser=1\n", config="", modes=None):
        modes = self.quad_modes if modes is None else modes
        data = {"root/menu_quad_settings.ini": (modes + quad).encode(),
                "root/opencomposite.ini": config.encode()}
        package.verify_menu_lasers(data.__getitem__)

    def test_disabled_calibration_default_rejected(self):
        with self.assertRaisesRegex(AssertionError, "menu_quad_settings"):
            self.verify("enable_laser=0\n")

    def test_enabled_quad_and_default_global_gate(self):
        self.verify("[menu_quad]\nenable_laser=1\n")

    def test_global_explicit_opt_out_rejected(self):
        for section in ("", "[default]\n", "[general]\n", "[laser_aim]\n"):
            for value in ("false", "OFF", "disabled"):
                with self.subTest(section=section, value=value):
                    with self.assertRaisesRegex(AssertionError, "menuLaserEnabled"):
                        self.verify(config=section + "menuLaserEnabled=" + value + "\n")

    def test_global_enabled_aliases_and_comments(self):
        for value in ("true", "ON", "enabled"):
            self.verify(config="[general]\nmenuLaserEnabled: " + value + " ; enabled test profile\n")

    def test_calibration_matches_native_reader(self):
        for quad in (" enable_laser=1\n", "Enable_Laser=1\n",
                     ";enable_laser=1\n", "enable_laser=1\nenable_laser=0\n"):
            with self.subTest(quad=quad):
                with self.assertRaisesRegex(AssertionError, "menu_quad_settings"):
                    self.verify(quad)
        self.verify("enable_laser=0\nenable_laser=-1\n")

    def test_global_section_scope_and_last_assignment(self):
        self.verify(config="[keyboard]\nmenuLaserEnabled=false\n")
        self.verify(config="MenuLaserEnabled=false\n")
        self.verify(config="[general]\nmenuLaserEnabled=false\n[default]\nmenuLaserEnabled=true\n")
        with self.assertRaisesRegex(AssertionError, "menuLaserEnabled"):
            self.verify(config="[general]\nmenuLaserEnabled=true\n[default]\nmenuLaserEnabled=false\n")

    def test_calibration_whitespace_after_equals(self):
        self.verify("enable_laser= +1\n")
        with self.assertRaisesRegex(AssertionError, "menu_quad_settings"):
            self.verify("enable_laser=1\nenable_laser= 0\n")

    def test_calibration_unicode_is_not_a_native_integer(self):
        for quad in ("enable_laser=١\n", "\ufeffenable_laser=1\n"):
            with self.subTest(quad=quad):
                with self.assertRaisesRegex(AssertionError, "menu_quad_settings"):
                    self.verify(quad)

    def test_global_hash_is_not_an_inline_comment(self):
        with self.assertRaisesRegex(AssertionError, "Invalid"):
            self.verify(config="menuLaserEnabled=true # not a native inline comment\n")

    def test_global_indented_continuation(self):
        with self.assertRaisesRegex(AssertionError, "menuLaserEnabled"):
            self.verify(config="menuLaserEnabled=true\n false\n")
        self.verify(config="menuLaserEnabled=false\n ON\n")
        with self.assertRaisesRegex(AssertionError, "Invalid"):
            self.verify(config="menuLaserEnabled=true\n false ; continuation is verbatim\n")

    def test_enabled_diagnostic_quad_rejected(self):
        for name in ("always_show_quad", "show_calibration_quad", "show_profile_quad"):
            for value in ("1", "-1", " +1"):
                with self.subTest(name=name, value=value):
                    with self.assertRaisesRegex(AssertionError, name):
                        self.verify("enable_laser=1\n" + name + "=" + value + "\n")

    def test_explicit_quiet_defaults_required(self):
        for line in self.quad_modes.splitlines(keepends=True):
            with self.subTest(line=line):
                with self.assertRaisesRegex(AssertionError, line.split("=")[0]):
                    self.verify(modes=self.quad_modes.replace(line, ""))

    def test_diagnostic_flags_follow_native_reader(self):
        self.verify("enable_laser=1\nshow_calibration_quad=1\nshow_calibration_quad=0\n")
        for line in (" show_calibration_quad=0", "Show_calibration_quad=0",
                     ";show_calibration_quad=0", "show_calibration_quad=\u0660"):
            with self.subTest(line=line):
                with self.assertRaisesRegex(AssertionError, "show_calibration_quad"):
                    self.verify("enable_laser=1\nshow_calibration_quad=1\n" + line + "\n")

    def test_invalid_global_boolean_rejected(self):
        with self.assertRaisesRegex(AssertionError, "Invalid"):
            self.verify(config="menuLaserEnabled=1\n")


if __name__ == "__main__":
    unittest.main()
