#!/usr/bin/env python3
"""Check Minecraft JNI references in the native sources against a version.

The proxy resolves Minecraft members by name + descriptor at runtime, so a
version port has to know exactly which of those references still exist. This
tool scans ``native/`` for JNI lookups and reports, per reference:

* ``ok``           - the descriptor still exists with the same name
* ``renamed``      - the descriptor exists under another name
* ``desc-changed`` - the name exists but no member has that descriptor
* ``missing``      - neither name nor descriptor exists
* ``class-missing``- a referenced ``net.minecraft``/``com.mojang`` class is gone
* ``unattributed`` - the lookup's owner cannot be derived statically

Coverage: ``SnapshotJni`` calls and ``FindMappedMethod`` calls with a known
owner variable. ``PACKET("Foo")``, file-local ``const char*`` constants and the
``#define PACKET(NAME)`` prefix are resolved before lookup. Descriptor-pinned
lookups (``findMethodByDescriptor``) are listed with the number of classes that
still declare a method with that descriptor.

Usage::

    python tools/mcport/check_sources.py                     # default 1.21.10
    python tools/mcport/check_sources.py --version 1.20.1    # sanity check
    python tools/mcport/check_sources.py --report .portcache/check.txt --only missing,desc-changed,renamed
"""

from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcport import load  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NATIVE = os.path.join(ROOT, "native")

# Variables whose Minecraft class is unambiguous in the current sources.
OWNER_VARS = {
    "server.refs.minecraftCls": "net.minecraft.client.Minecraft",
    "mcCls": "net.minecraft.client.Minecraft",
    "teleport": "net.minecraft.network.protocol.game.ServerboundAcceptTeleportationPacket",
    "listener": "net.minecraft.client.multiplayer.ClientPacketListener",
    "gpCls": "com.mojang.authlib.GameProfile",
    "userCls": "net.minecraft.client.User",
    "piuCls": "net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket",
    "piEntryCls": "net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket$Entry",
    "gameTypeCls": "net.minecraft.world.level.GameType",
    "sptCls": "net.minecraft.network.protocol.game.ClientboundSetPlayerTeamPacket",
    "server.refs.connectionCls": "net.minecraft.network.Connection",
}

SNAPSHOT_HELPERS = {"object", "stat", "integer", "real", "decimal", "longValue", "boolean",
                    "get", "getInt", "method", "field", "make"}
CLASS_LITERAL = re.compile(r'"(((?:net|com)\.(?:minecraft|mojang)[\w.$]+))"')
CONST_DEF = re.compile(r'const(?:expr)?\s+char\s*\*\s*(\w+)\s*=\s*"([^"]+)"')
DEFINE_PREFIX = re.compile(r'#define\s+(\w+)\(NAME\)\s+"([^"]+)"\s+NAME')
FIND_MAPPED = re.compile(r'FindMappedMethod\(\s*env\s*,\s*([\w.]+)\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"')
FIND_BY_DESC = re.compile(r'findMethod(?:s)?ByDescriptor(?:Except)?\(\s*[\w.]+,\s*"([^"]+)"')


def iter_sources():
    for base, _, files in os.walk(NATIVE):
        for name in sorted(files):
            if name.endswith((".cpp", ".h")):
                yield os.path.join(base, name)


def split_args(text: str) -> list[str]:
    """Split a call argument list on top-level commas."""
    args, depth, current, in_string = [], 0, "", False
    for ch in text:
        if in_string:
            current += ch
            if ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            current += ch
        elif ch in "([{":
            depth += 1
            current += ch
        elif ch in ")]}":
            depth -= 1
            current += ch
        elif ch == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += ch
    if current.strip():
        args.append(current.strip())
    return args


def resolve_owner(expr: str, consts: dict[str, str], macros: dict[str, str]) -> str | None:
    expr = expr.strip()
    literal = re.fullmatch(r'"([^"]+)"', expr)
    if literal:
        return literal.group(1)
    macro = re.fullmatch(r'(\w+)\("([^"]+)"\)', expr)
    if macro and macro.group(1) in macros:
        return macros[macro.group(1)] + macro.group(2)
    if expr in consts:
        return consts[expr]
    return None


def simple_name(member) -> str:
    head = member.signature.split("(", 1)[0].strip()
    return head.rsplit(" ", 1)[-1].strip()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", default="1.21.10")
    parser.add_argument("--report", default="")
    parser.add_argument("--only", default="", help="comma separated status filter for stdout")
    args = parser.parse_args()

    official, _ = load(args.version)
    rows: list[tuple[str, str, str, str, str, str]] = []
    class_refs: dict[str, str] = {}
    descriptors: dict[str, set[str]] = {}

    for path in iter_sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        text = open(path, "r", encoding="utf-8").read()
        consts = {name: value for name, value in CONST_DEF.findall(text)}
        macros = {name: prefix for name, prefix in DEFINE_PREFIX.findall(text)}

        for hit in CLASS_LITERAL.finditer(text):
            class_name = hit.group(1).replace("/", ".")
            if class_name.endswith(".") or class_name.startswith("com.mojang.authlib"):
                continue  # macro prefixes and authlib live outside the game mappings
            class_refs.setdefault(class_name, rel)
        for hit in FIND_MAPPED.finditer(text):
            owner_var, name, _srg, desc = hit.groups()
            owner = OWNER_VARS.get(owner_var)
            if owner:
                rows.append(("(findmapped)", owner, name, desc, rel, ""))
            else:
                rows.append(("unattributed", owner_var, name, desc, rel, ""))
        for hit in FIND_BY_DESC.finditer(text):
            descriptors.setdefault(hit.group(1), set()).add(rel)

        for call in re.finditer(r'\bj\.(\w+)\s*\(', text):
            helper = call.group(1)
            if helper not in SNAPSHOT_HELPERS:
                continue
            start = call.end() - 1
            depth, end = 0, None
            for index in range(start, len(text)):
                if text[index] == "(":
                    depth += 1
                elif text[index] == ")":
                    depth -= 1
                    if depth == 0:
                        end = index
                        break
            if end is None:
                continue
            argv = split_args(text[start + 1:end])
            if helper == "make":
                if len(argv) < 2:
                    continue
                owner = resolve_owner(argv[0], consts, macros)
                desc = re.fullmatch(r'"([^"]+)"', argv[1])
                if owner and desc:
                    rows.append(("(ctor)", owner, "<init>", desc.group(1), rel, ""))
                continue
            if len(argv) < 3:
                continue
            owner = resolve_owner(argv[0], consts, macros)
            name = re.fullmatch(r'"([^"]+)"', argv[1])
            desc = re.fullmatch(r'"([^"]+)"', argv[3]) if len(argv) >= 4 else None
            if owner and name and desc:
                rows.append(("(snapshot)", owner, name.group(1), desc.group(1), rel, ""))
            elif owner and name:
                rows.append(("(field-name)", owner, name.group(1), "", rel, ""))

    resolved: list[tuple[str, str, str, str, str, str]] = []
    for status, owner, name, desc, rel, extra in rows:
        if status == "unattributed" or not owner:
            resolved.append(("unattributed", owner, name, desc, rel, extra))
            continue
        if not owner.startswith(("net.minecraft", "com.mojang")):
            resolved.append(("external", owner, name, desc, rel, extra))
            continue
        if owner not in official.members:
            close = [n for n in official.members if n.endswith("." + owner.rsplit(".", 1)[-1])][:3]
            resolved.append(("class-missing", owner, name, desc, rel, "; ".join(close)))
            continue
        same_desc = [m for m in official.members_of(owner) if m.desc == desc]
        if not same_desc:
            if desc:
                named = [m for m in official.members_of(owner) if simple_name(m) == name]
                detail = "; ".join(f"{m.signature} -> {m.desc}" for m in named[:3])
                resolved.append(("desc-changed" if named else "missing", owner, name, desc, rel, detail))
            else:
                named = [m for m in official.members_of(owner) if simple_name(m) == name]
                if named:
                    resolved.append(("ok", owner, name, named[0].desc, rel, f"{named[0].kind} obf={named[0].obf_name}"))
                else:
                    resolved.append(("missing", owner, name, "", rel, "field/name not found"))
            continue
        hit = next((m for m in same_desc if simple_name(m) == name), None)
        if hit:
            resolved.append(("ok", owner, name, desc, rel, f"obf={hit.obf_name}"))
        else:
            resolved.append(("renamed", owner, name, desc, rel, "; ".join(m.signature for m in same_desc[:2])))

    for class_name, rel in sorted(class_refs.items()):
        if class_name in official.members:
            continue
        close = [n for n in official.members if n.endswith("." + class_name.rsplit(".", 1)[-1])][:3]
        resolved.append(("class-missing", class_name, "", "", rel, "; ".join(close)))

    for desc, files in sorted(descriptors.items()):
        owners = [c for c in official.members if any(m.desc == desc for m in official.members_of(c))]
        status = "by-descriptor" if owners else "by-descriptor:none"
        resolved.append((status, ",".join(sorted(files)), "", desc, "", f"{len(owners)} class(es)"))

    wanted = {s for s in args.only.split(",") if s} or None
    counts: dict[str, int] = {}
    out = []
    for status, owner, name, desc, rel, extra in resolved:
        counts[status] = counts.get(status, 0) + 1
        if wanted and status not in wanted:
            continue
        out.append(f"[{status:<17}] {owner}.{name}{desc}  ({rel}) {extra}".rstrip())
    for status, count in sorted(counts.items()):
        print(f"{status}: {count}")
    print("\n".join(out))
    if args.report:
        os.makedirs(os.path.dirname(args.report), exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(f"# target {args.version}\n")
            fh.write("\n".join(f"[{s:<17}] {o}.{n}{d}  ({r}) {e}".rstrip()
                               for s, o, n, d, r, e in resolved))
        print(f"report -> {args.report}")


if __name__ == "__main__":
    sys.exit(main())
