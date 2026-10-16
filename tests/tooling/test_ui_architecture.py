"""Reader UI policy and include traversal regressions."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from ui_architecture import UI_ROOT, ui_errors


class UIArchitectureTests(unittest.TestCase):
    def check(self, files):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, text in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            return ui_errors(root)

    def test_actual_reader_ui(self):
        self.assertEqual(ui_errors(ROOT), [])

    def test_indirect_dependency_and_angle_source_include(self):
        errors = self.check({
            'src/' + UI_ROOT + '/panel.cpp': '#include "helper.h"',
            'src/' + UI_ROOT + '/helper.h': '#include <reading/book.h>',
            'src/reading/book.h': '',
        })
        self.assertEqual(len(errors), 1)
        self.assertIn('helper.h -> src/reading/book.h', errors[0])

    def test_allowed_presentation_helper_cannot_hide_model_dependencies(self):
        for dependency in ('reading/book.h', 'storage/settings.h', 'platform/android/device.h'):
            with self.subTest(dependency=dependency):
                errors = self.check({
                    'src/' + UI_ROOT + '/panel.cpp': '#include "assets/font_catalog.h"',
                    'src/assets/font_catalog.h': '#include "' + dependency + '"',
                    'src/' + dependency: '',
                })
                self.assertEqual(len(errors), 1)
                self.assertIn('src/assets/font_catalog.h -> src/' + dependency, errors[0])

    def test_external_callers_cannot_reintroduce_retired_properties(self):
        errors = self.check({
            'src/interfaces/reader/window/reader_settings.cpp': 'button->setProperty("reader_side_buttons", true);',
            'src/interfaces/reader/reader_appearance.cpp': 'button->property("reader_icon_action");',
            'src/interfaces/reader/window/reader.cpp': '''
                button->setProperty("reader_dark", true);
                button->setStyleSheet("QPushButton[reader_style=\\"primary\\"] {}");
                const char *description = "reader_action_chevron reader_icon_action";
                const char *raw = R"(setProperty("reader_icon_action", true))";
                // button->property("reader_list_style");
            ''',
        })
        self.assertEqual(len(errors), 2)
        self.assertTrue(any('reader_settings.cpp' in error for error in errors))
        self.assertTrue(any('reader_appearance.cpp' in error for error in errors))

    def test_plain_colored_backgrounds_are_rejected(self):
        errors = self.check({'src/reader/panel.cpp': '''
            void readerStyle(QWidget *widget, const QString &source) {}
            readerStyle(bar, "background:palette(midlight);");
            readerStyle(note, styles::font(24) +
                              "color:black;background-color:#ddd;");
            readerStyle(label, "background:transparent;color:palette(mid);");
            readerStyle(page, "background:palette(window);");
            readerStyle(button, "QPushButton{background:palette(midlight);}");
            // readerStyle(old, "background:black;");
        '''})
        self.assertEqual(len(errors), 2, errors)
        self.assertIn('background: palette(midlight); use Panel::setFill', errors[0])
        self.assertIn('background: #ddd', errors[1])

    def test_cycles_comments_generated_and_public_qt(self):
        self.assertEqual(self.check({
            'src/' + UI_ROOT + '/a.h': '#include "b.h"\n#include "support/strings.h"\n#include <QtWidgets/QWidget>\n/* #include "reading/book.h" */',
            'src/' + UI_ROOT + '/b.h': '#include "a.h"\nconst char *s = R"tag(// setProperty("reader_icon_action", true))tag";',
            'src/support/strings.h': '#include "kobalt_strings.inc"',
        }), [])

    def test_unknown_local_and_private_qt(self):
        errors = self.check({'src/' + UI_ROOT + '/a.h': '#include "unknown.h"\n#include <QtGui/6.5/QtGui/private/qguiapplication_p.h>\n#include <qpa/qplatformscreen.h>\n#include <qwidget_p.h>'})
        self.assertEqual(len(errors), 4)
        self.assertTrue(any('unresolved local header' in error for error in errors))

    def test_include_examples_in_raw_literals_are_not_dependencies(self):
        self.assertEqual(self.check({'src/' + UI_ROOT + '/a.cpp': '''
            const char *example = R"example(
                #include "reading/book.h"
                #include <private/qwidget_p.h>
            )example";
        '''}), [])

    def test_root_escape_is_rejected_before_reading(self):
        errors = self.check({'src/' + UI_ROOT + '/a.h': '#include "../../../../escape.h"',
                             'escape.h': '#include "missing.h"'})
        self.assertEqual(len(errors), 1)
        self.assertIn('outside source root', errors[0])

    def test_retired_properties_only_in_property_use_or_stylesheet(self):
        errors = self.check({'src/' + UI_ROOT + '/a.cpp': '''
            button->setProperty("reader_action_chevron", true);
            button->property("reader_list_style");
            button->setStyleSheet("QPushButton[reader_toolbar_action=\\"true\\"] {}");
            button->setProperty("reader_dark", true);
            button->setProperty("reader_style", "normal");
            const char *description = "reader_icon_action reader_side_buttons";
            const char *raw = R"(setProperty("reader_icon_action", true))";
            // button->setProperty("reader_icon", 0);
        '''})
        self.assertEqual(len(errors), 3)
        self.assertTrue(all('retired visual' in error for error in errors))


if __name__ == '__main__':
    unittest.main()
