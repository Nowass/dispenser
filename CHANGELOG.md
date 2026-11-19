# Changelog

Všechny podstatné změny aplikace Dispenser se zapisují do tohoto souboru.

Formát vychází z [Keep a Changelog](https://keepachangelog.com/cs/1.1.0/) a verze se řídí
[sémantickým verzováním](https://semver.org/lang/cs/).

## [Nevydáno]

## [0.5.0-beta.2] – 2026-11-19

Druhá beta verze aplikace pro Android. Aplikace dál pracuje v režimu DEMO a vaše data z verze
0.5.0-beta.1 zůstávají zachována. Bluetooth připojení je ve vývoji a bude zpřístupněno po ověření
na skutečném zařízení.

### Změněno
- Aktualizace z verze 0.5.0-beta.1 instalací přes stávající aplikaci: název akvária, rozvrhy, simulace a historie zůstávají beze změny.
- Režim DEMO funguje stejně jako dosud.
- Příprava na připojení k dávkovači přes Bluetooth (zabezpečené spojení, uložení přístupových klíčů v telefonu, párování a pozvánky dalších telefonů). Ve vydané verzi je vypnutá; v Nastavení zůstává informace „Připojení přes Bluetooth bude dostupné později.“
- Obrazovky jsou připravené na zobrazení skutečných údajů ze zařízení; v režimu DEMO se nic nemění.

### Zabezpečení a soukromí
- Aplikace nově uvádí oprávnění Bluetooth (na Androidu 12 a novějším „Zařízení v okolí“, na starších verzích Bluetooth) a fotoaparát (pro pozdější načítání QR kódů). Tato verze o ně nikdy nežádá a režim DEMO je nepoužívá.
- Licence nových knihoven (CameraX, ZXing, Noise-C, libsodium) jsou v obrazovce O aplikaci.

### Známá omezení
- Režim DEMO: zařízení zatím nelze připojit; zásobník, baterie a historie jsou simulované.
- Bluetooth připojení zatím nebylo ověřeno na skutečném zařízení.
- Verze pro 32bitové procesory x86 se už nevydává (týká se jen starých emulátorů); podporované jsou procesory ARM (32 i 64 bitů) a x86_64.

## [0.5.0-beta.1] – 2026-11-15

První beta verze aplikace pro Android. Aplikace zatím pracuje v režimu DEMO: připojení
k dávkovači přes Bluetooth ještě není k dispozici, údaje o zásobníku, baterii a historie
pocházejí ze simulace. Rozvrhy jsou skutečná data uložená v telefonu.

### Přidáno
- Přehled s příští dávkou, odhadem zásobníku a simulovanou baterií v tmavém vzhledu Material 3.
- Kalendář s měsíčním a týdenním zobrazením a detailem vybraného dne.
- Editor rozvrhů: jednorázové, denní (každých N dní) a týdenní (každých N týdnů ve vybrané dny) dávkování s náhledem nejbližších pěti termínů. Uložení je možné jen u platného formuláře; množství se zadává jako běžné desetinné číslo (např. 12,5).
- Správné chování při přechodu na letní a zimní čas: neexistující čas se přeskočí, opakovaný čas proběhne jednou a jednorázovou dávku nelze naplánovat na čas, který neexistuje.
- Ukládání rozvrhů v telefonu se stavem „Uloženo v telefonu“; aplikace nepředstírá synchronizaci se zařízením.
- Nová instalace začíná s prázdným seznamem rozvrhů; rozvrhy si vytvoříte sami.
- Plovoucí navigační lišta a přepínání obrazovek tažením.
- Simulace dávkování se simulovaným časem, výpadkem zařízení a resetem; odhad zásobníku, blokace při nedostatku, doplnění, výhled zásoby a jednorázové varování při nízké zásobě (navržená hranice 20 %).
- Historie simulovaných událostí.
- Nastavení s názvem akvária, ikona aplikace a úvodní obrazovka.
- Obrazovka O aplikaci s přehledem změn, licencemi knihoven třetích stran a kontrolou aktualizací přes GitHub Releases.
- Záloha a obnovení konfigurace (rozvrhy a název akvária, volitelně simulace a historie) do souboru JSON přes systémový výběr souborů; obnovení s potvrzením, kontrolou souboru a bezpečným návratem při chybě.
- Aplikace je vydávána pod licencí Apache License 2.0; text licence je dostupný v obrazovce O aplikaci.

### Zabezpečení a soukromí
- Automatická záloha Androidu (Google účet) a přenos do nového telefonu jsou pro data aplikace vypnuté; data se přenášejí jen ruční zálohou, kterou sami uložíte.
- Aplikace neobsahuje analytiku ani telemetrii; síť používá jen pro kontrolu aktualizací spuštěnou na vyžádání.

### Známá omezení
- Režim DEMO: zařízení zatím nelze připojit; zásobník, baterie a historie jsou simulované.
