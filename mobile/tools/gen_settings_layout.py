#!/usr/bin/env python3
"""Generate the settings page layout of the mobile app from OrcaSlicer's desktop Tab.cpp.

The desktop settings tabs are built in code (TabPrint::build, TabFilament::build, TabPrinter::build_fff, ...):
pages, option groups, option lines and multi-option lines. This script interprets those build functions closely
enough to recover that structure, so the mobile settings follow upstream as it changes: run it against a new
OrcaSlicer checkout and the pages, groups and option order come out as on the desktop.

Statements it does not understand but that look like layout code are reported (stderr); fix the parser or add an
entry to layout_overrides.json when an upstream change introduces a new construct.

usage: gen_settings_layout.py <orca-src> <out.json> [overrides.json]
"""
import json
import re
import sys

STR = r'"(?:[^"\\]|\\.)*"'


def strip_comments_and_dead_code(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1]); i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1]); i = j + 1
        elif src.startswith('//', i):
            j = src.find('\n', i); i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2); i = n if j < 0 else j + 2
        else:
            out.append(c); i += 1
    text = ''.join(out)
    # preprocessor: "#if 0" drops its branch, "#if 1" its #else; other conditions keep the first branch
    lines, stack, keep = [], [], True
    for line in text.split('\n'):
        s = line.strip()
        if s.startswith('#if'):
            cond = s != '#if 0'
            stack.append((keep, cond))
            keep = keep and cond
        elif s.startswith('#el') and stack:
            outer, cond = stack[-1]
            keep = outer and not cond
            stack[-1] = (outer, True)
        elif s.startswith('#endif') and stack:
            keep = stack.pop()[0]
        elif s.startswith('#'):
            pass
        elif keep:
            lines.append(line)
    return '\n'.join(lines)


def function_body(src, qualified_name):
    m = re.search(r'\n[\w:<>\s\*&]*\b' + re.escape(qualified_name) + r'\s*\([^)]*\)\s*(const\s*)?\{', src)
    if not m:
        raise SystemExit('function not found: ' + qualified_name)
    start = m.end() - 1
    return src[start + 1:matching(src, start)]


def matching(s, i):
    """index of the bracket closing the one at s[i] (skipping strings)"""
    open_c = s[i]; close_c = {'{': '}', '(': ')', '[': ']'}[open_c]
    depth, j, n = 0, i, len(s)
    while j < n:
        c = s[j]
        if c == '"':
            j += 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
        elif c == "'":
            j += 1
            while j < n and s[j] != "'":
                j += 2 if s[j] == '\\' else 1
        elif c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return j
        j += 1
    raise SystemExit('unbalanced')


def remove_lambdas(s):
    """replace lambda bodies by {} : their code is behaviour, not layout"""
    out, i = [], 0
    lam = re.compile(r'\[[^\[\]]*\]\s*(\([^()]*(?:\([^()]*\)[^()]*)*\))?\s*(mutable\s*)?(->\s*[\w:<>\*\s]+?)?\s*\{')
    while True:
        m = lam.search(s, i)
        if not m:
            out.append(s[i:]); break
        # a lambda starts with a capture list after "(", ",", "=" or "return"
        before = s[:m.start()].rstrip()
        if not before or before[-1] not in '(,=' and not before.endswith('return'):
            out.append(s[i:m.end()]); i = m.end(); continue
        brace = m.end() - 1
        out.append(s[i:m.start()] + 'LAMBDA'); i = matching(s, brace) + 1
    return ''.join(out)


def parse_block(s):
    """-> list of statements; a statement is a str, or (header, [statements]) for if/for/else/plain blocks"""
    items, buf, i, n = [], '', 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            buf += s[i:j + 1]; i = j + 1; continue
        if c == '(' or c == '[':
            j = matching(s, i); buf += s[i:j + 1]; i = j + 1; continue
        if c == '{':
            head = buf.strip()
            is_block = head == '' or re.match(r'(if|for|while|switch|else|do|try)\b', head) is not None
            if is_block:
                j = matching(s, i)
                items.append((head, parse_block(s[i + 1:j])))
                buf = ''; i = j + 1; continue
            j = matching(s, i); buf += s[i:j + 1]; i = j + 1; continue
        if c == ';':
            if buf.strip():
                items.append(buf.strip())
            buf = ''; i += 1; continue
        buf += c; i += 1
    if buf.strip():
        items.append(buf.strip())
    return items


def unq(lit):
    return bytes(lit[1:-1], 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


def tr_string(expr):
    """L("x") / _L("x") / "x" / L"x" / _(L("x")) -> x ; None if not a literal"""
    e = expr.strip()
    m = re.fullmatch(r'(?:_\(\s*)?(?:_?L|_u8L|_L)?\s*\(?\s*L?(' + STR + r')\s*\)?\s*\)?', e)
    if m:
        return unq(m.group(1))
    m = re.fullmatch(r'L?(' + STR + r')', e)
    return unq(m.group(1)) if m else None


def split_args(a):
    args, buf, depth, i = [], '', 0, 0
    while i < len(a):
        c = a[i]
        if c == '"':
            j = i + 1
            while j < len(a) and a[j] != '"':
                j += 2 if a[j] == '\\' else 1
            buf += a[i:j + 1]; i = j + 1; continue
        if c in '([{<' and not (c == '<' and depth == 0 and not re.search(r'\w$', buf)):
            depth += 1
        elif c in ')]}>' and depth > 0:
            depth -= 1
        if c == ',' and depth == 0:
            args.append(buf.strip()); buf = ''
        else:
            buf += c
        i += 1
    if buf.strip():
        args.append(buf.strip())
    return args


class Interp:
    """walks the statements of a build function and records pages / groups / lines"""

    def __init__(self, funcs, tab, warnings):
        self.funcs, self.tab, self.warn = funcs, tab, warnings
        self.pages = []
        self.page = None
        self.groups = {}      # var -> group dict
        self.options = {}     # var -> option dict (key, idx, overrides)
        self.lines = {}       # var -> line dict
        self.vectors = {}     # var -> list of strings
        self.loopvars = {}    # var -> current value
        self.defs = {}        # ConfigOptionDef var -> dict

    def ev(self, expr):
        """evaluate a string expression: literals, loop variables, + concatenation"""
        parts = [p.strip() for p in re.split(r'\+(?=(?:[^"]*"[^"]*")*[^"]*$)', expr)]
        out = ''
        for p in parts:
            t = tr_string(p)
            if t is not None:
                out += t
            elif p in self.loopvars:
                out += self.loopvars[p]
            elif re.fullmatch(r'\w+\.key', p) and p.split('.')[0] in self.loopvars:
                out += self.loopvars[p.split('.')[0]]
            else:
                return None
        return out

    def new_page(self, title, icon):
        self.page = {'title': title, 'icon': icon, 'groups': []}
        self.pages.append(self.page)
        return self.page

    def opt_from_get(self, call):
        m = re.fullmatch(r'(\w+)(?:\.get\(\))?\s*->\s*get_option\s*\((.*)\)', call.strip(), re.S)
        if not m:
            return None
        args = split_args(m.group(2))
        key = self.ev(args[0])
        if key is None:
            return None
        o = {'key': key}
        if len(args) > 1 and args[1].strip() not in ('0', '-1'):
            o['idx'] = args[1].strip()
        return o

    def run(self, items):
        for it in items:
            if isinstance(it, tuple):
                self.block(*it)
            else:
                self.stmt(re.sub(r'\s+', ' ', it))

    def block(self, head, body):
        m = re.match(r'for \(\s*(?:const\s+)?[\w:<>&\s]+?[&\s](\w+)\s*:\s*\{(.*)\}\s*\)$', head)
        if m:   # for (const std::string k : { "a", "b" })
            for v in [unq(x.strip()) for x in split_args(m.group(2)) if x.strip()]:
                self.loopvars[m.group(1)] = v
                self.run(body)
            self.loopvars.pop(m.group(1), None)
            return
        m = re.match(r'for \(\s*(?:const\s+)?[\w:<>&\s]+?[&\s](\w+)\s*:\s*(\w+)\(?\)?\s*\)$', head)
        if m:
            var, coll = m.group(1), m.group(2)
            if coll in self.vectors:
                for v in self.vectors[coll]:
                    self.loopvars[var] = v
                    self.run(body)
                self.loopvars.pop(var, None)
                return
            if coll in RUNTIME_LISTS:   # lists the core provides at run time (libslic3r functions)
                self.loopvars[var] = '{' + coll + '}'
                g = self.cur_group()
                n0 = len(g['lines']) if g else 0
                self.run(body)
                if g:
                    added = g['lines'][n0:]
                    del g['lines'][n0:]
                    g['lines'].append({'expand': coll, 'template': added})
                self.loopvars.pop(var, None)
                return
            self.warn.append(f'{self.tab}: unknown loop collection {coll}')
            return
        if head.startswith('if') and re.search(r'getAgent\(\)|registered_printer_agents', head):
            return   # network agent picker: desktop-only
        self.run(body)

    def cur_group(self):
        if not self.page or not self.page['groups']:
            return None
        return self.page['groups'][-1] if self.last_group is None else self.last_group

    last_group = None

    def add_line(self, gvar, line):
        g = self.groups.get(gvar)
        if g is None:
            self.warn.append(f'{self.tab}: line for unknown group {gvar}: {line}')
            return
        g['lines'].append(line)

    def stmt(self, s):
        if re.match(r'(for|if) \(', s):   # single-statement for / if
            j = matching(s, s.index('('))
            self.block(s[:j + 1], [s[j + 1:].strip()])
            return
        # pages
        m = re.match(r'(?:auto\s+|PageShp\s+)?(\w+) = add_options_page\((.*)\)$', s)
        if m:
            args = split_args(m.group(2))
            self.new_page(tr_string(args[0]), tr_string(args[1]) if len(args) > 1 else '')
            return
        m = re.match(r'(?:auto\s+)?(\w+) = build_kinematics_page\(\)$', s)
        if m:
            sub = Interp(self.funcs, self.tab, self.warn)
            sub.run(self.funcs['TabPrinter::build_kinematics_page'])
            for p in sub.pages:
                p['marlin_only'] = True
            self.pages.extend(sub.pages)
            return
        if re.match(r'(build_unregular_pages|add_filament_overrides_page)\(', s):
            name = {'build_unregular_pages': 'TabPrinter::build_unregular_pages',
                    'add_filament_overrides_page': 'TabFilament::add_filament_overrides_page'}[s.split('(')[0]]
            sub = Interp(self.funcs, self.tab, self.warn)
            sub.run(self.funcs[name])
            self.pages.extend(sub.pages)
            return
        # groups
        m = re.match(r'(?:auto\s+|ConfigOptionsGroupShp\s+)?(\w+) = (\w+)->new_optgroup\((.*)\)$', s)
        if m:
            args = split_args(m.group(3))
            title = tr_string(args[0]) if args else ''
            g = {'title': title or '', 'icon': tr_string(args[1]) if len(args) > 1 and tr_string(args[1]) is not None else '', 'lines': []}
            if len(args) > 2 and args[2].strip() == '0':
                g['no_label_column'] = True
            self.page['groups'].append(g)
            self.groups[m.group(1)] = g
            self.last_group = g
            return
        m = re.match(r'(\w+)->hide_labels\(\)$', s)
        if m:
            self.groups[m.group(1)]['hide_labels'] = True
            return
        m = re.match(r'(\w+)->append_separator\(\)$', s)
        if m:
            self.add_line(m.group(1), {'separator': True})
            return
        # single option lines
        m = re.match(r'(\w+)->append_single_option_line\((.*)\)$', s)
        if m:
            args = split_args(m.group(2))
            key = self.ev(args[0])
            if key is not None:
                line = {'option': key}
                if len(args) > 2 and args[2].strip() not in ('0', '-1'):
                    line['idx'] = args[2].strip()
            elif args[0] in self.options:
                line = dict(self.options[args[0]])
                line['option'] = line.pop('key')
            else:
                self.warn.append(f'{self.tab}: single option line with unknown option: {s}')
                return
            if len(args) > 1 and self.ev(args[1]) is not None and not self.ev(args[1]).startswith('param_'):
                line['wiki'] = self.ev(args[1])
            self.add_line(m.group(1), line)
            return
        m = re.match(r'append_retraction_option\((\w+), (\w+), \w+\)$', s)
        if m:   # TabFilament::add_filament_overrides_page: a filament override (checkbox + value, nil = printer value)
            self.add_line(m.group(1), {'option': self.ev(m.group(2)), 'override': True})
            return
        m = re.match(r'append_option_line\((\w+), (.*)\)$', s)
        if m:
            args = split_args(m.group(2))
            key = self.ev(args[0])
            if key is None:
                self.warn.append(f'{self.tab}: append_option_line key: {s}')
                return
            self.add_line(m.group(1), {'option': key, 'all_indices': True})
            return
        # options
        m = re.match(r'(?:Option\s+)?(\w+) = (\w+(?:\.get\(\))?\s*->\s*get_option\(.*\))$', s)
        if m:
            o = self.opt_from_get(m.group(2))
            if o:
                self.options[m.group(1)] = o
                return
        m = re.match(r'(?:auto\s+|Option\s+)?(\w+) = Option\((\w+), (.*)\)$', s) or re.match(r'Option (\w+)\((\w+), (.*)\)$', s)
        if m:
            key = self.ev(m.group(3))
            o = {'key': key, 'custom_def': dict(self.defs.get(m.group(2), {}))}
            self.options[m.group(1)] = o
            return
        m = re.match(r'(\w+)\.opt\.(\w+) = (.*)$', s)
        if m and m.group(1) in self.options:
            field, val = m.group(2), m.group(3).strip()
            o = self.options[m.group(1)]
            if field in ('full_width', 'is_code', 'multiline', 'readonly'):
                o[field] = val == 'true'
            elif field in ('height', 'width'):
                v = val.split('//')[0].strip()
                o[field] = int(v) if v.lstrip('-').isdigit() else {'gcode_field_height': 15, 'notes_field_height': 25}.get(v, 15)
            elif field in ('label', 'tooltip', 'sidetext'):
                t = tr_string(val)
                if t is not None:
                    o[field] = t
            elif field == 'gui_type':
                o['gui_type'] = val.split('::')[-1]
            return
        # ConfigOptionDef built in place (synthetic options such as extruders_count)
        m = re.match(r'ConfigOptionDef (\w+)$', s)
        if m:
            self.defs[m.group(1)] = {}
            return
        m = re.match(r'(\w+)\.(type|label|tooltip|min|max|mode|sidetext) = (.*)$', s) or re.match(r'(\w+)\.(set_default_value)\((.*)\)$', s)
        if m and m.group(1) in self.defs:
            d, field, val = self.defs[m.group(1)], m.group(2), m.group(3)
            if field in ('label', 'tooltip', 'sidetext'):
                d[field] = tr_string(val)
            elif field == 'type':
                d['type'] = val.split(',')[0].strip()
            elif field in ('min', 'max'):
                d[field] = val
            elif field == 'mode':
                d['mode'] = val.replace('com', '').lower()
            return
        # composite lines
        m = re.match(r'(?:Line\s+|auto\s+)?(\w+) = (?:Line\s*)?\{(.*)\}$', s)
        if m:
            args = split_args(m.group(2))
            line = {'label': tr_string(args[0]) if args else '', 'options': []}
            if len(args) > 1:
                t = tr_string(args[1])
                if t is not None:
                    line['tooltip'] = t
                else:
                    mm = re.match(r'(\w+)->get_option\((' + STR + r')\)\.opt\.tooltip', args[1])
                    if mm:
                        line['tooltip_of'] = unq(mm.group(2))
                    elif re.match(r'\w+\.tooltip', args[1]):
                        line['tooltip'] = self.defs.get(args[1].split('.')[0], {}).get('tooltip', '')
            if len(args) > 0 and tr_string(args[0]) is None and re.match(r'\w+\.label', args[0]):
                line['label'] = self.defs.get(args[0].split('.')[0], {}).get('label', '')
            self.lines[m.group(1)] = line
            return
        m = re.match(r'(\w+)\.append_option\((.*)\)$', s)
        if m and m.group(1) in self.lines:
            arg = m.group(2).strip()
            o = self.opt_from_get(arg)
            if o is None and arg in self.options:
                o = dict(self.options[arg])
            if o is None:
                mm = re.match(r'Option\s*\{\s*(\w+), (.*)\}$', arg)
                if mm:
                    o = {'key': self.ev(mm.group(2)), 'custom_def': dict(self.defs.get(mm.group(1), {}))}
            if o is None:
                self.warn.append(f'{self.tab}: append_option: {s}')
                return
            self.lines[m.group(1)]['options'].append(o)
            return
        m = re.match(r'(\w+)\.(label_path|full_width) = (.*)$', s)
        if m and m.group(1) in self.lines:
            if m.group(2) == 'label_path':
                self.lines[m.group(1)]['wiki'] = self.ev(m.group(3))
            else:
                self.lines[m.group(1)]['full_width'] = True
            return
        m = re.match(r'(\w+)\.widget = (.*)$', s)
        if m and m.group(1) in self.lines:
            self.lines[m.group(1)]['widget'] = True
            return
        m = re.match(r'(\w+)->append_line\((\w+)\)$', s)
        if m:
            if m.group(2) not in self.lines:
                self.warn.append(f'{self.tab}: append_line of unknown line {s}')
                return
            line = dict(self.lines[m.group(2)])
            if line.get('widget'):   # a desktop widget: the app implements it by key
                line['custom'] = line['options'][0]['key'] if line.get('options') else 'widget:' + line.get('label', '')
            self.add_line(m.group(1), line)
            return
        m = re.match(r'create_line_with_widget\((\w+)\.get\(\), (' + STR + r'), (' + STR + r')', s)
        if m:
            self.add_line(m.group(1), {'custom': unq(m.group(2)), 'wiki': unq(m.group(3))})
            return
        # string vectors used by loops
        m = re.match(r'(?:static\s+)?(?:const\s+)?std::vector<std::string>\s*(\w+)\s*=?\s*\{(.*)\}$', s)
        if m:
            self.vectors[m.group(1)] = [unq(x.strip()) for x in split_args(m.group(2)) if x.strip()]
            return
        if re.search(r'new_optgroup|append_single_option_line|append_line|append_option|add_options_page|create_line_with_widget', s):
            self.warn.append(f'{self.tab}: not understood: {s[:200]}')


# loops over lists that libslic3r computes: expanded by the core at run time (layout.expand)
RUNTIME_LISTS = {'publishable_printer_retraction_options', 'publishable_printer_z_hop_options'}

FUNCS = ['TabPrint::build', 'TabFilament::build', 'TabFilament::add_filament_overrides_page', 'TabPrinter::build_fff',
         'TabPrinter::build_kinematics_page', 'TabPrinter::build_unregular_pages', 'TabPrintModel::build', 'TabPrintPlate::build']


def main():
    orca, out = sys.argv[1], sys.argv[2]
    overrides = json.load(open(sys.argv[3])) if len(sys.argv) > 3 else {}
    src = strip_comments_and_dead_code(open(orca + '/src/slic3r/GUI/Tab.cpp', encoding='utf-8').read())
    funcs = {}
    for f in FUNCS:
        try:
            funcs[f] = parse_block(remove_lambdas(function_body(src, f)))
        except SystemExit as e:
            print('warning:', e, file=sys.stderr)
    warnings = []
    result = {}
    for tab, fn in (('print', 'TabPrint::build'), ('filament', 'TabFilament::build'), ('printer', 'TabPrinter::build_fff')):
        it = Interp(funcs, tab, warnings)
        it.run(funcs[fn])
        result[tab] = it.pages
    # desktop page order of the printer tab: build_unregular_pages inserts the Motion ability, Multimaterial and
    # Extruder pages between "Machine G-code" and "Notes"
    order = overrides.get('printer_page_order')
    if order:
        result['printer'].sort(key=lambda p: order.index(p['title']) if p['title'] in order else len(order))
    for tab, pages in overrides.get('extra', {}).items():
        result.setdefault(tab, []).extend(pages)
    result['custom_defs'] = overrides.get('custom_defs', {})
    for w in warnings:
        print('warning:', w, file=sys.stderr)
    json.dump(result, open(out, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    n = sum(len(l) for t in ('print', 'filament', 'printer') for p in result[t] for g in p['groups'] for l in [g['lines']])
    print(f'{out}: ' + ', '.join(f"{t} {len(result[t])} pages" for t in ('print', 'filament', 'printer')) + f', {n} lines, {len(warnings)} warnings')


if __name__ == '__main__':
    main()
