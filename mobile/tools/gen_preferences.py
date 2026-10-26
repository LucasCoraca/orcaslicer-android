#!/usr/bin/env python3
"""Reads PreferencesDialog::create_items() of upstream Preferences.cpp and writes its tabs, titles and items as JSON
inside a C++ include (the mobile core serves it to the app's Preferences dialog), so the dialog follows upstream.

usage: gen_preferences.py <Preferences.cpp> <out.inc>

Each item: {"kind": checkbox|combobox|input|spinctrl|decimal|button|language|region|loglevel|backup|auto_reslice|
darkmode|downloads|bambu_cloud|network_plugin_version|link_association, "var": the C++ variable, "label", "tooltip",
"key", ...} where translatable texts are kept as {"tr": text}. Code for Windows / macOS only is left out.
"""
import json, re, sys

src, out = sys.argv[1:3]
text = open(src, encoding='utf-8').read()


def strip_comments(s):
    out, k = [], 0
    while k < len(s):
        if s.startswith('//', k):
            k = s.find('\n', k)
            if k < 0: break
            continue
        if s.startswith('/*', k):
            k = s.find('*/', k) + 2
            continue
        if s[k] == '"':
            j = k + 1
            while s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            out.append(s[k:j + 1])
            k = j + 1
            continue
        if s[k] == "'" and k + 2 < len(s) and (s[k + 2] == "'" or s[k + 1] == '\\'):
            j = s.find("'", k + 2 if s[k + 1] == '\\' else k + 1)
            out.append(s[k:j + 1])
            k = j + 1
            continue
        out.append(s[k])
        k += 1
    return ''.join(out)


text = strip_comments(text)
start = text.index('void PreferencesDialog::create_items()')
body_start = text.index('{', start)


def matching(s, i, o='(', c=')'):
    depth = 0
    k = i
    while k < len(s):
        ch = s[k]
        if ch == '"':
            k += 1
            while s[k] != '"':
                k += 2 if s[k] == '\\' else 1
        elif ch == "'":
            k += 1
            while s[k] != "'":
                k += 2 if s[k] == '\\' else 1
        elif ch == o:
            depth += 1
        elif ch == c:
            depth -= 1
            if depth == 0:
                return k
        k += 1
    sys.exit('unbalanced')


body = text[body_start:matching(text, body_start, '{', '}') + 1]

# preprocessor: drop the Windows / macOS-only parts
lines, stack = [], []
for line in body.split('\n'):
    t = line.strip()
    if t.startswith('#if'):
        cond = t
        on = not re.search(r'_WIN32|__WXMSW__|__APPLE__|__WXOSX__|_MSW', cond) or re.search(r'!\s*defined\s*\(\s*(_WIN32|__APPLE__)', cond) or t.startswith('#ifndef')
        stack.append(bool(on))
        continue
    if t.startswith('#else'):
        stack[-1] = not stack[-1]
        continue
    if t.startswith('#elif'):
        stack[-1] = False
        continue
    if t.startswith('#endif'):
        stack.pop()
        continue
    if all(stack):
        lines.append(re.sub(r'(?<![:"])//.*$', '', line))
body = '\n'.join(lines)


def split_args(s):
    args, depth, cur, k = [], 0, '', 0
    while k < len(s):
        ch = s[k]
        if ch == '"':
            j = k + 1
            while s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            cur += s[k:j + 1]
            k = j + 1
            continue
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
        k += 1
    if cur.strip():
        args.append(cur.strip())
    return args


def unquote(lit):
    return bytes(lit[1:-1], 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


# the setting names defined as macros (AppConfig.hpp: #define SETTING_... "key")
macros = {}
for header in [src, src.replace('slic3r/GUI/Preferences.cpp', 'libslic3r/AppConfig.hpp'), src.replace('.cpp', '.hpp')]:
    try:
        for m in re.finditer(r'#define\s+(\w+)\s+"([^"]*)"', open(header, encoding='utf-8').read()):
            macros[m.group(1)] = m.group(2)
    except OSError:
        pass

STR = r'"(?:[^"\\]|\\.)*"'


def literals(s):
    """adjacent string literals ("a" "b") joined"""
    return ''.join(unquote(x) for x in re.findall(STR, s))


def split_plus(s):
    out, depth, cur, k = [], 0, '', 0
    while k < len(s):
        ch = s[k]
        if ch == '"':
            j = k + 1
            while s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            cur += s[k:j + 1]
            k = j + 1
            continue
        if ch in '([{': depth += 1
        elif ch in ')]}': depth -= 1
        if ch == '+' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
        k += 1
    out.append(cur.strip())
    return out


def value(arg):
    """_L("x") -> {"tr": x}; "x" -> x; a number; {a, b} -> a list; a macro -> its text; else the expression"""
    arg = arg.strip()
    m = re.fullmatch(r'_L\(\s*((?:' + STR + r'\s*)+)\)(\s*\+\s*dots)?', arg)
    if m:
        return {'tr': literals(m.group(1)), 'dots': bool(m.group(2))}
    if re.fullmatch(r'(?:' + STR + r'\s*)+', arg):
        return literals(arg)
    if arg.startswith('{') and arg.endswith('}'):
        return [value(a) for a in split_args(arg[1:-1])]
    if arg in macros:
        return macros[arg]
    parts = split_plus(arg)
    if len(parts) > 1:   # _L("Metric") + " (" + _L("mm") + ")"
        vals = [value(p) for p in parts]
        if all(isinstance(v, str) or (isinstance(v, dict) and 'tr' in v) for v in vals):
            return {'concat': vals}
    m = re.fullmatch(r'_L\(\s*("(?:[^"\\]|\\.)*")\s*\)(\s*\+\s*dots)?', arg)
    if m:
        return {'tr': unquote(m.group(1)), 'dots': bool(m.group(2))}
    m = re.fullmatch(r'L\(\s*("(?:[^"\\]|\\.)*")\s*\)', arg)
    if m:
        return {'tr': unquote(m.group(1))}
    if re.fullmatch(r'"(?:[^"\\]|\\.)*"', arg):
        return unquote(arg)
    if re.fullmatch(r'L"(?:[^"\\]|\\.)*"', arg):
        return unquote(arg[1:])
    if re.fullmatch(r'-?\d+(\.\d+)?', arg):
        return float(arg) if '.' in arg else int(arg)
    return {'expr': arg}


# the option lists: std::vector<wxString|std::string> NAME = { ... };
vectors = {}
for m in re.finditer(r'std::vector<\s*(wxString|std::string)\s*>\s*(\w+)\s*=\s*\{', body):
    end = matching(body, m.end() - 1, '{', '}')
    vectors[m.group(2)] = [value(a) for a in split_args(body[m.end():end])]

tabs = []
pos = 0
pattern = re.compile(r'AppendItem\(\s*_L\(\s*"([^"]*)"\s*\)\s*\)|create_item_title\(\s*_L\(\s*"([^"]*)"\s*\)\s*\)|(?:auto\s+)?(\w+)\s*=\s*create_item_(\w+)\s*\(')
for m in pattern.finditer(body):
    if m.group(1) is not None:
        tabs.append({'tab': {'tr': m.group(1)}, 'items': []})
        continue
    if not tabs:
        continue
    if m.group(2) is not None:
        tabs[-1]['items'].append({'kind': 'title', 'label': {'tr': m.group(2)}})
        continue
    var, kind = m.group(3), m.group(4)
    args = [value(a) for a in split_args(body[m.end():matching(body, m.end() - 1)])]
    item = {'kind': kind, 'var': var}
    strs = [a for a in args if isinstance(a, str)]
    if kind == 'checkbox':
        keys = [a for a in args[2:] if isinstance(a, str)]
        item.update(label=args[0], tooltip=args[1] if len(args) > 1 else '', key=keys[0] if keys else '')
        rest = [a for a in args[2:] if isinstance(a, dict) and 'tr' in a]
        if rest: item['secondary'] = rest[0]
    elif kind == 'combobox':
        item.update(label=args[0], tooltip=args[1], key=args[2])
        opts = args[3]
        item['options'] = opts if isinstance(opts, list) else vectors.get(opts['expr'], []) if isinstance(opts, dict) and 'expr' in opts else []
        if len(args) > 4 and isinstance(args[4], list):
            item['values'] = args[4]
        elif len(args) > 4 and isinstance(args[4], dict) and args[4].get('expr') in vectors:
            item['values'] = vectors[args[4]['expr']]
    elif kind == 'input':
        item.update(label=args[0], side=args[1], tooltip=args[2], key=args[3])
    elif kind == 'spinctrl':
        item.update(label=args[0], side=args[2], tooltip=args[3], key=args[4], min=args[5], max=args[6])
    elif kind == 'decimal_input':
        item.update(kind='decimal', label=args[0], side=args[1], tooltip=args[2], key=args[3], min=args[4], max=args[5], decimals=args[6])
    elif kind == 'button':
        item.update(label=args[0], button=args[1], tooltip=args[3] if len(args) > 3 else '')
        # what the button does when it only clears an app config value
        lam = body[m.end():matching(body, m.end() - 1)]
        a = re.search(r'app_config->set\(\s*"(\w+)"\s*,\s*""\s*\)', lam)
        e = re.search(r'app_config->erase\(\s*"(\w+)"\s*,\s*"(\w+)"\s*\)', lam)
        if a: item['clear'] = a.group(1)
        elif e: item['erase'] = [e.group(1), e.group(2)]
    elif kind == 'loglevel_combobox':
        item.update(kind='loglevel', label=args[0], tooltip=args[1], options=vectors.get(args[2].get('expr', ''), []) if isinstance(args[2], dict) else [])
    elif kind == 'link_association':
        item.update(url_prefix=args[0], website=args[1])
    elif kind.endswith('_combobox'):
        item.update(kind=kind[:-len('_combobox')], label=args[0], tooltip=args[1] if len(args) > 1 else '')
    else:
        item.update(label=args[0], tooltip=args[1] if len(args) > 1 else '')
        if len(args) > 2 and isinstance(args[2], str): item['key'] = args[2]
    tabs[-1]['items'].append(item)

data = json.dumps(tabs, ensure_ascii=False)
with open(out, 'w', encoding='utf-8') as f:
    f.write('// generated by mobile/tools/gen_preferences.py from slic3r/GUI/Preferences.cpp — do not edit\n')
    f.write('static const char *k_preferences_layout = R"PREFS(' + data + ')PREFS";\n')
