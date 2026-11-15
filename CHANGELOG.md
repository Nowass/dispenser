# Changelog

Všechny podstatné změny aplikace Dispenser se zapisují do tohoto souboru.

Formát vychází z [Keep a Changelog](https://keepachangelog.com/cs/1.1.0/) a verze se řídí
[sémantickým verzováním](https://semver.org/lang/cs/).

Zatím nebyla zveřejněna žádná verze (GitHub Releases). Položky níže shrnují interní vývojové
milníky; nejde o publikovaná vydání. Při prvním vydání se přesunou pod číslo verze.

## [Nevydáno] – vývojové milníky

### Přidáno
- Přehled s příští dávkou, odhadem zásobníku a simulovanou baterií; tmavý vzhled Material 3 (A1.1, A1.1.1).
- Kalendář s měsíčním a týdenním zobrazením a detailem vybraného dne (A1.2a).
- Editor rozvrhů: jednorázové, denní (každých N dní) a týdenní (každých N týdnů ve vybrané dny) dávkování, náhled nejbližších pěti termínů (A1.2b).
- Ukládání rozvrhů v telefonu; stav „Uloženo v telefonu“ bez předstírání synchronizace se zařízením (A1.2c).
- Plovoucí navigační lišta a přepínání obrazovek tažením.
- Simulace dávkování se simulovaným časem, výpadkem zařízení a resetem; odhad zásobníku, blokace při nedostatku, doplnění a výhled zásoby; historie událostí (A1.3).
- Nastavení s názvem akvária, ikona aplikace a úvodní obrazovka (A1.4a).
- Obrazovka O aplikaci, Co je nového, licence knihoven třetích stran a kontrola aktualizací přes GitHub Releases (A1.4b).

### Změněno
- Ruční uložení v editoru je zakázané, dokud formulář obsahuje chyby nebo se nezměnil.
- Varování na nízkou zásobu (navržená hranice 20 %) se zobrazuje jednou za epizodu.

### Opraveno
- Opakování s velmi dlouhým intervalem nyní najde budoucí termíny.
- Pole množství přijímá pouze běžná desetinná čísla (např. 12,5).
- Jednorázovou dávku nelze uložit v čase, který kvůli přechodu na letní čas neexistuje.
