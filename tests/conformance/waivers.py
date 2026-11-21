"""The one matcher for differences.json (run.py --differences) and volatile.json (regress.py).

An entry has regexes "interface", "method" and "field", and may also have "apptype" (a regex of the app type's
number), "argset" (a regex of the argument set: valid, edge, null, invalid) and "arch" (x64 or x86, for differences
only). "interface" matches a case's name too. A call through a function table ("FnTable:<version>") is the same call
as the interface's: an entry for the interface is for both.
"""

import re


def names_of(record):
    who = record.get("interface", record.get("case", ""))
    return {who, who[len("FnTable:"):]} if who.startswith("FnTable:") else {who}


def waived(rules, record, field, arch=None):
    """Whether an entry of rules covers this record's field (with arch, entries for another arch don't)"""
    names = names_of(record)
    for d in rules:
        if arch is not None and d.get("arch", arch) != arch:
            continue
        if d["field"] != field and not re.fullmatch(d["field"], field):
            continue
        if not any(re.fullmatch(d["interface"], n) for n in names) or not re.fullmatch(d["method"], record.get("method", "")):
            continue
        if "apptype" in d and not re.fullmatch(d["apptype"], str(record.get("apptype", ""))):
            continue
        if "argset" in d and not re.fullmatch(d["argset"], str(record.get("argset", ""))):
            continue
        return True
    return False


def self_test():
    """Returns the cases that failed"""
    rec = {"interface": "FnTable:IVRSystem_022", "method": "PollNextEvent", "apptype": 1, "argset": "null"}
    cases = [
        ("an interface entry covers its function table", [{"interface": "IVRSystem_\\d+", "method": "PollNextEvent", "field": "ret"}], "ret", None, True),
        ("a field regex", [{"interface": "IVRSystem_\\d+", "method": ".*", "field": "ret|out"}], "out", None, True),
        ("another field", [{"interface": "IVRSystem_\\d+", "method": ".*", "field": "ret"}], "out", None, False),
        ("another app type", [{"interface": ".*", "method": ".*", "field": "ret", "apptype": "2"}], "ret", None, False),
        ("another argument set", [{"interface": ".*", "method": ".*", "field": "ret", "argset": "valid|edge"}], "ret", None, False),
        ("its argument set", [{"interface": ".*", "method": ".*", "field": "ret", "argset": "null"}], "ret", None, True),
        ("another arch", [{"interface": ".*", "method": ".*", "field": "ret", "arch": "x86"}], "ret", "x64", False),
        ("its arch", [{"interface": ".*", "method": ".*", "field": "ret", "arch": "x64"}], "ret", "x64", True),
    ]
    return [what for what, rules, field, arch, want in cases if waived(rules, rec, field, arch) != want]
