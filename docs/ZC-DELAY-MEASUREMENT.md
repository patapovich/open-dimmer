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
ZTU tarjoaa sille sisältämäänsä uudempaa 2.1.x-versiota, ja diagnostiikkaversio ottaa sen
vastaan muutaman sekunnin kuluttua verkon kytkemisestä. Mittaus ei siis ehdi alkaa.

Kunnes firmis osaa kieltäytyä päivityksestä, käännä diagnostiikkaversio versiotavulla, joka on
vähintään ZTU:n sisältämä versio. 0x9B (2.1.11) riittää, kun ZTU sisältää 2.1.11:n tai
vanhemman. Käytä omaa build-hakemistoa, koska pelkkä käännösasetusten muutos ei käännä vanhoja
objektitiedostoja uudelleen. Flashaus SWD:llä verkko irti (probe kiinni vain jännitteettömänä),
fw-kloonin hakemistossa:

```
make DIAG=1 VERSION_BYTE=0x9B BUILD=build-diag-9b flash-all
```

Zigbee2MQTT näyttää silloin `mcu_version` 2.1.11 own. Diagnostiikkaversion tunnistaa
DP:istä 110..123, jotka tulevat 10 sekunnin välein.

Paluu normaaliin versioon: `make flash-all` SWD:llä tai ZTU-päivitys, joka sisältää uudemman
version kuin diagnostiikkaversion (ZTU tarjoaa sen itse).

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
