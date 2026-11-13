# Contributing to 3ds-d2

Thank you for trying 3ds-d2. You don't need to write code to help: playing the game and telling us what happened is one of the most useful things you can do. Every crash report, profiler run, screenshot and "this felt slow here" helps. All kinds of feedback are welcome, small or large.

There are three ways to help:

1. **[Send a report from the console](#reporting-a-problem-from-the-console)** when something goes wrong. It takes a minute and needs no account.
2. **[Run the profiler](#profiling-help-make-the-game-faster)** in places where the game feels slow. This is how we find what to make faster next.
3. **[Open a GitHub issue](#bugs-ideas-and-feedback-on-github)** for bugs, ideas and feedback, or **[send a pull request](#pull-requests)**.

New to the project? The [README](README.md) explains what 3ds-d2 is, what you need and how to install it.

## Reporting a problem from the console

3ds-d2 has a built-in reporter that sends to the project's [Sentry](https://sentry.io). Nothing is sent until you press a button, nothing is collected while you play, and your save files and character names are never sent.

### When you're asked

- **After a crash.** If 3ds-d2 crashes (Luma3DS shows its exception screen), the next start offers that session as a report.
- **After an error** that ends the game.
- **When you ask for one.** If something looks or feels wrong, tap **report** at the bottom of the bottom screen's **INFO** tab (it lights up as **will report**; tap it again to take that back). Then keep playing as you like: the report is made after the game ends.

When the game ends, the bottom screen says *"Preparing the report. This can take a minute or two; please wait."* After a long session the logs are big, so give it a moment. It isn't frozen.

### On the report screen

The top screen shows what would be sent. Before sending, you can add:

- **Y: a description.** In a few words, what happened and what you were doing. For example: *"Crashed when I opened the stash in Lut Gholein"*, or *"The Cold Plains felt choppy with many monsters"*. This helps more than anything else.
- **R: a topic.** If your report is about a GitHub issue, put its number here, for example `#24`. Reports with the same topic are grouped together in Sentry, and we can match them to the issue.
- **L: screenshots.** Up to 4 pairs, each with the top and bottom screen. See below.

Then:

- **A** sends it over Wi-Fi. Sentry's **event ID** is shown. Note it down if you also want to write about it on GitHub.
- **X** marks it as not to be sent.
- **B** keeps it for later. Reports you don't send wait on the SD card, and the next start shows the list (**X** there sends all of them). Hold **SELECT** while 3ds-d2 starts to open the list at any time.

### Screenshots

A picture of a glitch says more than a paragraph. There are two ways to take one:

- In the game, press **L + R + Select**. This saves a pair: the top screen and the bottom screen.
- From the Luma3DS menu (**L + Down + Select**), choose *Take screenshot*. This also works while the game is suspended in the HOME Menu.

Take the screenshot when the problem is on screen, then attach it with **L** on the report screen. Screenshots are only sent if you choose them.

The [Problem reports](README.md#problem-reports) section of the README lists exactly what a report contains.

## Profiling: help make the game faster

Many of 3ds-d2's speed-ups came from profiler runs that players sent in. A profiler run records the game in detail for a while, and its report shows us where the time goes. We especially need runs from places we haven't measured yet: **Acts III, IV and V**, big fights, and anywhere the game feels slow.

### How to do a run

1. In the game, open the bottom screen's **INFO** tab and tap **profiler run**. A checklist of measurements opens.
2. Check what to record. If you're not sure, **frame timing** on its own is cheap and very useful. The others:

   | Measurement | What it records | Cost |
   | --- | --- | --- |
   | frame timing | every frame's timing | small |
   | guest code profile | where the game's own code spends its time | about 12% slower while it runs |
   | sprite trace | the sprite cache's traffic | SD card writes |
   | file-read trace | every read of a game file | SD card writes |
   | SD card benchmark | how fast your SD card reads | a 1 s pause at the start |

3. Tap **start the run** (**back to INFO** returns to the diagnostics), and play the part that feels slow: a fight, a crowded area, an area's first load. While the run is on, INFO's button is lit as **profiling**.
4. To stop, tap **profiling**, then **stop the run**. Several minutes is plenty. (A run still on when you quit is stopped for you.)
5. Quit the game from its menu (*Save and Exit Game*, then *Exit Diablo II*). The run is then offered as a report on its own; you don't need to tap **report** as well.
6. In the report, use **Y** to say **where you were and what you did**: *"Act III, Kurast Docks to Spider Forest, about 20 monsters, felt slow when they all attacked"*. Then send it.

### Tips for a useful run

- **Keep it focused.** One area or situation per run is easier to read than an hour of everything.
- **Say what you felt.** "Smooth" is useful too: it tells us an area is fine.
- **Try the options.** INFO's **options** button opens a page with the port's switches (drawing on core 2, the 50 Hz screen, the frame lock, the native sprite decoder, reading tiles ahead). They change at once, even during a run, so a run with one switched off and back on lets us compare. Mention it in the description if you do.

## Bugs, ideas and feedback on GitHub

[GitHub issues](https://github.com/exsilium/3ds-d2/issues) are the place for bug reports, ideas and any feedback: controls that feel awkward, text that's hard to read, things that confused you. You don't need to be sure it's a bug.

A helpful bug report has:

- **What happened**, and what you expected instead.
- **How to make it happen**, if you know: where you were, what you pressed.
- **Your build:** the CIA's name, for example `3ds-d2-0.4.0-dev-c8ddbb858e2e.cia`. The last part is the build ID. Or pre-build Release version.
- **Your console:** for example New 3DS XL, or New 2DS XL. If you're playing in an emulator, say which one (Azahar, for example).
- **Sentry's event ID**, if you sent a report about it.
- **Screenshots**, if you have them.

Please don't attach Diablo II's own files (`Game.exe`, the `.mpq` files) or anything copied from them. 3ds-d2 only works with your own copy of the game.

## Pull requests

Pull requests are welcome, from typo fixes to new features. Please open them against the **`development`** branch. `master` only receives releases, merged from `development`.

- **Talk first about big changes.** An issue first is a good idea for anything large, so we can agree on the approach.
- **Keep a pull request to one thing.** Mention the issue it fixes (`Closes #<n>`).
- **Build and test with the project's containers:** `scripts/docker.sh package-cia <game-dir>` builds the CIA, and `scripts/docker.sh test host` (and the other suites listed in [docs/build-and-run.md](docs/build-and-run.md)) runs the tests. Say in the pull request what you tested, and on what: a console, Azahar, or the tests only.
- **Update the docs** that describe what you changed: controls in [docs/controls.md](docs/controls.md) and the README, and the HELP page for button changes.
- **Never commit the game's files.** No Diablo II files or assets go in the repository.

### Project conventions

These apply to everyone who works on the code, by hand or with tools.

**Branches and issues**
- Start feature branches from `origin/development`, and open pull requests with `development` as the base. `master` changes only through the release merge from `development`.
- **Only the maintainer closes issues.** `Closes #<n>` in a pull request is welcome, but GitHub acts on it only on the default branch (`master`), so issues stay open after a merge into `development` until the maintainer closes them. Don't close issues yourself.

**Commits and pull requests**
- They carry the author's own words. No tool-generated footers ("Generated with …") or co-author trailers for AI assistants.
- When copying the format of an older commit or pull request, leave such lines out.

**Building**
- Build and test only through `scripts/docker.sh`. It runs the containers as your user, with networking off.
  - Files made by a plain `docker run` (as root), or by git on the host inside the backend's work tree, can't be rewritten by later builds.
  - [docs/build-and-run.md](docs/build-and-run.md#the-backends-work-tree) explains the backend's work tree and how to change its patch series.
- Build CIAs for testing with the game directory: `scripts/docker.sh package-cia <game-dir>`. Without it, the CIA gets drawn placeholder art instead of the game's own icon, banner and tune.
  - Check that the output says `art:` from the game, not `drawn`.

**Console options and measurements**
- **A switch or measurement meant for a console run must be reachable on the bottom screen:** the OPTIONS page, or the profiler run's checklist. Console testers don't edit `settings.txt` by hand, so a `settings.txt`-only key is fine for Azahar runs (`measure-smoothness.sh --setting`) but gives nothing on hardware.
- **Post measurements as a comment on their issue:** the build ID, the Sentry short ID (`3DS-D2-…`), the tables and the caveats. The issue is where the work's evidence is kept. A section in [docs/hardware-test.md](docs/hardware-test.md) is welcome as well, not instead.

**Game code and third-party code**
- **Our code and our tools may be distributed; Blizzard's code may not.**
  - This includes Game.exe translated or decompiled by our tools: tools such as the planned ahead-of-time recompiler (#111) run on the user's own Game.exe, on the user's machine, and their output only ever goes into the user's own CIA.
  - Natives are written from an understanding of the game's behaviour and cite addresses, not copied code.
- **Third-party code is pinned at a commit, listed in [third_party/deps.lock](third_party/deps.lock), and described in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).**
  - It keeps its licence, and patches on top of it share the licence of the files they change.
  - If GPL code is linked into the app, the CIA as a whole is distributed under the GPL, while our own files stay BSD-2. Code used only by tests doesn't affect the CIA.

### Useful reading

- [docs/build-and-run.md](docs/build-and-run.md): building, the test suites, running on a console and in Azahar, debugging, and reading console reports.
- [docs/architecture.md](docs/architecture.md): how the pieces fit together, and where the project is heading.
- [docs/natives.md](docs/natives.md): replacing a game routine with native code, checking it and measuring it.
- [docs/controls.md](docs/controls.md): every button, and the bottom screen.
- [docs/hardware-test.md](docs/hardware-test.md): building the CIA, testing it on a New 3DS, and the measurements so far.
- [docs/memory-budget.md](docs/memory-budget.md): where the 124 MiB go.
- [docs/compatibility.md](docs/compatibility.md): what has been tested, on which build and where.

## How we work together

3ds-d2 is a hobby project, and everyone here is giving their free time. Please:

- **Be kind and patient,** with other contributors and with first-time reporters. There are no silly questions and no reports too small.
- **Keep it about the work.** Criticise code and ideas, not people.
- **Assume good intent,** and ask when something is unclear.
- **Respect the game's copyright.** Don't share, or ask for, Diablo II's files, keys or cracks. Everyone plays with their own copy.

Discussions that break these may be edited, locked or removed.

## Thank you

Every report, run, issue and pull request makes 3ds-d2 better for everyone who plays it. Thank you for taking the time.
