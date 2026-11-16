# Contributing

Mullion takes bug reports, results from games and code. Its code follows [CODE.md](docs/CODE.md), and
[TESTING.md](docs/TESTING.md) describes how to build and run the tests. Both apply to every contributor, maintainers
included.

Mullion is a separate project from [DXMT](https://github.com/3Shain/dxmt), which it is built on. Problems with Mullion
are reported here, not to DXMT. Mullion does not forward contributions to DXMT.

## 1. Reporting a problem

Each problem gets one [issue](https://github.com/dttdrv/mullion/issues), opened after checking that it has not been
reported already. Two symptoms can share an issue until it is clear whether they have the same cause. A report gives:

- the game or application, its version and where it came from (Steam, another store, a demo)
- the Mullion [release](https://github.com/dttdrv/mullion/releases) or commit
- the Mac's model and chip, and the macOS version
- the Wine or CrossOver version, and how Mullion was [installed](https://github.com/dttdrv/mullion/wiki/Install)
- the settings that apply: game settings, launch options,
  [Mullion's options](https://github.com/dttdrv/mullion/wiki/Settings) and environment variables
- what the reporter did, what they expected, what happened, and whether it happens every time
- the logs, written with [`MULLION_LOG_PATH`](https://github.com/dttdrv/mullion/wiki/Settings), attached as files
- a screenshot for a rendering problem, and the crash report for a crash
- the last release that worked, if one did

What the reporter could not find out is written as "unknown". A report keeps what was seen apart from what may have
caused it, and marks a guess as a guess: a wrong cause stated as fact sends the investigation the wrong way. A report
drafted with an AI tool is welcome once the reporter has checked each statement in it against what happened.

A performance report also gives the place in the game, the resolution and settings, whether shaders were still
compiling, how frame rate or frame time was measured, and the comparison it makes, measured the same way on the same
Mac.

Reports of games that work are welcome as well, in
[Game reports](https://github.com/dttdrv/mullion/discussions/categories/game-reports).

## 2. Preparing a change

**2.1 Discuss a larger change first.** New features, changes to the architecture, new dependencies and changes to
what Mullion supports start as an [issue](https://github.com/dttdrv/mullion/issues), before the work does. A bug fix
can go straight to a [pull request](https://github.com/dttdrv/mullion/pulls). Agreeing on the direction first saves
work that would not be accepted.

**2.2 Make the change complete.** A change starts from [`main`](https://github.com/dttdrv/mullion/tree/main). It
includes everything [CODE.md 1.2](docs/CODE.md#1-design) says it reaches, and its tests with their registration in the
test directory's `meson.build`. Unrelated cleanup goes in a pull request of its own
([CODE.md 7.1](docs/CODE.md#7-form)).

**2.3 Make every commit build and work.** Pull requests are merged with their commits as they are, so each commit is
one logical change that builds and works on its own. A fix and its test go in the same commit. Commits such as "fix
review" or "try again" are folded into the commits they correct before review, for example with
[`git rebase --autosquash`](https://git-scm.com/docs/git-rebase).

The size of a change is set by what a reviewer can check, not by a line count. Independent parts are separate pull
requests.

## 3. Showing that it works

**3.1 Show the test failing, then passing.** A change in behaviour carries a test
([CODE.md 6.1](docs/CODE.md#6-tests)). The author shows it failing for the reason the change addresses, on the code
without the change or with the repaired behaviour deliberately broken, and then passing with the change. A test only
ever seen passing has not shown that it detects the defect.

**3.2 Run the suite on what is submitted.** The new test and the existing tests of every path the change reaches run
on a build with assertions enabled (Meson's `debugoptimized` or `debug`
[build type](https://mesonbuild.com/Builtin-options.html)), under Metal's API validation, as every GPU family the
change can reach: `tests/run.py --build <build> --wine <wine> --validate <tests>` runs the named tests as the Mac's
own GPU, as Apple9, Apple8 and Apple7, and as those three again with Direct3D 12 kept off Metal 4 (`--family` names
others, or fewer). New tests, and tests of timing, threading or the order of work, run more than once, since one pass
does not show that a result is stable: `--repeat <n>` runs each test that many times, and a test passes only when it
gives the same passing answer every time.

**3.3 Test the final revision.** The evidence covers the exact revision submitted, built from that source, with the
tests loading the libraries that build produced. After a rebase, a split or any further edit, the checks it affects
run again. The pull request gives the commit, the build options, the Mac and macOS version, the Wine distribution and
version, the commands and their results.

**3.4 Report everything that did not pass.** Failures, skipped tests and checks that could not run are listed
separately, and none of them counts as a pass. Failures from every attempt are included, also those that passed when
run again: with `--jobs`, a test that failed while others ran beside it runs once more alone, and the run lists each
test that passed only then. A failure counts as already known only when it matches an entry of the list of known
failures it was run against, and the report names that list and the failure's cause: `tests/known.txt` has a line for
each, with the GPU family, the test, what the test said and the cause, and an entry matches by what the test said,
with its count and the digest of its wrong results. The run fails on a failure or skip the list does not have, and on
a listed test that passes. A change that fixes a known failure removes its entry. The runner's exit status alone does
not show that every test passed.

**3.5 Measure a speed change.** [CODE.md 5.1](docs/CODE.md#5-performance) says what to measure. The pull request
gives the conditions, the number of runs and their spread, and shows that the output did not change:
`tests/perf/perf_probe.py run` writes every value its scenes printed for a build into a file, and
`tests/perf/perf_probe.py compare` sets two such files side by side.

**3.6 Without the right Mac, open a draft.** A contributor without access to a Mac, or to the GPU family a change
affects, opens the pull request as a draft and lists the checks that are missing. It can be reviewed, and leaves draft
once someone has run them. `MULLION_GPU_FAMILY` ([CODE.md 1.5](docs/CODE.md#1-design)) runs the fallback paths of
older GPU families on a newer Mac by masking selected capability queries. Other queries still describe the Mac's own
GPU, and nothing changes its scheduling, compiler or timing, so a change for one GPU's own behaviour still needs
that GPU.

A change to documentation alone needs none of this, only a reader's check that it is correct.

## 4. Pull requests

**4.1 Explain the change in its description.** The title reads like a commit subject ([section 5](#5-commit-messages)).
The description opens with what the change does and why, then the cause and any design decision that is not obvious,
the contract behind it with a link to its source, and the issue it relates to. A reviewer should not need any other
conversation to follow it.

**4.2 Give the evidence.** The description gives what [section 3](#3-showing-that-it-works) asks for: the commands,
the failing result before the fix and the passing one after it, and what was not run. Long logs go in attachments.

**4.3 Keep the description true.** A pull request stays a draft while work remains. When the change is revised, its
title and description are updated to match it, and what no longer applies is removed.

## 5. Commit messages

The subject is `area: what is now true`. The area is the part of Mullion the commit changes, as in the
[history](https://github.com/dttdrv/mullion/commits/main): `d3d12`, `d3d11`, `dxgi`, `airconv`, `winemetal`, `tests`,
`install`, `readme`. After the colon, the subject states the behaviour the commit gives, in the present tense and
starting lowercase, as in
[this commit](https://github.com/dttdrv/mullion/commit/77e472908f790bd9b02e0af5b3dab6ed579344a5):

```
d3d12: a sampler feedback encode clears before it records

the clear and the record were dispatches of one pass, which Metal runs unordered, so a late clear could erase bits
a record had set in a coarser region.
```

The body explains what the subject cannot: the cause, why this change addresses it, the specification it follows and
any measurement. It reads on its own, without GitHub. A commit whose subject says everything needs no body.

## 6. Review

**6.1 The author reviews first.** The author reads the whole diff before asking for review, and answers questions
directly. Each revision says what changed, and the checks it affects run again.

**6.2 Reviewers read the code, not only the diff.** A review reads the code the change touches and checks, in this
order, that it is correct, handles failure, holds under load, is tested, is fast and is no larger than it has to be.
A finding that blocks a change names where, and the case that fails, the evidence that is missing or the rule it
breaks. Passing tests alone do not show that a change is correct, and an objection without a case is not a finding.

**6.3 Say what is required.** A reviewer marks what must change, what is a question and what is a suggestion. The
author may disagree, with evidence. A reviewer who cannot judge part of a change says so, and asks someone who can.

**6.4 Approval covers what was reviewed.** A change made after approval is reviewed again. A maintainer merges, and
may ask for a change to be split, return it to draft or close it with a reason. When a pull request waits without an
answer, one reminder is welcome.

## 7. AI tools

Changes written with AI tools are welcome, under the same rules as any other. The contributor who submits one is
responsible for it: they understand every line, have checked every claim it makes, and answer review themselves. An
agent does not open issues or pull requests, or post comments, unless a person has reviewed and authorized each one.
A review written by an AI tool is advice to a reviewer, never an approval.

## 8. Licensing

Contributions are licensed under the
[GNU Lesser General Public License, version 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) or later
(see [LICENSE](LICENSE)). A contributor submits only work they have the right to contribute, and keeps the license
notices of the code they change or take from elsewhere.
