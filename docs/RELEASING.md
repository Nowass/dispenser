# Releasing the Dispenser Android app (GitHub Releases)

Status: preparation only (A1.4b). **No release has been published yet.** The repository
`Nowass/dispenser` is currently **private**; it is planned to become public after a separate
security audit. Until then the in-app update check reports that release information is
unavailable (unauthenticated GitHub API returns 404). No credentials are ever embedded in the app.

## Versioning

- `versionName`: **semantic versioning** (`MAJOR.MINOR.PATCH`, optional pre-release such as
  `0.5.0-beta.1`). The app compares versions by SemVer precedence, never as strings.
- `versionCode`: **strictly increasing integer for every published APK**, including pre-releases.
  It is assigned manually per release and never reused. No formula derived from the version
  number (a beta and the following stable release must get different, increasing codes).
  Example sequence: `0.5.0-beta.1` → 5, `0.5.0-beta.2` → 6, `0.5.0` → 7.
- The current development version stays `0.4.0` / `4` until the first release is prepared.
- `applicationId` stays `cz.nowass.dispenser`.

## Tags and release assets

- Git tag: `v` + versionName, e.g. `v0.5.0` or `v0.5.0-beta.1`.
- Mark pre-releases as **pre-release** on GitHub *and* use a SemVer pre-release tag.
- Attach exactly one signed APK, e.g. `dispenser-0.5.0.apk`
  (content type `application/vnd.android.package-archive`).
- Release notes: copy the matching section of `CHANGELOG.md` (move items from
  "Nevydáno – vývojové milníky" under the new version heading first).

## Update channels (installed build decides; never switched automatically)

- **Stable build** (versionName without pre-release): considers stable releases only.
- **Beta build** (pre-release versionName): considers beta *and* stable releases and offers the
  newest newer one.
- The app shows the newest newer release that has an APK asset and opens its official
  `https://github.com/...` release page in the browser. It never downloads, installs or executes
  anything itself.

## Signing (required before the first release)

- Production APKs must be signed with a **dedicated release keystore stored outside Git**
  (e.g. in a password manager / secure backup). Android only installs an update over an
  existing installation if it is signed with the same key, so losing the key blocks updates.
- Never commit keystores or their passwords; `android/.gitignore` excludes `*.jks`,
  `*.keystore` and `keystore.properties`. Provide signing configuration locally or via CI
  secrets when release signing is set up (not part of A1.4b; no key has been generated).

## After the repository becomes public

1. Set `dispenser.github.sourceLinkEnabled=true` in `android/gradle.properties` to show
   "Zdrojový kód projektu" in About.
2. Verify the update check against a real published release.
3. Decide the application source-code license (currently undecided; no `LICENSE` file).

## Third-party licenses

After any dependency change run:

```sh
./gradlew :app:generateThirdPartyLicenses
./gradlew :app:checkThirdPartyLicenses
```

The generator reads the resolved release runtime classpath and each POM (with parent-POM
inheritance) and fails if a license cannot be determined or an attribution-requiring license
(e.g. BSD-3-Clause) has no per-library notice in `app/src/main/assets/licenses/notices/`.
