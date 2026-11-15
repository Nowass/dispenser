# Asset provenance and attribution

Every image or icon that ships in the app or lives in this repository is listed here with its
origin and license status. **An asset whose distribution rights are not confirmed must not be
published** (public repository or released APK) until the owner confirms them.

| Asset | Used in | Origin | License / status |
|---|---|---|---|
| `docs/assets/app-icon-reference.png` | Design reference only (not in the APK) | Supplied by the project owner (A1.4a); original source not recorded | **Rights unconfirmed** – owner must confirm before the repository is made public |
| Launcher icon: `android/app/src/main/res/drawable/ic_launcher_foreground.xml`, `ic_launcher_background.xml`, `ic_launcher_monochrome.xml`, `mipmap-anydpi/ic_launcher*.xml`, splash icon | APK | Vector drawings created for this project, modelled on `app-icon-reference.png` | Apache-2.0 (project), **subject to the reference image's rights** – owner must confirm before the first public APK |
| `ic_battery.xml` ("battery"), `ic_reservoir.xml` ("local_drink"), `ic_water_drop.xml` ("water_drop"), History glyph in `DispenserIcons.kt` | APK | Google Material Icons (path data) | Apache-2.0, Copyright Google LLC – attributed in `NOTICE`, source comments and the in-app license list |
| Material icons via `androidx.compose.material:material-icons-core` | APK | Library dependency | Apache-2.0 – listed in the generated third-party licenses |

## Before going public

- [ ] Owner confirms the origin and publication rights of `app-icon-reference.png` (and thus of
      the launcher icon derived from it), and records the source in the table above.
- If the rights cannot be confirmed: replace the reference image and redesign the launcher icon
  before publishing. The image is already part of the Git history; removing it from history
  would require a history rewrite, which needs a separate explicit decision.
