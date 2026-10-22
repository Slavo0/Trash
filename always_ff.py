#!/usr/bin/env python3
"""Find suspicious blocking '=' assignments inside SystemVerilog always_ff.

Run:
    python find_always_ff_blocking.py "C:\\path\\to\\module.sv"
Or run without arguments and paste the file path when prompted.

Uses a lightweight tokenizer and procedural statement parser; no extra packages.
This is a heuristic checker, not a full SystemVerilog compiler.
"""

import argparse
from bisect import bisect_right
from dataclasses import dataclass
from pathlib import Path
import re
import sys


TOKEN_PATTERN = re.compile(
    r'(?P<directive>^[ \t]*`[^\n]*(?:\\\n[^\n]*)*)'
    r'|(?P<line_comment>//[^\n]*)'
    r'|(?P<block_comment>/\*.*?\*/)'
    r'|(?P<string>"(?:\\.|[^"\\])*")'
    r'|(?P<space>\s+)'
    r'|(?P<escaped_id>\\[^\s]+)'
    r'|(?P<identifier>[A-Za-z_$][A-Za-z0-9_$]*)'
    r'|(?P<operator>===|!==|<<<\=?|>>>\=?|<<=|>>=|<=|>=|==|!='
    r'|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|&&|\|\|)'
    r'|(?P<other>[^\s])',
    re.MULTILINE | re.DOTALL,
)

# Initializers in declarations are not procedural blocking assignments.
DECLARATION_WORDS = {
    "reg", "logic", "bit", "byte", "shortint", "int", "longint", "integer",
    "time", "realtime", "real", "shortreal", "string", "event", "chandle",
    "var", "wire", "tri", "signed", "unsigned", "genvar", "parameter",
    "localparam", "typedef", "const",
}


@dataclass(frozen=True)
class Token:
    value: str
    offset: int
    line: int
    column: int


def tokenize(source: str) -> list[Token]:
    line_starts = [0] + [m.end() for m in re.finditer(r"\n", source)]
    result = []
    for match in TOKEN_PATTERN.finditer(source):
        if match.lastgroup in {
            "directive", "line_comment", "block_comment", "string", "space"
        }:
            continue
        offset = match.start()
        line_index = bisect_right(line_starts, offset) - 1
        result.append(Token(match.group(), offset, line_index + 1,
                            offset - line_starts[line_index] + 1))
    return result


class FFScanner:
    def __init__(self, tokens: list[Token]):
        self.t = tokens
        self.n = len(tokens)
        self.matches: set[int] = set()

    def at(self, i: int) -> str:
        return self.t[i].value if i < self.n else ""

    def collect_equals(self, start: int, end: int) -> None:
        for i in range(start, min(end, self.n)):
            if self.t[i].value == "=":
                self.matches.add(i)

    def skip_group(self, i: int, *, collect: bool = False) -> int:
        """Skip a balanced (...), [...], or {...} group."""
        pairs = {"(": ")", "[": "]", "{": "}"}
        opening = self.at(i)
        if opening not in pairs:
            return i
        stack = [pairs[opening]]
        j = i + 1
        while j < self.n and stack:
            value = self.at(j)
            if value in pairs:
                stack.append(pairs[value])
            elif value == stack[-1]:
                stack.pop()
            j += 1
        if collect:
            self.collect_equals(i, j)
        return j

    def statement(self, i: int) -> int:
        if i >= self.n:
            return i
        kind = self.at(i)

        # begin : label ... end : label
        if kind == "begin":
            i += 1
            if self.at(i) == ":":
                i += 2  # optional named block
            while i < self.n and self.at(i) != "end":
                i = self.statement(i)
            if self.at(i) == "end":
                i += 1
                if self.at(i) == ":":
                    i += 2
            return i

        # fork ... join / join_any / join_none
        if kind == "fork":
            i += 1
            if self.at(i) == ":":
                i += 2
            while i < self.n and self.at(i) not in ("join", "join_any", "join_none"):
                i = self.statement(i)
            if self.at(i) in ("join", "join_any", "join_none"):
                i += 1
            return i

        if kind == "if":
            i += 1
            if self.at(i) == "(":
                i = self.skip_group(i, collect=True)
            i = self.statement(i)
            if self.at(i) == "else":
                i = self.statement(i + 1)
            return i

        if kind in ("case", "casex", "casez", "randcase"):
            i += 1
            if self.at(i) == "(":
                i = self.skip_group(i, collect=True)
            while i < self.n and self.at(i) != "endcase":
                # Skip ternary colons and bracketed/parenthesized expressions.
                ternary = 0
                while i < self.n and self.at(i) != "endcase":
                    ch = self.at(i)
                    if ch == "?":
                        ternary += 1
                    elif ch == ":":
                        if ternary:
                            ternary -= 1
                        else:
                            i += 1
                            break
                    i += 1
                if self.at(i) == "endcase":
                    break
                i = self.statement(i)
            if self.at(i) == "endcase":
                i += 1
                if self.at(i) == ":":
                    i += 2
            return i

        if kind in ("for", "foreach", "while", "repeat", "wait"):
            i += 1
            if self.at(i) == "(":
                # An 'i = 0' in a for-loop header is normally intentional.
                i = self.skip_group(i, collect=(kind not in ("for", "foreach")))
            return self.statement(i)

        if kind == "forever":
            return self.statement(i + 1)

        if kind == "do":
            i = self.statement(i + 1)
            if self.at(i) == "while":
                i += 1
                if self.at(i) == "(":
                    i = self.skip_group(i, collect=True)
                if self.at(i) == ";":
                    i += 1
            return i

        if kind in ("unique", "unique0", "priority"):
            return self.statement(i + 1)

        if kind in ("@", "#"):
            i += 1
            if self.at(i) == "(":
                i = self.skip_group(i)
            else:
                i += 1  # @event, @*, #5, etc.
            return self.statement(i)

        # End tokens are handled by their enclosing block.
        if kind in ("end", "endcase", "join", "join_any", "join_none", "else"):
            return i + 1

        # Basic statement, assignment or declaration, up to the top-level ';'.
        start = i
        while i < self.n:
            value = self.at(i)
            if value == ";":
                i += 1
                break
            if value in ("(", "[", "{"):
                break  # malformed / incomplete code, avoid eating next block
            i += 1
        is_declaration = self.at(start) in DECLARATION_WORDS
        if self.at(start) in ("automatic", "static"):
            is_declaration = self.at(start + 1) in DECLARATION_WORDS
        if not is_declaration:
            self.collect_equals(start, i)
        return max(i, start + 1)

    def find(self) -> list[Token]:
        for i, token in enumerate(self.t):
            if token.value != "always_ff":
                continue
            start = i + 1
            if self.at(start) == "@":
                start += 1
                if self.at(start) == "(":
                    start = self.skip_group(start)
                else:
                    start += 1
            if start < self.n:
                self.statement(start)
        return [self.t[i] for i in sorted(self.matches)]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Find blocking '=' inside always_ff (including nested if/begin/end)."
    )
    parser.add_argument("file", nargs="?", help="Path to a SystemVerilog .sv file")
    args = parser.parse_args()

    path_string = args.file or input("Путь к .sv файлу: ").strip()
    # Support pasted Windows paths with surrounding quotes.
    path = Path(path_string.strip().strip('"').strip("'"))
    if not path.is_file():
        print(f"Файл не найден: {path}", file=sys.stderr)
        return 2
    try:
        content = path.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError:
        print("Файл не читается как UTF-8. Сохраните его в UTF-8.", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"Не удалось прочитать файл: {exc}", file=sys.stderr)
        return 2

    found = FFScanner(tokenize(content)).find()
    if not found:
        print("Подозрительных '=' внутри always_ff не найдено.")
        return 0

    lines = content.splitlines()
    print(f"Найдено подозрительных присваиваний: {len(found)}\n")
    for token in found:
        line_text = lines[token.line - 1].strip()
        print(f"{path}:{token.line}:{token.column}: {line_text}")
    # Return non-zero so this can also be used as a CI check.
    return 1


if __name__ == "__main__":
    sys.exit(main())
                i = self.skip_group(i)
                continue
            if value in ("end", "endcase", "join", "join_any", "join_none"):
                    if ch in ("(", "[", "{"):
                        i = self.skip_group(i, collect=True)
                        continue

