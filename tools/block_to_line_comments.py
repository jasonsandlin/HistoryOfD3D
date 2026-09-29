#!/usr/bin/env python3
"""Convert C/C++ block comments (/* ... */) to line comments (// ...).

Rules:
  * String and char literals are preserved verbatim (a /* inside a string is
    NOT treated as a comment).
  * Existing // line comments are left untouched.
  * A standalone block comment (only whitespace before /* on its line) becomes
    one // line per source line, keeping the original indentation and stripping
    a leading decorative '*'.
  * A trailing block comment (code before /*, nothing meaningful after */)
    becomes 'code // text'.
  * A block comment with code AFTER */ on the same line is moved to a trailing
    // comment at the end of that physical line, so no code gets commented out.

Only files passed on the command line are processed. Vendored third-party
sources must not be passed in.
"""
import sys


def convert(src: str) -> str:
    out = []
    i, n = 0, len(src)
    pending_trailing = []  # comments appended as // ... right before next newline

    def flush_before_newline():
        if pending_trailing:
            out.append(" // " + "  ".join(pending_trailing))
            pending_trailing.clear()

    while i < n:
        c = src[i]

        # string or char literal: copy verbatim
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                d = src[i]
                out.append(d)
                if d == '\\' and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                i += 1
                if d == q:
                    break
            continue

        # existing line comment: copy to end of line
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                out.append(src[i])
                i += 1
            continue

        # block comment
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = i + 2
            while j < n and not (src[j] == '*' and j + 1 < n and src[j + 1] == '/'):
                j += 1
            inner = src[i + 2:j]
            end = j + 2  # index just past */

            line_start = src.rfind('\n', 0, i) + 1
            prefix = src[line_start:i]
            has_code_before = prefix.strip() != ''
            indent = prefix[:len(prefix) - len(prefix.lstrip())]

            nl = src.find('\n', end)
            suffix = src[end:nl if nl != -1 else n]
            has_code_after = suffix.strip() != ''

            raw_lines = inner.split('\n')

            def clean(line: str) -> str:
                s = line.strip()
                if s.startswith('*'):
                    s = s[1:].lstrip()
                return s

            cleaned = [clean(l) for l in raw_lines]

            if has_code_after:
                # move whole comment to a trailing // at end of the physical line
                text = ' '.join(t for t in (l.strip() for l in raw_lines) if t)
                pending_trailing.append(text)
                i = end
                if i < n and src[i] == ' ':
                    i += 1  # swallow the single separator space (e.g. "*/ {" -> "{")
                continue

            if has_code_before:
                # trailing comment: code // text
                out.append('// ' + cleaned[0] if cleaned[0] else '//')
                for extra in cleaned[1:]:
                    out.append('\n' + indent + ('// ' + extra if extra else '//'))
                i = end
                continue

            # standalone block comment: one // line per source line
            parts = []
            for idx, t in enumerate(cleaned):
                if idx == 0 and t == '':
                    parts.append('//')
                else:
                    parts.append('// ' + t if t else '//')
            out.append(('\n' + indent).join(parts))
            i = end
            continue

        if c == '\n':
            flush_before_newline()
            out.append(c)
            i += 1
            continue

        out.append(c)
        i += 1

    flush_before_newline()
    return ''.join(out)


def main():
    for path in sys.argv[1:]:
        with open(path, 'r', encoding='utf-8', newline='') as f:
            src = f.read()
        new = convert(src)
        if new != src:
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            print('converted', path)
        else:
            print('unchanged', path)


if __name__ == '__main__':
    main()
