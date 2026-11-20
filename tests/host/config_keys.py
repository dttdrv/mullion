#!/usr/bin/env python3
"""contract: an option that no document names is one nobody can use, and a documented option that nothing reads is a
lie a user will trust. So both directions are held to: every name src/ asks the configuration for by literal appears
in mullion.conf, and every name mullion.conf documents is asked for somewhere -- as that literal, or, for the router's
per-API keys, built the way src/mullion/mullion_router.hpp builds it from the API names in
src/mullion/mullion_router_policy.hpp. Those are derived from the source rather than listed here, so an API added to
the router without its configuration line fails this check instead of passing it.

One name is read as a legacy fallback and is deliberately not offered as its own setting: the alias named in
LEGACY_FALLBACKS below. It is still held to, in the weaker form -- the option it falls back from must document the
alias, so the old name is findable and not folklore.

The checks then run against made-to-order fixtures, because a guard that has only ever passed has not been shown to
fail: a name read in code and undocumented, and a documented name nothing reads, each have to be reported."""
import pathlib
import re
import sys
import tempfile

GET_OPTION = re.compile(r"""getOption(?:<[^>]*>)?\(\s*"([^"]+)\"""")
# (std::string("router.") + name(api)).c_str(): a name built at run time, so it appears in no literal
CONSTRUCTED = re.compile(r"""getOption(?:<[^>]*>)?\(\s*\(\s*std::string\("([^"]+)"\)\s*\+\s*(\w+)\(""")
DOCUMENTED = re.compile(r"^[ \t]*#[ \t]*([A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+)[ \t]*=", re.M)
ROUTER_API = re.compile(r'return "([A-Za-z0-9_]+)";')

# read as a fallback from the name before it, and said in that option's comment rather than listed on its own
LEGACY_FALLBACKS = {"dxmt.shaderMetalVersion": "mullion.shaderMetalVersion"}


def names_in_source(root):
  """every option name src/ asks for: literals, plus the per-API names the router builds at run time"""
  found = set()
  # only src/ names an option, so only src/ is walked: a first version scanned the whole tree, build directories and
  # all, and took over two minutes to grep a few dozen lines
  sources = [p for p in sorted((root / "src").rglob("*"))
             if p.is_file() and p.suffix in (".cpp", ".hpp", ".c", ".h")]
  for path in sources:
    found.update(GET_OPTION.findall(path.read_text(errors="replace")))
  for path in sources:
    for prefix, builder in CONSTRUCTED.findall(path.read_text(errors="replace")):
      for api in api_names(root, builder):
        found.add(prefix + api)
  return found


def api_names(root, builder):
  """the names `name(Api)` can return -- derived, so adding one without documenting it fails here"""
  policy = root / "src/mullion/mullion_router_policy.hpp"
  if builder != "name" or not policy.is_file():
    return []
  body = policy.read_text(errors="replace")
  at = body.find("name(Api api)")
  return ROUTER_API.findall(body[at:at + 400]) if at >= 0 else []


def documented_in(conf_text):
  return set(DOCUMENTED.findall(conf_text))


def check(root):
  """the reasons this tree's configuration names do not agree, as a list of messages"""
  src = root / "src"
  conf_path = root / "mullion.conf"
  if not src.is_dir() or not conf_path.is_file():
    return [f"{root}: no src/ or no mullion.conf, so nothing was measured"]
  conf = conf_path.read_text(errors="replace")
  read = names_in_source(root)
  listed = documented_in(conf)
  reasons = []
  for name in sorted(read - listed):
    if name in LEGACY_FALLBACKS:
      falls_back_from = LEGACY_FALLBACKS[name]
      if name not in conf:
        reasons.append(f"{name}: read as a fallback, and the alias is not named anywhere in mullion.conf")
      elif falls_back_from not in listed:
        reasons.append(f"{name}: the option it falls back from ({falls_back_from}) is not documented either")
      continue
    reasons.append(f"{name}: read by src/ with getOption, and written nowhere in mullion.conf")
  for name in sorted(listed - read):
    reasons.append(f"{name}: documented in mullion.conf, and no getOption call in src/ asks for it")
  return reasons


def fixtures():
  """a tree with exactly one error in each direction, to show the check reports both"""
  reasons = []
  root = pathlib.Path(tempfile.mkdtemp())
  (root / "src").mkdir(parents=True)
  (root / "src/x.cpp").write_text(
      'auto a = Config::getInstance().getOption<bool>("thing.onlyInCode", false);\n'
  )
  (root / "mullion.conf").write_text("# thing.onlyInConf = 0\n")
  found = check(root)
  if not any("thing.onlyInCode" in r and "nowhere" in r for r in found):
    reasons.append(f"a name read in code and undocumented was not reported: {found}")
  if not any("thing.onlyInConf" in r and "no getOption" in r for r in found):
    reasons.append(f"a documented name nothing reads was not reported: {found}")

  # and a tree where the two sides agree must say nothing, or the fixture above proves nothing
  (root / "src/x.cpp").write_text(
      'auto a = Config::getInstance().getOption<bool>("thing.onlyInCode", false);\n'
      'auto b = Config::getInstance().getOption<int>("thing.onlyInConf", 0);\n'
  )
  (root / "mullion.conf").write_text("# thing.onlyInCode = False\n# thing.onlyInConf = 0\n")
  if check(root):
    reasons.append(f"a tree that agrees with itself was reported: {check(root)}")
  return reasons


def main():
  root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
  reasons = check(root) + fixtures()
  # what was compared, so a pass reads as a measurement rather than a silence
  read = names_in_source(root)
  listed = documented_in((root / "mullion.conf").read_text(errors="replace"))
  print(
      f"config keys: {len(read)} read in src/, {len(listed)} documented in mullion.conf, "
      f"{len(LEGACY_FALLBACKS)} legacy fallback named in prose"
  )
  for reason in reasons:
    print(f"wrong: {reason}")
  # five holds: the two directions over this tree, and the three the fixtures need to show they can fail
  checks = 5
  print(f"{'failed' if reasons else 'passed'}: {len(reasons) or checks} {'wrong results' if reasons else 'config-name checks'}")
  return 1 if reasons else 0


if __name__ == "__main__":
  sys.exit(main())
