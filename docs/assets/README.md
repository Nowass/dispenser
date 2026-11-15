# Asset provenance and attribution

Every image or icon that ships in the app or lives in this repository is listed here with its
origin and license status. An asset whose distribution rights are not confirmed must not be
published (public repository or released APK) until the owner confirms them.

| Asset | Used in | Origin | License / status |
|---|---|---|---|
| `docs/assets/app-icon-reference.png` | Design reference only (not in the APK) | AI-generated specifically for the Dispenser project | **Confirmed** by the project owner for inclusion in the repository (2026-11-15) |
| Launcher icon: `android/app/src/main/res/drawable/ic_launcher_foreground.xml`, `ic_launcher_background.xml`, `ic_launcher_monochrome.xml`, `mipmap-anydpi/ic_launcher*.xml`, splash icon | APK | Vector drawings created for the Dispenser project, modelled on the AI-generated reference artwork above | **Confirmed** by the project owner for inclusion in the repository and the Android application (2026-11-15); distributed under the project license (Apache-2.0) |
| `ic_battery.xml` ("battery"), `ic_reservoir.xml` ("local_drink"), `ic_water_drop.xml` ("water_drop"), History glyph in `DispenserIcons.kt` | APK | Google Material Icons (path data), sourced separately | Apache-2.0, Copyright Google LLC – attributed in `NOTICE`, source comments and the in-app license list |
| Material icons via `androidx.compose.material:material-icons-core` | APK | Library dependency | Apache-2.0 – listed in the generated third-party licenses |

## Adding new assets

Record each new image, icon or font here before committing it: origin (own work, AI-generated
for the project, third-party with source URL), license and any required attribution. Third-party
material that requires attribution must also be added to `NOTICE` and, if it ships in the APK,
to the in-app license information.
