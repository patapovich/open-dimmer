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

LED seuraa PA4:ää vain diagnostiikkaversiossa (`make DIAG=1`). Sen oletusversio on 2.0.14.
Diagnostiikkaversio kieltäytyy kaikista päivityksistä, joita ZTU sille tarjoaa, joten se pysyy
moduulissa, vaikka ZTU:n mukana olisi uudempi MCU-versio. Flashaus SWD:llä verkko irti (probe
kiinni vain jännitteettömänä), fw-kloonin hakemistossa:

```
make DIAG=1 flash-all
```

Zigbee2MQTT näyttää silloin kentässä `mcu_firmware` Open Dimmer ja kentässä `mcu_version`
2.0.14. Diagnostiikkaversion tunnistaa DP:istä 110..123, jotka tulevat 10 sekunnin välein.

Paluu normaaliin versioon onnistuu vain SWD:llä: `make flash-all`. ZTU-päivitys ei auta, koska
diagnostiikkaversio ei ota päivityksiä vastaan.

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
- Kirjaa tulos tiedostoon `docs/stock-firmware-analysis.md`. Jos viive on suuri (yli noin
  0,3 ms), johtoajan alkua voidaan aikaistaa firmiksessä vastaavasti.
