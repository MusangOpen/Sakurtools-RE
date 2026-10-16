#!/usr/bin/env python3
"""Minecraft mapping probe used for version ports of SakuraTools.

The proxy talks to Minecraft through JNI, so every lookup depends on
(a) class names, (b) member names and (c) descriptors. All three change between
Minecraft versions, and they differ between loaders:

* NeoForge 1.21.x runs with official (Mojang) names for classes *and* members.
* Forge 1.20.1 runs with Mojang class names but SRG member names (m_/f_).

This tool merges the two authoritative sources so a port can be driven by data
instead of guesswork:

* ``client.txt``  - Mojang official mappings (named class -> obfuscated class,
                    named member signature -> obfuscated member name).
* ``joined.tsrg`` - MCPConfig mappings (obfuscated class/member -> SRG name).

Usage
-----
    python tools/mcport/mcport.py members net.minecraft.network.protocol.game.ClientboundLoginPacket
    python tools/mcport/mcport.py ctors   net.minecraft.network.protocol.game.ClientboundLevelChunkWithLightPacket
    python tools/mcport/mcport.py classes 'Clientbound.*Time.*'
    python tools/mcport/mcport.py diff    1.20.1 1.21.10 ClientboundSetTimePacket
    python tools/mcport/mcport.py javap   1.21.10 net.minecraft.network.protocol.game.ClientboundSetTimePacket --code

Cache layout (created by ``fetch``)::

    .portcache/client-<version>.txt      official mappings
    .portcache/mcp-<version>/config/joined.tsrg
    .portcache/client-<version>.jar      client jar (for javap verification)

Only the official mappings and MCPConfig are needed for most queries; the client
jar is only required by ``javap``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CACHE = os.path.join(ROOT, ".portcache")
MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
MCP_CONFIG = "https://maven.minecraftforge.net/de/oceanlabs/mcp/mcp_config/{v}/mcp_config-{v}.zip"
JAVAP = os.environ.get("MCPORT_JAVAP", "javap")

PRIMITIVES = {
    "void": "V",
    "boolean": "Z",
    "byte": "B",
    "char": "C",
    "short": "S",
    "int": "I",
    "long": "J",
    "float": "F",
    "double": "D",
}


def _get(url: str, out: str | None = None) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "SakuraTools-port/1.0"})
    data = urllib.request.urlopen(req, timeout=180).read()
    if out:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as fh:
            fh.write(data)
    return data


def vanilla_version(version: str) -> dict:
    cached = os.path.join(CACHE, f"{version}.json")
    if not os.path.exists(cached):
        manifest = json.loads(_get(MANIFEST))
        entry = next((v for v in manifest["versions"] if v["id"] == version), None)
        if not entry:
            raise SystemExit(f"unknown Minecraft version {version}")
        _get(entry["url"], cached)
    with open(cached, "r", encoding="utf-8") as fh:
        return json.load(fh)


def split_params(params: str) -> list[str]:
    if not params.strip():
        return []
    return [p for p in params.split(",") if p.strip()]


class Member:
    """One mapped member: constructor, method or field."""

    def __init__(self, owner: "Official", signature: str, obf_name: str, is_static: bool) -> None:
        self.signature = signature
        self.obf_name = obf_name
        self.is_static = is_static
        body = signature.strip()
        self.params: list[str] = []
        self.return_type = ""
        if "(" in body and body.endswith(")"):
            self.kind = "method"
            head, params = body.split("(", 1)
            self.params = split_params(params[:-1])
            head = head.strip()
            self.return_type = head.rsplit(" ", 1)[0].strip() if " " in head else "void"
        else:
            self.kind = "field"
            head, _ = body.rsplit(" ", 1)
            self.return_type = head.strip()
        self.desc = self._descriptor(owner, obfuscated=False)
        self.obf_desc = self._descriptor(owner, obfuscated=True)

    def _descriptor(self, owner: "Official", obfuscated: bool) -> str:
        def convert(name: str) -> str:
            name = name.strip()
            if name.endswith("[]"):
                return "[" + convert(name[:-2])
            if name.endswith("..."):
                return "[" + convert(name[:-3])
            if name in PRIMITIVES:
                return PRIMITIVES[name]
            if name.startswith("?"):
                return "Ljava/lang/Object;"
            if obfuscated:
                return "L" + owner.obf_class(name) + ";"
            return "L" + name.replace(".", "/") + ";"

        return "(" + "".join(convert(p) for p in self.params) + ")" + convert(self.return_type) \
            if self.kind == "method" else convert(self.return_type)

    @property
    def is_ctor(self) -> bool:
        return self.kind == "method" and self.signature.split("(", 1)[0].strip().endswith(" <init>")


class Official:
    """Mojang official mappings: named <-> obf, with named-space signatures."""

    def __init__(self, path: str) -> None:
        self.named_to_obf: dict[str, str] = {}
        self.obf_to_named: dict[str, str] = {}
        self.members: dict[str, list[Member]] = {}
        current_named = None
        pending_static = False
        member_re = re.compile(r"^(?:\d+:\d+:)?(.+?) -> (.+)$")
        with open(path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                stripped = line.strip()
                if not line.startswith(" ") and stripped.endswith(":"):
                    named, obf = [p.strip() for p in stripped[:-1].split("->")]
                    current_named = named
                    self.named_to_obf[named] = obf
                    self.obf_to_named[obf] = named
                    self.members.setdefault(named, [])
                    pending_static = False
                    continue
                match = member_re.match(stripped)
                if not match or current_named is None:
                    continue
                signature, obf_name = match.group(1).strip(), match.group(2).strip()
                member = Member(self, signature, obf_name, pending_static)
                pending_static = False
                self.members[current_named].append(member)

    def members_of(self, named_class: str) -> list[Member]:
        return self.members.get(named_class, [])

    def obf_class(self, named_or_dotted: str) -> str:
        return self.named_to_obf.get(named_or_dotted, named_or_dotted.replace(".", "/"))

    def find(self, named_class: str, desc: str, name: str | None = None) -> Member | None:
        """Look a member up by descriptor; optionally narrow by named name."""
        for member in self.members_of(named_class):
            if member.desc != desc:
                continue
            if name and not member.signature.split("(", 1)[0].strip().endswith(" " + name):
                if not member.signature.startswith(name + "("):
                    continue
            return member
        return None


class Tsrg:
    """MCPConfig TSRG2: (obf class, obf member name, obf descriptor) -> SRG name."""

    def __init__(self, path: str) -> None:
        self.classes: dict[str, str] = {}
        self.methods: dict[tuple[str, str, str], str] = {}
        self.fields: dict[tuple[str, str, str], str] = {}
        current = None
        with open(path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                if not line.startswith("\t"):
                    parts = line.split()
                    if len(parts) < 2 or parts[0] == "tsrg2":
                        continue
                    current = parts[0]
                    self.classes[current] = parts[1]
                    continue
                parts = line.strip().split()
                if len(parts) == 4:  # method: obfName obfDesc srgName id
                    self.methods[(current, parts[0], parts[1])] = parts[2]
                elif len(parts) == 3:  # field: obfName srgName id
                    self.fields[(current, parts[0], "")] = parts[1]

    def srg_of(self, obf_class: str, member: Member) -> str:
        if member.kind == "method":
            return self.methods.get((obf_class, member.obf_name, member.obf_desc), "-")
        return self.fields.get((obf_class, member.obf_name, ""), "-")


def load(version: str) -> tuple[Official, Tsrg]:
    official_path = os.path.join(CACHE, f"client-{version}.txt")
    tsrg_path = os.path.join(CACHE, f"mcp-{version}", "config", "joined.tsrg")
    for path in (official_path, tsrg_path):
        if not os.path.exists(path):
            raise SystemExit(f"missing {path} - run: python tools/mcport/mcport.py fetch {version}")
    return Official(official_path), Tsrg(tsrg_path)


def cmd_fetch(args) -> None:
    meta = vanilla_version(args.version)
    _get(meta["downloads"]["client_mappings"]["url"], os.path.join(CACHE, f"client-{args.version}.txt"))
    print(f"mappings   -> .portcache/client-{args.version}.txt")
    if args.with_jar:
        _get(meta["downloads"]["client"]["url"], os.path.join(CACHE, f"client-{args.version}.jar"))
        print(f"client jar -> .portcache/client-{args.version}.jar")
    zip_path = os.path.join(CACHE, f"mcp_config-{args.version}.zip")
    if not os.path.exists(zip_path):
        _get(MCP_CONFIG.format(v=args.version), zip_path)
    extract = os.path.join(CACHE, f"mcp-{args.version}")
    if not os.path.exists(os.path.join(extract, "config", "joined.tsrg")):
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(extract, members=[n for n in zf.namelist() if n.startswith("config") and "patches" not in n])
    print(f"tsrg       -> .portcache/mcp-{args.version}/config/joined.tsrg")


def render(official: Official, tsrg: Tsrg, named: str, member: Member) -> str:
    obf_class = official.named_to_obf.get(named, "?")
    srg = tsrg.srg_of(obf_class, member) if official.named_to_obf.get(named) else "-"
    kind = "C" if member.is_ctor else member.kind[0].upper()
    static = "static " if member.is_static else ""
    return f"  {kind} {member.desc:<9} {static}{member.signature:<78} obf={member.obf_name:<14} srg={srg}"


def cmd_members(args) -> None:
    official, tsrg = load(args.version)
    named = args.name
    if named not in official.members:
        close = [n for n in official.members if named.lower() in n.lower()][:8]
        print(f"{named}: not present in {args.version}" + (f"; close: {close}" if close else ""))
        return
    print(f"{named} -> obf {official.named_to_obf[named]}")
    for member in sorted(official.members_of(named), key=lambda m: (m.kind, m.desc, m.signature)):
        if not args.all and member.kind != "method":
            continue
        if args.filter and args.filter not in member.signature:
            continue
        print(render(official, tsrg, named, member))


def cmd_ctors(args) -> None:
    official, tsrg = load(args.version)
    named = args.name
    if named not in official.members:
        close = [n for n in official.members if named.lower() in n.lower()][:8]
        print(f"{named}: not present in {args.version}" + (f"; close: {close}" if close else ""))
        return
    print(f"{named} -> obf {official.named_to_obf[named]}")
    for member in official.members_of(named):
        if member.is_ctor:
            print(render(official, tsrg, named, member))


def cmd_classes(args) -> None:
    official, _ = load(args.version)
    pattern = re.compile(args.pattern, re.IGNORECASE)
    hits = [name for name in official.members if pattern.search(name)]
    for name in sorted(hits)[: args.limit]:
        print(f"{name} -> {official.named_to_obf[name]}")
    print(f"({len(hits)} match(es))")


def cmd_diff(args) -> None:
    old, _ = load(args.old)
    new, _ = load(args.new)
    names = sorted({*old.members, *new.members}) if args.name == "*" else [args.name]
    for named in names:
        if args.name != "*" and named not in old.members and named not in new.members:
            print(f"{named}: missing in both versions")
            continue
        old_map = {m.desc: m for m in old.members_of(named)}
        new_map = {m.desc: m for m in new.members_of(named)}
        removed = sorted(set(old_map) - set(new_map))
        added = sorted(set(new_map) - set(old_map))
        if not removed and not added and args.name == "*":
            continue
        print(f"== {named}")
        for desc in removed:
            print(f"  - {desc:<10} {old_map[desc].signature}")
        for desc in added:
            print(f"  + {desc:<10} {new_map[desc].signature}")


def cmd_javap(args) -> None:
    official, _ = load(args.version)
    jar = os.path.join(CACHE, f"client-{args.version}.jar")
    if not os.path.exists(jar):
        raise SystemExit(f"missing {jar} - run: python tools/mcport/mcport.py fetch {args.version} --with-jar")
    obf = args.name if "/" in args.name or args.name.startswith("$") else official.named_to_obf.get(args.name, args.name)
    cmd = [JAVAP, "-p", "-s", "-cp", jar, obf.replace("/", ".")]
    if args.code:
        cmd.insert(2, "-c")
    subprocess.run(cmd, check=False)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    fetch = sub.add_parser("fetch", help="download mappings for a version")
    fetch.add_argument("version")
    fetch.add_argument("--with-jar", action="store_true")
    fetch.set_defaults(func=cmd_fetch)

    members = sub.add_parser("members", help="list named members with descriptors and SRG names")
    members.add_argument("name")
    members.add_argument("--version", default="1.21.10")
    members.add_argument("--all", action="store_true", help="include fields")
    members.add_argument("--filter", default="")
    members.set_defaults(func=cmd_members)

    ctors = sub.add_parser("ctors", help="list constructors")
    ctors.add_argument("name")
    ctors.add_argument("--version", default="1.21.10")
    ctors.set_defaults(func=cmd_ctors)

    classes = sub.add_parser("classes", help="search named classes")
    classes.add_argument("pattern")
    classes.add_argument("--version", default="1.21.10")
    classes.add_argument("--limit", type=int, default=40)
    classes.set_defaults(func=cmd_classes)

    diff = sub.add_parser("diff", help="diff member signatures between two versions")
    diff.add_argument("old")
    diff.add_argument("new")
    diff.add_argument("name", nargs="?", default="*")
    diff.set_defaults(func=cmd_diff)

    javap = sub.add_parser("javap", help="disassemble a class from the client jar")
    javap.add_argument("version")
    javap.add_argument("name")
    javap.add_argument("--code", action="store_true", help="include bytecode")
    javap.set_defaults(func=cmd_javap)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
