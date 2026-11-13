<!--
What the change does and why, then the cause, any design decision that is not obvious, and the contract behind it,
with a link to its source. The issue it relates to, and any pull request it depends on.
CONTRIBUTING.md sections 3 and 4 say what the evidence needs:
https://github.com/dttdrv/mullion/blob/main/CONTRIBUTING.md#3-showing-that-it-works
-->

<!--
How it was tested. The evidence covers the final revision submitted, names the build options, and identifies the
libraries loaded from its build. Checks that later edits affect run again.
The results name the Mac and its GPU, the macOS version, the Wine distribution and version, the ABI, the shader
front end and any capability overrides.
For a behaviour change: the same test fails for the defect without the fix, or with the repaired behaviour
deliberately broken, and then passes with the fix.
The tests of every path the change reaches, with assertions and Metal's API validation: passes, failures from every
attempt, skips, and checks that could not run. New tests, and tests of timing, threading or order, run repeatedly.
A known failure names its entry, the revision of its list and its cause. The commands are tests/run.py's, with
--validate, --family and --repeat as they were given, and the report is what it printed.
A documentation change gives the reading and link checks done instead.
-->

<!--
For a speed change: the work removed, the same source measured with and without the change over the whole path it
affects, CPU submission and GPU time included, the conditions, the number of runs and their spread, and that the
output is unchanged. tests/perf/perf_probe.py compare prints such a report from two runs.
-->

<!--
What was not run, and what remains. A pull request stays a draft while work or required checks remain.
-->
