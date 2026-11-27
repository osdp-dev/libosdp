---
lastUpdated: 2026-11-26
description: "How to read a LibOSDP version — what a bare version, a git-position suffix, and the -dev pre-release marker each mean, how release lines and maintenance branches work, what a version bump promises, and how to verify a release is authentic."
---
# Releases and Versioning

Everything you need to answer three questions: *what am I running*, *what
changes if I upgrade*, and *is this build genuine*.

## Reading a version string

`osdp_get_version()` returns the human-readable version, and it is deliberately
unambiguous — **a bare `X.Y.Z` is the only thing that is a release.** Anything
else tells you where the build sits relative to one.

| What you see | What it means |
|---|---|
| `4.0.1` | Exactly the released tag `v4.0.1`. This is a release build. |
| `4.0.1+5.gabc1234` | Five commits past `v4.0.1`, at commit `abc1234`. A development build of the 4.0 line — later than 4.0.1, earlier than whatever ships next. |
| `4.1.0-dev.37+ga1b2c3d` | Inside a **pre-release cycle** for 4.1.0, 37 commits in. **4.1.0 does not exist yet.** |

The companion `osdp_get_source_info()` returns the branch the build came from,
plus the tag or commit — e.g. `master (abc1234)`. A trailing `+` there means the
source tree had uncommitted changes when it was built, so the commit hash alone
does not fully describe it.

:::warning A `-dev` version is not that version
`4.1.0-dev.…` does **not** mean "4.1.0, near enough". It means the maintainers
have opened a development cycle *aiming at* 4.1.0: the number names the version
being worked toward, and anything in it may still change or be withdrawn before
it ships. Everything documented for 4.1.0 is a promise about the eventual
release, not about the `-dev` build in your hands.

If you are shipping a product, track a release, not a cycle.
:::

## What a version change promises

LibOSDP versions are `MAJOR.MINOR.PATCH`:

| Bump | What it means for you |
|---|---|
| **Patch** — `4.0.1` → `4.0.2` | Fixes only. Drop-in: no API changes, no rebuild of your code required beyond relinking. |
| **Minor** — `4.0.x` → `4.1.0` | New capability, added compatibly. Existing calls keep working; code written against the older minor still compiles. |
| **Major** — `4.x.y` → `5.0.0` | May remove or change existing API. Read the changelog before upgrading. |

The shared library's `SOVERSION` is the **major** version, so `libosdp.so.4`
serves every 4.x release: minor and patch upgrades are ABI-compatible and a
major upgrade requires relinking.

:::info LibOSDP's version is not the OSDP version
`4.0.0` is the version of *this library*. The version of the *protocol* it
speaks is set by the IEC 60839-11-5 / SIA OSDP specification, and each PD
advertises its own protocol capability on the wire. A new LibOSDP major does
not imply a new OSDP revision, and vice versa.
:::

## Release lines and branches

`master` is the **development tip** — it carries whatever is next, and builds
off it always report a git position or a `-dev` marker, never a bare release
number.

Each release line gets a long-lived **maintenance branch**, forked from the
tag and named `release_v<major>.<minor>` (e.g. `release_v3.2`). Patch releases
for that line are cut on the branch while `master` moves on:

```text
master        ──●──●──●─────●(v4.0.0)──●──●──●─────●(v4.1.0) ...
                             │
release_v4.0                 └──●──●(v4.0.1)──●(v4.0.2) ...
```

Fixes typically land on the oldest affected line and are merged forward, so a
patch release on `release_v4.0` also reaches `master`.

**What to track:**

- **Shipping a product** — pin a **tag** (`v4.0.1`). It is signed, immutable, and
  matches a published changelog entry.
- **Staying current on a line** — follow the `release_v<major>.<minor>` branch to
  pick up patch fixes without taking new features.
- **Following development** — use `master` if you want the newest work, and expect
  version strings with a git position or `-dev` marker. Don't ship from it.

## Where releases are published

Every release is tagged `v<version>` in the
[libosdp repository](https://github.com/osdp-dev/libosdp) and published to:

- **GitHub Releases** — release notes come from that version's changelog entry,
  `CHANGELOG/RELEASE-v<version>.md` in the repository.
- **PyPI** — `pip install libosdp`.
- **NuGet** — the `LibOSDP` package, for .NET (`dotnet add package LibOSDP`).
- **ESP Component Registry** — the `sidcha/libosdp` component, for ESP-IDF.
- **PlatformIO Registry** — the `LibOSDP` library, for Arduino/PlatformIO builds.

## Verifying a release is genuine

Every release tag is **GPG-signed**, and the signature is re-verified by CI before
any artifact is built or published — an unsigned or mismatched tag produces no
release. You can check the same thing yourself:

```sh
curl -sS https://github.com/sidcha.gpg | gpg --import
git verify-tag --raw v4.0.1 2>&1 | grep -E 'GOODSIG|VALIDSIG'
```

That key URL serves every key the maintainer's account publishes, including
retired ones, so a good signature alone is not enough. The tag is genuine when
the output has a `GOODSIG` line (gpg prints `EXPKEYSIG` or `REVKEYSIG`
instead for an expired or revoked key) and the last field of the `VALIDSIG`
line, the certifying primary key, is the release key:

```text
D8861B9C6B9C4284D980D6016804DFEE281234B2
```

Because the tag covers the commit, this also confirms the entire source tree at
that point.

---

*Maintainers cutting a release: the flow lives in `scripts/make_release.py` in
the repository; run it with `--help`.*
