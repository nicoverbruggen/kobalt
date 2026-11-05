// Writes src/ui/controls/phosphor_geometry.cpp from the Phosphor SVGs in
// assets/icons/phosphor. Each icon becomes a QPainterPath on the SVG's 256
// unit canvas. Arcs become cubic curves of at most 90 degrees each, so the
// file needs no SVG parser at runtime and draws the same on every Qt version.
//
// Usage: kobalt-icon-geometry <svg folder> <output file>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// Rounds to 1/10000 of a canvas unit, far below a pixel at any icon size.
// Compilers, libm versions and CPUs can differ in the last bits of sin, cos
// and fused multiply-adds. The coarse grid hides those differences in practice.
std::string number(double value)
{
    char text[64];
    std::snprintf(text, sizeof text, "%.4f", value);
    std::string result = text;
    result.erase(result.find_last_not_of('0') + 1);
    if (result.back() == '.')
        result.pop_back();
    return result == "-0" ? "0" : result;
}

class Writer
{
public:
    void line(const std::string &call, std::initializer_list<double> values)
    {
        out << "    path." << call << '(';
        bool first = true;
        for (const double value : values) {
            out << (first ? "" : ", ") << number(value);
            first = false;
        }
        out << ");\n";
    }

    void close() { out << "    path.closeSubpath();\n"; }

    std::ostringstream out;
};

// Reads the path data grammar of SVG 1.1: commands, then numbers that may
// share separators, signs and decimal points without spaces.
class Tokens
{
public:
    explicit Tokens(std::string text) : text(std::move(text)) { }

    bool command(char *result)
    {
        skip();
        if (at < text.size() && std::isalpha(static_cast<unsigned char>(text[at]))) {
            *result = text[at++];
            return true;
        }
        return false;
    }

    bool hasNumber()
    {
        skip();
        return at < text.size()
                && (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '-'
                    || text[at] == '+' || text[at] == '.');
    }

    double number()
    {
        skip();
        const std::size_t start = at;
        if (at < text.size() && (text[at] == '-' || text[at] == '+'))
            ++at;
        bool dot = false;
        while (at < text.size()
               && (std::isdigit(static_cast<unsigned char>(text[at]))
                   || (!dot && text[at] == '.'))) {
            dot = dot || text[at] == '.';
            ++at;
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < text.size() && (text[at] == '-' || text[at] == '+'))
                ++at;
            while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])))
                ++at;
        }
        if (start == at)
            throw std::runtime_error("expected a number at offset " + std::to_string(at));
        return std::stod(text.substr(start, at - start));
    }

    // Arc flags are single digits and may run into the next number.
    bool flag()
    {
        skip();
        if (at >= text.size() || (text[at] != '0' && text[at] != '1'))
            throw std::runtime_error("expected an arc flag at offset " + std::to_string(at));
        return text[at++] == '1';
    }

    bool done()
    {
        skip();
        return at >= text.size();
    }

private:
    void skip()
    {
        while (at < text.size()
               && (std::isspace(static_cast<unsigned char>(text[at])) || text[at] == ','))
            ++at;
    }

    std::string text;
    std::size_t at = 0;
};

constexpr double pi = 3.14159265358979323846;

// SVG 1.1 implementation notes F.6.5 and F.6.6: endpoint to centre form.
void arc(Writer &w, double x1, double y1, double rx, double ry, double degrees, bool large,
         bool sweep, double x2, double y2)
{
    if (x1 == x2 && y1 == y2)
        return;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0 || ry == 0) {
        w.line("lineTo", { x2, y2 });
        return;
    }
    const double phi = degrees * pi / 180, c = std::cos(phi), s = std::sin(phi);
    const double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    const double px = c * dx + s * dy, py = -s * dx + c * dy;
    const double lambda = px * px / (rx * rx) + py * py / (ry * ry);
    if (lambda > 1) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const double numerator = rx * rx * ry * ry - rx * rx * py * py - ry * ry * px * px;
    const double denominator = rx * rx * py * py + ry * ry * px * px;
    const double coefficient =
            (large == sweep ? -1 : 1) * std::sqrt(std::max(0.0, numerator / denominator));
    const double cpx = coefficient * rx * py / ry, cpy = -coefficient * ry * px / rx;
    const double cx = c * cpx - s * cpy + (x1 + x2) / 2, cy = s * cpx + c * cpy + (y1 + y2) / 2;
    const auto angle = [](double ux, double uy, double vx, double vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const double start = angle(1, 0, (px - cpx) / rx, (py - cpy) / ry);
    double delta = angle((px - cpx) / rx, (py - cpy) / ry, (-px - cpx) / rx, (-py - cpy) / ry);
    if (!sweep && delta > 0)
        delta -= 2 * pi;
    if (sweep && delta < 0)
        delta += 2 * pi;
    const int segments = std::max(1, int(std::ceil(std::fabs(delta) / (pi / 2) - 1e-9)));
    const double step = delta / segments, t = 4.0 / 3.0 * std::tan(step / 4);
    const auto point = [&](double ex, double ey) {
        const double x = rx * ex, y = ry * ey;
        return std::make_pair(c * x - s * y + cx, s * x + c * y + cy);
    };
    for (int i = 0; i < segments; ++i) {
        const double a = start + i * step, b = a + step;
        const auto p1 = point(std::cos(a) - t * std::sin(a), std::sin(a) + t * std::cos(a));
        const auto p2 = point(std::cos(b) + t * std::sin(b), std::sin(b) - t * std::cos(b));
        const auto p3 =
                i + 1 == segments ? std::make_pair(x2, y2) : point(std::cos(b), std::sin(b));
        w.line("cubicTo", { p1.first, p1.second, p2.first, p2.second, p3.first, p3.second });
    }
}

std::string geometry(const std::string &data)
{
    Writer w;
    Tokens tokens(data);
    double x = 0, y = 0, startX = 0, startY = 0, controlX = 0, controlY = 0;
    char command = 0, previous = 0;
    while (!tokens.done()) {
        if (!tokens.command(&command) && !command)
            throw std::runtime_error("path data must start with a command");
        const bool relative = std::islower(static_cast<unsigned char>(command));
        const char upper = char(std::toupper(static_cast<unsigned char>(command)));
        const double ox = relative ? x : 0, oy = relative ? y : 0;
        if (upper == 'Z') {
            w.close();
            x = startX;
            y = startY;
            previous = upper;
            command = 0;
            continue;
        }
        switch (upper) {
        case 'M':
            x = ox + tokens.number();
            y = oy + tokens.number();
            startX = x;
            startY = y;
            w.line("moveTo", { x, y });
            // Further pairs after a move are line segments.
            command = relative ? 'l' : 'L';
            break;
        case 'L':
            x = ox + tokens.number();
            y = oy + tokens.number();
            w.line("lineTo", { x, y });
            break;
        case 'H':
            x = ox + tokens.number();
            w.line("lineTo", { x, y });
            break;
        case 'V':
            y = oy + tokens.number();
            w.line("lineTo", { x, y });
            break;
        case 'C':
        case 'S': {
            double x1;
            double y1;
            if (upper == 'C') {
                x1 = ox + tokens.number();
                y1 = oy + tokens.number();
            } else {
                const bool reflect = previous == 'C' || previous == 'S';
                x1 = reflect ? 2 * x - controlX : x;
                y1 = reflect ? 2 * y - controlY : y;
            }
            controlX = ox + tokens.number();
            controlY = oy + tokens.number();
            x = ox + tokens.number();
            y = oy + tokens.number();
            w.line("cubicTo", { x1, y1, controlX, controlY, x, y });
            break;
        }
        case 'Q':
            controlX = ox + tokens.number();
            controlY = oy + tokens.number();
            x = ox + tokens.number();
            y = oy + tokens.number();
            w.line("quadTo", { controlX, controlY, x, y });
            break;
        case 'A': {
            const double rx = tokens.number(), ry = tokens.number(), rotation = tokens.number();
            const bool large = tokens.flag(), sweep = tokens.flag();
            const double ex = ox + tokens.number(), ey = oy + tokens.number();
            arc(w, x, y, rx, ry, rotation, large, sweep, ex, ey);
            x = ex;
            y = ey;
            break;
        }
        default:
            throw std::runtime_error(std::string("unsupported path command ") + command);
        }
        previous = upper;
        if (!tokens.hasNumber())
            command = 0;
    }
    return w.out.str();
}

std::string read(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + file.string());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Each Phosphor icon is one filled path with the default nonzero fill rule.
std::string pathData(const std::string &svg, const std::string &name)
{
    const auto element = svg.find("<path");
    if (element == std::string::npos || svg.find("<path", element + 1) != std::string::npos)
        throw std::runtime_error(name + ": expected exactly one path");
    if (svg.find("fill-rule") != std::string::npos || svg.find("transform") != std::string::npos)
        throw std::runtime_error(name + ": fill rules and transforms are not supported");
    const auto start = svg.find(" d=\"", element);
    if (start == std::string::npos)
        throw std::runtime_error(name + ": path has no data");
    const auto end = svg.find('"', start + 4);
    return svg.substr(start + 4, end - start - 4);
}

// A prefix keeps names such as "export" clear of C++ keywords.
std::string identifier(const std::string &name)
{
    std::string result = "icon_" + name;
    std::replace(result.begin(), result.end(), '-', '_');
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <svg folder> <output file>\n", argv[0]);
        return 2;
    }
    try {
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(argv[1]))
            if (entry.path().extension() == ".svg")
                files.push_back(entry.path());
        // Sort by name, the order the lookup searches. Whole file names would
        // put "eye-slash.svg" before "eye.svg".
        std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
            return a.stem().string() < b.stem().string();
        });
        std::ostringstream out;
        out << "// Generated by platforms/desktop/icon-geometry.cpp from assets/icons/phosphor.\n"
               "// Do not edit. Build the update-icon-geometry target after changing an SVG.\n"
               "#include \"phosphor_geometry.h\"\n"
               "#include <QByteArray>\n"
               "#include <QPainterPath>\n"
               "#include <algorithm>\n"
               "#include <cstring>\n"
               "#include <iterator>\n\n"
               "namespace kobalt::ui {\n"
               "namespace {\n";
        std::vector<std::string> names;
        for (const auto &file : files) {
            const auto name = file.stem().string();
            names.push_back(name);
            out << "void " << identifier(name) << "(QPainterPath &path) {\n"
                << "    path.setFillRule(Qt::WindingFill);\n"
                << geometry(pathData(read(file), name)) << "}\n";
        }
        out << "struct Icon {\n"
               "    const char *name;\n"
               "    void (*build)(QPainterPath &);\n"
               "};\n"
               "// Sorted by name for a binary search.\n"
               "constexpr Icon icons[] = {\n";
        for (const auto &name : names)
            out << "    {\"" << name << "\", " << identifier(name) << "},\n";
        out << "};\n"
               "}\n\n"
               "bool phosphorGeometry(const QString &name, QPainterPath &path) {\n"
               "    const auto key = name.toLatin1();\n"
               "    const auto end = std::end(icons);\n"
               "    const auto found = std::lower_bound(std::begin(icons), end, key, [](const Icon &icon, const QByteArray &wanted) {\n"
               "        return std::strcmp(icon.name, wanted.constData()) < 0;\n"
               "    });\n"
               "    if (found == end || key != found->name) return false;\n"
               "    found->build(path);\n"
               "    return true;\n"
               "}\n"
               "}\n";
        std::ofstream file(argv[2], std::ios::binary);
        file << out.str();
        file.close();
        if (!file)
            throw std::runtime_error(std::string("cannot write ") + argv[2]);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "icon-geometry: %s\n", error.what());
        return 1;
    }
    return 0;
}
