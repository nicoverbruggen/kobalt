#!/usr/bin/env python3
"""Check the reader UI boundary with a reusable local include traversal."""
from collections import deque
from pathlib import Path
import re

SUFFIXES = ('.c', '.cpp', '.h', '.hpp')
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"\n]+)[>"]', re.MULTILINE)
LITERAL = re.compile(r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)"|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
TOKENS = re.compile(LITERAL.pattern + r'|//[^\n]*|/\*[\s\S]*?\*/')


def blank(text):
    return ''.join('\n' if char == '\n' else ' ' for char in text)


def without_comments(text):
    return TOKENS.sub(lambda match: blank(match[0]) if match[0].startswith(('//', '/*')) else match[0], text)


def without_literals(text):
    return LITERAL.sub(lambda match: blank(match[0]), text)


def private_qt_header(name):
    return ('private' in name.split('/') or 'qpa' in name.split('/')
            or bool(re.fullmatch(r'(?:Qt[^/]+/)?q[^/]*_p\.h', name)))


def dependency_errors(root, folders, *, allowed_path=lambda path: True,
                      include_error=lambda name: False, code_errors=lambda text: (),
                      generated_headers=(), unresolved_is_error=True):
    """Follow source includes, reporting the chain to each rejected dependency.

    Policy callbacks receive source-relative paths, include names, and comment-free
    text. External angle headers are left to the compiler; local headers must exist.
    """
    root = Path(root).resolve()
    source = root / 'src'
    pending = deque((path.resolve(), [path.relative_to(root).as_posix()])
                    for folder in folders for path in sorted((source / folder).rglob('*'))
                    if path.is_file() and path.suffix in SUFFIXES)
    visited, errors = set(), []
    while pending:
        path, chain = pending.popleft()
        if path in visited:
            continue
        visited.add(path)
        if not path.is_relative_to(source):
            errors.append(' -> '.join(chain + ['outside source root']))
            continue
        text = without_comments(path.read_text())
        code = without_literals(text)
        errors.extend(' -> '.join(chain + [message]) for message in code_errors(text))
        for match in INCLUDE.finditer(text):
            if not code[match.start():match.end()].lstrip().startswith('#'):
                continue
            delimiter, name = match.groups()
            if include_error(name):
                errors.append(' -> '.join(chain + [name]))
                continue
            candidates = [(path.parent / name).resolve(), (source / name).resolve()]
            target = next((candidate for candidate in candidates if candidate.is_file()), None)
            if target is None:
                if delimiter == '"' and unresolved_is_error and name not in generated_headers:
                    errors.append(' -> '.join(chain + [name + ' (unresolved local header)']))
                continue
            if not target.is_relative_to(source):
                errors.append(' -> '.join(chain + [name + ' (outside source root)']))
                continue
            relative = target.relative_to(source).as_posix()
            if not allowed_path(relative):
                errors.append(' -> '.join(chain + ['src/' + relative]))
            elif target.suffix in SUFFIXES:
                pending.append((target, chain + ['src/' + relative]))
    return errors


UI_ROOT = 'interfaces/reader/ui'
UI_HELPERS = frozenset(('support/qt_font.h', 'support/qt_compat.h', 'support/strings.h',
                        'ui/input.h', 'ui/tap_zones.h', 'assets/assets.h', 'assets/bundled_fonts.h',
                        'assets/font_catalog.h'))
RETIRED = re.compile(r'reader_(?:action_\w+|list_action|icon_action|toolbar_action|full_toolbar_cell|side_buttons|icon|list_style)\Z')


def retired_property_errors(text):
    errors = []
    code = without_literals(text)
    for match in re.finditer(r'\b(?:setProperty|property)\s*\(', code):
        start = match.end()
        while start < len(text) and text[start].isspace():
            start += 1
        literal = LITERAL.match(text, start)
        if literal and literal[0].startswith('"') and RETIRED.fullmatch(literal[0][1:-1]):
            errors.append('retired visual property: ' + literal[0][1:-1])
    for literal in LITERAL.finditer(text):
        for match in re.finditer(r'\[(reader_\w+)\s*=', literal[0]):
            if RETIRED.fullmatch(match[1]):
                errors.append('retired visual stylesheet property: ' + match[1])
    return errors


PLAIN_BACKGROUNDS = frozenset(('transparent', 'palette(window)'))


def plain_background_errors(text):
    """Reject a colored background in a readerStyle without a selector.

    Panels and custom painters fill from their palette, and the theme sets every
    widget's palette, so such a background does not last. Selector rules such as
    QPushButton{...} are drawn by the style sheet itself and stay allowed.
    """
    errors = []
    code = without_literals(text)
    for match in re.finditer(r'\breaderStyle\s*\(', code):
        if re.search(r'\bvoid\s*$', code[:match.start()]):
            continue
        depth, end = 1, match.end()
        while depth and end < len(code):
            depth += {'(': 1, ')': -1}.get(code[end], 0)
            end += 1
        source = ''.join(literal[0][1:-1] for literal in LITERAL.finditer(text, match.end(), end - 1)
                         if literal[0].startswith('"'))
        if '{' in source:
            continue
        for value in re.findall(r'background(?:-color)?\s*:\s*([^;]+)', source):
            if value.strip() not in PLAIN_BACKGROUNDS:
                errors.append('plain style sheet background: ' + value.strip() + '; use Panel::setFill')
    return errors


def ui_errors(root):
    root = Path(root).resolve()
    errors = dependency_errors(root, (UI_ROOT,),
                             allowed_path=lambda name: name.startswith(UI_ROOT + '/') or name in UI_HELPERS,
                             include_error=private_qt_header,
                             generated_headers=('kobalt_strings.inc',))
    for path in sorted((root / 'src').rglob('*')):
        if path.is_file() and path.suffix in SUFFIXES:
            # Retired presentation properties must also stay out of UI callers.
            if not path.resolve().is_relative_to(root / 'src'):
                errors.append(path.relative_to(root).as_posix() + ' -> outside source root')
                continue
            text = without_comments(path.read_text())
            errors.extend(path.relative_to(root).as_posix() + ' -> ' + message
                          for message in retired_property_errors(text) + plain_background_errors(text))
    return errors


if __name__ == '__main__':
    import sys
    errors = ui_errors(Path(__file__).resolve().parents[1])
    print('\n'.join(errors) if errors else 'Reader UI architecture checks passed.')
    sys.exit(bool(errors))
