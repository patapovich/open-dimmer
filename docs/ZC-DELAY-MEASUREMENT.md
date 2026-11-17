# Nollakohtailmaisimen viiveen mittaus (valinnainen)

Mitattava asia: kuinka paljon PA4:n reuna (nollakohtailmaisin) myöhästyy verkkojännitteen todellisesta
nollakohdasta. Se vaikuttaa vain aivan himmeimpään tasoon (johtoaika alkaa reunasta, ei todellisesta
nollasta). Lamppu toimii nyt hyvin minimissä 6 %, joten mittaus on vain tiedoksi.

## Turvallisuus

- Levy ei ole erotettu verkosta. Mitään johtoa, probea, skooppia tai läppäriä ei kytketä levyyn
  verkon ollessa päällä.
- Mittaus tehdään kokonaan erotettuna: valotransistori katsoo moduulin LEDiä, ja verkon
  vertailu tulee erillisestä muuntajasta. Kumpikaan ei kosketa levyä.
- Valotransistori ei saa koskea levyyn eikä jännitteisiin osiin. Pidä se kotelon ulkopuolella
  LEDin kohdalla, tai teippaa se muovikoteloon LED-reiän päälle.

## Tarvikkeet

- Pieni verkkomuuntaja, jossa on vaihtojännitelähtö (esim. 9–12 V AC -pistokemuuntaja). Ei
  tasajännitteistä virtalähdettä.
- Valotransistori (esim. BPW85 tai vastaava) ja 1 kΩ vastus, 3–5 V paristo tai skoopin oma
  lähtö.
- Kaksikanavainen skooppi. Maadoitettu skooppi on tässä sallittu, koska mikään ei kytkeydy
  levyyn.

## Firmis

LED seuraa PA4:ää vain diagnostiikkaversiossa (`make DIAG=1`, versio 2.0.14). Se flashataan
SWD:llä verkko irti (probe kiinni vain jännitteettömänä):

```
cd ~/projects/zdms16-py32/fw && make DIAG=1 flash-all
```

Mittauksen jälkeen ZTU päivittää MCU:n itse takaisin normaaliin versioon (2.1.x on uudempi kuin
2.0.14), joten paluuseen ei tarvita SWD:tä.

## Kytkentä

1. Muuntaja samaan pistorasiaan kuin moduuli. Skoopin kanava 1 muuntajan toisiolle.
2. Valotransistori: kollektori vastuksen kautta +3…5 V:iin, emitteri maahan, skoopin kanava 2
   kollektorille. Valo LEDistä vetää kanavan 2 alas.
3. Triggaus kanavan 1 nousevasta nollaläpäisystä.

## Luenta

- Viive = aika kanavan 1 nollaläpäisystä kanavan 2 reunaan. Katso molemmat suunnat (nouseva ja
  laskeva puolijakso), koska ilmaisin on epäsymmetrinen (PA4 korkea 10,08 ms, matala
  10,02 ms).
- Vähennä mittausketjun omat viiveet:
  - muuntajan vaihesiirto tyhjäkäynnillä (yleensä alle 0,1–0,3 ms; tarkista vertaamalla kahta
    eri muuntajaa, jos mahdollista);
  - valotransistorin nousu- ja laskuaika (1 kΩ:lla noin 10–20 µs);
  - MCU:n keskeytysviive (noin 2 µs).
- Tulos kirjataan tiedostoon `~/projects/zdms16-py32-analysis.md`. Jos viive on suuri (yli noin
  0,3 ms), johtoajan alkua voidaan aikaistaa firmiksessä vastaavasti.
