#!/usr/bin/env python3
"""prose_as_code.py — find English sentences that a shell will try to execute.

WHY
---
This tree is heavily commented, and comment blocks wrap across many lines. When a continuation line
loses its `#`, the prose becomes executable. Found in emu_layer4_fcm_test.sh, where three lines of
an explanation ran as commands on every single run:

    one: command not found
    bash: integer expression expected: No such file or directory
    never: command not found

`bash -n` does NOT catch this — the lines are syntactically valid commands. It was harmless there by
luck. It would not be harmless if the stray line began with a word that IS a command: a sentence
starting with "test ...", "printf ...", "kill ...", "rm ..." or "exit ..." parses as an instruction
and does something. `exit` is the one that should worry anybody: a comment continuation beginning
with the word "exit" ends the script early, and every check after it silently never runs — a whole
test file passing because it stopped before testing anything.

HOW IT DECIDES
    A line is suspect when it is not a comment, not blank, not inside a heredoc or a quoted string,
    it directly follows or sits among comment lines, and its first word is neither a shell keyword,
    nor an assignment/function definition, nor any command name known to exist (host PATH plus the
    container-only tools listed below).

    That last clause is why `bash "integer expression expected"` above is NOT flagged on its own —
    `bash` is a real command. Detection is per-BLOCK, so the neighbouring lines flag the block and a
    human reads the whole thing. Missing one line of a block that is reported anyway costs nothing.

    python3 prose_as_code.py FILE...      # exit 1 if any suspect line is found
"""
import sys, os, re, shutil

# Commands that exist only inside the project's containers, so `shutil.which` cannot see them from
# the host. Anything here is a real command, not prose.
CONTAINER_CMDS = {
    "adb", "emulator", "apksigner", "zipalign", "aapt", "aapt2", "apktool", "d8", "dx", "sdkmanager",
    "avdmanager", "qemu-system-aarch64", "qemu-aarch64", "ndk-build", "cmake", "ninja", "clang",
    "clang++", "llvm-strip", "llvm-objdump", "llvm-readelf", "aarch64-linux-gnu-gcc", "keytool",
    "jarsigner", "unzip", "zip", "monkey", "logcat", "screencap", "getprop", "pidof", "settle_frames",
}
KEYWORDS = {
    "if", "then", "else", "elif", "fi", "for", "do", "done", "while", "until", "case", "esac",
    "function", "select", "time", "coproc", "in", "{", "}", "(", ")", "!", "[", "[[", ":", ".",
    "local", "return", "exit", "export", "readonly", "declare", "typeset", "unset", "shift",
    "source", "eval", "exec", "trap", "set", "read", "wait", "break", "continue",
    # builtins that are not reserved words but are still commands; `command -v x` is the idiom this
    # tree uses to probe for tools, and it was flagged as prose until these were listed.
    "command", "builtin", "type", "hash", "let", "pushd", "popd", "alias", "jobs", "kill",
    "getopts", "mapfile", "umask", "ulimit", "printf", "echo", "true", "false",
}


def first_word(line):
    s = line.strip()
    m = re.match(r"([A-Za-z_][A-Za-z0-9_.+-]*)", s)
    return m.group(1) if m else None


# Keywords whose ARGUMENTS have a shape. `exit` is in KEYWORDS, so "exit early is what this would
# do" was accepted as code by the first version of this file — the exact case the docstring above
# calls the one to worry about. A keyword is not enough; the arguments have to make sense too.
NUMERIC_ARG = {"exit", "return", "shift", "break", "continue"}
IDENT_ARG = {"local", "export", "readonly", "unset", "declare", "typeset", "read"}
NUM_RE = re.compile(r'^(\d+|\$\S+|"\$[^"]+"|\$\{\w+\})$')
IDENT_RE = re.compile(r"^-{0,2}[A-Za-z_][A-Za-z0-9_]*(\+?=.*)?$")


def keyword_args_ok(w, rest):
    # Only the first simple command on the line: `local ab; ab=$(...)` is two commands.
    rest = re.split(r";|\|\||&&|\||#", rest, 1)[0].strip()
    toks = rest.split()
    if w in NUMERIC_ARG:
        if not toks:
            return True
        return len(toks) == 1 and bool(NUM_RE.match(toks[0]))
    if w in IDENT_ARG:
        # FIRST token only. `export VAR="$(cd "$X" && pwd)/y"` splits on whitespace INSIDE a quoted
        # command substitution, so checking every token rejected valid code. The strict check that
        # earns its keep is the numeric one above ("return to the menu" / "exit the app" are
        # plausible openings for a wrapped comment line); a stray line starting with `local` or
        # `export` is not, so leniency here costs little.
        return not toks or bool(IDENT_RE.match(toks[0]))
    return True


def looks_like_code(line, funcs=frozenset()):
    s = line.strip()
    if not s:
        return True
    # assignment, function def, redirect, pipe-leading, continuation, subshell, arithmetic
    if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\+?=", s):          # VAR=... / VAR+=...
        return True
    if re.match(r"^[A-Za-z_][A-Za-z0-9_-]*\s*\(\s*\)", s):    # name() {
        return True
    if s[0] in "|&<>(){}[]$\"'`#-*/=+\\":
        return True
    w = first_word(s)
    if w is None:
        return True
    rest = s[len(w):]
    if w in NUMERIC_ARG or w in IDENT_ARG:
        return keyword_args_ok(w, rest)
    if w in KEYWORDS or w in CONTAINER_CMDS or w in funcs:
        return True
    if shutil.which(w):
        return True
    return False


FUNC_RE = re.compile(r"^\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)")
SRC_RE = re.compile(r"^\s*(?:source|\.)\s+.*?([A-Za-z0-9_.-]+\.sh)")


def is_shell(path):
    """Only files that a shell actually runs. `work803/assets/.../varyings.sh` is GLSL shader source
    extracted from the game that happens to end in .sh; scanning it produced 150 confident findings
    about a file no shell will ever execute. A rule that flags game assets as shell bugs is worse
    than no rule."""
    try:
        with open(path, "rb") as fh:
            head = fh.readline(200)
    except OSError:
        return False
    return head.startswith(b"#!") and (b"sh" in head)


def known_functions(path):
    """Function names defined in this file, plus those in any lib it sources from the same directory.
    Without this, every call to a helper the file itself defines (`case_run ...`, `say ...`) reads as
    an unknown command, i.e. as prose — 12 such false positives in mutation_test.sh alone."""
    names = set()
    d = os.path.dirname(os.path.abspath(path))
    todo = [path]
    seen = set()
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    m = FUNC_RE.match(line)
                    if m:
                        names.add(m.group(1))
                    m = SRC_RE.match(line)
                    if m:
                        cand = os.path.join(d, m.group(1))
                        if os.path.exists(cand):
                            todo.append(cand)
        except OSError:
            pass
    return names


def scan(path, funcs=frozenset()):
    """Return [(lineno, text)] of lines that are prose sitting in command position."""
    out = []
    in_heredoc = None
    near_comment = False
    quote = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    for i, raw in enumerate(lines, 1):
        line = raw.rstrip("\n")
        stripped = line.strip()

        # --- heredocs: everything until the terminator is data, not code
        if in_heredoc is not None:
            if stripped == in_heredoc:
                in_heredoc = None
            continue
        m = re.search(r"<<-?\s*['\"]?([A-Za-z_][A-Za-z0-9_]*)['\"]?", line)
        if m and not stripped.startswith("#"):
            in_heredoc = m.group(1)
            continue

        # --- multi-line quoted strings: a say "....\n...." spans lines and is data
        if quote is not None:
            if quote in line:
                quote = None
            continue
        if not stripped.startswith("#"):
            # crude but adequate: an odd number of unescaped double quotes opens a multi-line string
            dq = len(re.findall(r'(?<!\\)"', line))
            if dq % 2 == 1:
                quote = '"'
                continue

        if not stripped:
            continue
        if stripped.startswith("#"):
            near_comment = True
            continue
        if near_comment and not looks_like_code(line, funcs):
            out.append((i, stripped))
        else:
            # a line of real code ends the comment block's neighbourhood
            near_comment = False
    return out


def main(argv):
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    total = 0
    scanned = skipped = 0
    for p in argv:
        if not is_shell(p):
            skipped += 1
            continue
        scanned += 1
        try:
            hits = scan(p, known_functions(p))
        except Exception as e:
            print(f"  [FAIL] {p}: {e}")
            total += 1
            continue
        for ln, text in hits:
            print(f"  [FAIL] {p}:{ln}: prose in command position: {text[:88]}")
            total += 1
    # The skip count is PRINTED, never silent: "0 findings" from a run that scanned nothing is the
    # defect this whole suite exists to hunt.
    note = f" ({skipped} non-shell file(s) skipped)" if skipped else ""
    if total:
        print(f"\n  {total} line(s) of prose would be executed as commands{note}")
        return 1
    if scanned == 0:
        print(f"  [FAIL] nothing was scanned{note} — that is not a pass")
        return 1
    print(f"  [ OK ] {scanned} shell file(s): no prose in command position{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
