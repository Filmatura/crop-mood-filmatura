# extract module strings from README.rst
#
# Simplified, Python-3 replacement for the original script: the original
# needed python2, rst2html/docutils, and git/hg history (via
# last_change_info.sh) none of which are available in this extracted,
# non-VCS source tree. This keeps the functional metadata (name, tags)
# and drops only the rendered description/help pages and "last changed"
# info, which only affect the cosmetic About page text in the Modules
# menu, not module behavior.

import sys, re

def c_repr(name):
    if "\n" in name:
        s = "\n"
        for l in name.split("\n"):
            s += "    %s\n" % ('"%s\\n"' % l.replace('"', r'\"'))
        return s
    else:
        return '"%s"' % name.replace('"', r'\"')

strings = []
def add_string(name, value):
    a = chr(ord('a') + len(strings))
    print("static char __module_string_%s_name [] MODULE_STRINGS_SECTION = %s;" % (a, c_repr(name)))
    print("static char __module_string_%s_value[] MODULE_STRINGS_SECTION = %s;" % (a, c_repr(value)))
    strings.append((name, value))

def declare_string_section():
    print()
    print("#define MODULE_STRINGS() \\")
    print("  MODULE_STRINGS_START() \\")
    for i, s in enumerate(strings):
        a = chr(ord('a') + i)
        print("    MODULE_STRING(__module_string_%s_name, __module_string_%s_value) \\" % (a, a))
    print("  MODULE_STRINGS_END()")

inp = open("README.rst").read().replace("\r\n", "\n")
lines = inp.strip("\n").split("\n")
title = lines[0]

add_string("Name", title)

tags = {}
for l in lines[2:]:
    l = l.strip()
    m = re.match("^:([^:]+):(.+)$", l)
    if m:
        name = m.group(1).strip()
        value = m.group(2).strip()
        if value.startswith("<") and value.endswith(">"):
            continue
        add_string(name, value)
        tags[name] = value

declare_string_section()
