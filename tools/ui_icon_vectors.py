#!/usr/bin/env python3
"""Compile the bundled Phosphor SVG subset to Qt paths without rasterizing it."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import xml.etree.ElementTree as ET

TOKEN = re.compile(r'[MmLlHhVvCcSsAaZz]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?')
COUNTS = {'m': 2, 'l': 2, 'h': 1, 'v': 1, 'c': 6, 's': 4, 'a': 7, 'z': 0}


def number(value):
    value = float(value)
    if not math.isfinite(value):
        raise ValueError('non-finite coordinate')
    return format(value, '.12g')


def arc_cubics(start, rx, ry, rotation, large, sweep, end):
    """Convert an SVG arc to cubic segments of at most a quarter turn each."""
    if large not in (0, 1) or sweep not in (0, 1):
        raise ValueError('invalid arc flag')
    if start == end:
        return []
    rx, ry = abs(rx), abs(ry)
    if rx == 0 or ry == 0:
        return [(*start, *end, *end)]
    phi = math.radians(rotation % 360)
    cos, sin = math.cos(phi), math.sin(phi)
    # SVG implementation notes, F.6.5: endpoints to center parameters.
    dx, dy = (start[0] - end[0]) / 2, (start[1] - end[1]) / 2
    x1, y1 = cos * dx + sin * dy, -sin * dx + cos * dy
    scale = (x1 / rx) ** 2 + (y1 / ry) ** 2
    if scale > 1:
        rx, ry = rx * math.sqrt(scale), ry * math.sqrt(scale)
    numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1
    factor = math.sqrt(max(0, numerator / denominator))
    if large == sweep:
        factor = -factor
    cx1, cy1 = factor * rx * y1 / ry, -factor * ry * x1 / rx
    cx = cos * cx1 - sin * cy1 + (start[0] + end[0]) / 2
    cy = sin * cx1 + cos * cy1 + (start[1] + end[1]) / 2

    def angle(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    theta = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi
    segments = max(1, math.ceil(abs(delta) / (math.pi / 2) - 1e-9))
    step = delta / segments
    k = 4 / 3 * math.tan(step / 4)

    def point(t):
        x, y = rx * math.cos(t), ry * math.sin(t)
        return cos * x - sin * y + cx, sin * x + cos * y + cy

    def tangent(t):
        x, y = -rx * math.sin(t), ry * math.cos(t)
        return cos * x - sin * y, sin * x + cos * y

    result = []
    for index in range(segments):
        a, b = theta + index * step, theta + (index + 1) * step
        p0, p1 = point(a), point(b)
        if index == segments - 1:
            p1 = tuple(end)
        t0, t1 = tangent(a), tangent(b)
        result.append((p0[0] + k * t0[0], p0[1] + k * t0[1], p1[0] - k * t1[0], p1[1] - k * t1[1], *p1))
    return result


def path_commands(data):
    tokens = TOKEN.findall(data)
    if TOKEN.sub('', data).strip(' ,\t\r\n'):
        raise ValueError('unsupported path command')
    if not tokens or tokens[0] not in ('M', 'm'):
        raise ValueError('path must begin with a move')
    result, position, start, command, index = [], [0., 0.], [0., 0.], None, 0
    previous = None  # The last command letter and its second control point, for S and T.
    while index < len(tokens):
        if tokens[index].isalpha():
            command = tokens[index]
            index += 1
        elif command is None or command in 'Zz':
            raise ValueError('missing path command')
        kind = command.lower()
        count = COUNTS[kind]
        values = [float(value) for value in tokens[index:index + count]]
        if len(values) != count or any(token.isalpha() for token in tokens[index:index + count]) \
                or not all(math.isfinite(value) for value in values):
            raise ValueError('invalid path coordinates')
        index += count
        x, y = position
        relative = command.islower()

        def absolute(px, py):
            return [px + x, py + y] if relative else [px, py]

        control = None
        if kind == 'z':
            result.append('path.closeSubpath();')
            end = list(start)
        elif kind in 'ml':
            end = absolute(*values)
            result.append('path.%s(%s, %s);' % ('moveTo' if kind == 'm' else 'lineTo', *map(number, end)))
            if kind == 'm':
                start = list(end)
                command = 'l' if relative else 'L'
        elif kind in 'hv':
            end = [x, y]
            axis = 0 if kind == 'h' else 1
            end[axis] = values[0] + (position[axis] if relative else 0)
            result.append('path.lineTo(%s, %s);' % tuple(map(number, end)))
        elif kind in 'cs':
            if kind == 'c':
                first, control, end = absolute(*values[0:2]), absolute(*values[2:4]), absolute(*values[4:6])
            else:
                reflect = previous and previous[0] in 'cs'
                first = [2 * x - previous[1][0], 2 * y - previous[1][1]] if reflect else [x, y]
                control, end = absolute(*values[0:2]), absolute(*values[2:4])
            result.append('path.cubicTo(%s);' % ', '.join(map(number, first + control + end)))
        else:
            rx, ry, rotation, large, sweep = values[:5]
            if large not in (0, 1) or sweep not in (0, 1):
                raise ValueError('invalid arc flag')
            end = absolute(*values[5:7])
            for cubic in arc_cubics(tuple(position), rx, ry, rotation, int(large), int(sweep), tuple(end)):
                result.append('path.cubicTo(%s);' % ', '.join(map(number, cubic)))
        previous = (kind, control)
        position = end
    return result


def geometry(source, entry):
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != entry['sha256']:
        raise ValueError('SVG checksum does not match manifest: ' + source.name)
    root = ET.fromstring(data)
    expected = {'viewBox': '0 0 256 256', 'fill': 'currentColor'}
    if root.tag != '{http://www.w3.org/2000/svg}svg' or root.attrib != expected:
        raise ValueError('unsupported SVG root: ' + source.name)
    commands = []
    for element in root:
        tag = element.tag.removeprefix('{http://www.w3.org/2000/svg}')
        if tag != 'path' or set(element.attrib) != {'d'}:
            raise ValueError('unsupported SVG element: ' + tag)
        if len(element) or (element.text or '').strip():
            raise ValueError('unexpected SVG element contents')
        commands += path_commands(element.attrib['d'])
    if not commands:
        raise ValueError('empty SVG geometry')
    return commands


def generate(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    lines = ['// Generated from the checked Phosphor SVGs by tools/ui_icon_vectors.py.',
             '// Each shape is a filled outline on a 256 unit canvas.',
             'static bool phosphorGeometry(const QString &name, QPainterPath &path) {']
    seen = set()
    for entry in manifest['files']:
        filename = entry['file']
        if Path(filename).name != filename or not filename.endswith('.svg') or filename in seen:
            raise ValueError('invalid or duplicate SVG filename')
        seen.add(filename)
        commands = geometry(directory / filename, entry)
        lines.append('    if (name == QStringLiteral(%s)) {' % json.dumps(filename[:-4]))
        # SVG fills with the nonzero rule, which Qt calls winding.
        lines.append('        path.setFillRule(Qt::WindingFill);')
        lines += ['        ' + command for command in commands]
        lines += ['        return true;', '    }']
    return '\n'.join(lines + ['    return false;', '}', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = generate(args.directory)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(data)


if __name__ == '__main__':
    main()
