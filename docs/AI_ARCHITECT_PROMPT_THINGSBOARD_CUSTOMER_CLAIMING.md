# Przekazanie lampy klientowi w ThingsBoard PE

Zaplanuj wdrożenie procesu przekazania gotowej lampy z tenant’a do klienta
końcowego przy użyciu **ThingsBoard PE Claiming**:
<https://thingsboard.io/docs/pe/user-guide/claiming/>.

### Cel produktu

Po wyprodukowaniu i uruchomieniu urządzenie ma zostać zarejestrowane w
ThingsBoard jako urządzenie tenant’a, a następnie przekazane przez klienta na
jego konto Customer. Klient ma móc znaleźć urządzenie i przejąć je bez dostępu
do uprawnień tenant-admin. To jest transfer własności, nie początkowe
provisioning urządzenia.

### Znany stan repozytorium — zweryfikuj go w kodzie

- Firmware lampy w `hq_led_lamp` wykonuje ThingsBoard provisioning przy
  pierwszym połączeniu: używa provision device key/secret, zapisuje otrzymany
  access token i łączy się ponownie jako zarejestrowane urządzenie.
- Produkcyjny flow lampy nie uruchamia obecnie procesu przekazania urządzenia
  klientowi.
- Platformowe `tb_claim_device()` znajduje się w
  `platform/hq_platform/src/thingsboard/tb_claim.c` i publikuje żądanie
  device-side na `v1/devices/me/claim`. Sprawdź dokładnie jego kontrakt,
  callbacki i ograniczenia; nie zakładaj, że samo potwierdzenie publikacji
  oznacza przeniesienie urządzenia do Customer.
- Istnieją testy i demo claimingu na poziomie platformy. Demo nie jest
  produkcyjnym flow lampy.
- `FIRMWARE_PRODUCTION_READINESS_PLAN.md`, sekcja
  `Lifecycle E2E: claiming + Customer`, opisuje test server-side claimingu
  przez `claimingData` i endpoint Customer. To test lifecycle, nie kompletna
  integracja produktu.
- Szczegóły lampy i jej provisioning’u są udokumentowane w
  `docs/THINGSBOARD_USER_MANUAL.md`.
- Lampa nie ma oczywistego ekranu ani lokalnego interfejsu, który mógłby
  bezpiecznie pokazać klientowi jednorazowy sekret. Portal Wi-Fi istnieje, ale
  nie zakładaj, że powinien ujawniać sekret claimingu bez analizy ryzyka.
- Istniejący dostęp MQTT lampy i jej access token nie są sekretem claimingu.
  Nie łącz tych poświadczeń ani ich ról.
- Lokalne środowisko testowe może używać ThingsBoard CE; docelowa funkcja
  produktu ma korzystać z PE. Sprawdź różnice edition/version i nie traktuj
  wyniku testu CE jako dowodu działania PE.

Najpierw zweryfikuj powyższe informacje w aktualnym checkoutcie i wskaż
konkretne pliki/moduły. Jeśli rzeczywisty stan repozytorium jest inny,
skoryguj założenia w planie.

### Granice repozytoriów i automatycznego review/commitu

- `hq_led_lamp` jest repozytorium produktu, a
  `hq_led_lamp/platform/hq_platform` jest osobnym, zagnieżdżonym repozytorium
  Git (submodule). Zmiana pliku platformy nie jest zwykłą zmianą pliku lampy.
- Sprawdź `/home/dima/projects/hq_workspace/configs/hq_led_lamp.yml` i sposób
  działania `/home/dima/projects/hq_workspace/dev-automation/dev_automation/git_scope.py`
  oraz `controller.py`. W
  dostarczonej konfiguracji brak jawnej listy `git.repositories`; w takim
  ustawieniu automat obejmuje tylko repozytorium `hq_led_lamp` i nie wykrywa,
  nie recenzuje ani nie commit’uje zmian wewnątrz `hq_platform`.
- Jeśli plan wymaga zmian w `hq_platform`, uwzględnij jako warunek przed
  implementacją konfigurację `git.repositories`, tak aby automat jawnie
  obejmował zarówno repozytorium `hq_led_lamp`, jak i osobne repozytorium
  `hq_platform`. Zadań modyfikujących platformę nie wolno opisywać tak, jakby
  ich commit objąłby je automatycznie przez repozytorium lampy.
- AI reviewer ma recenzować zmiany dla wszystkich skonfigurowanych
  repozytoriów. Reviewer **nie wykonuje git commit**. Po akceptacji review
  commity wykonuje pythonowy kontroler `dev-automation` w każdym
  skonfigurowanym repozytorium. Dla zmian w `hq_led_lamp` commit wykonuje ten
  sam kontroler aplikacji Python; dla zmian w `hq_platform` również musi go
  wykonać kontroler, ale tylko jeśli platforma jest jawnie skonfigurowanym
  targetem.
- Zaplanuj zadania tak, by każdy etap obejmujący zmianę repozytorium miał
  jednoznaczne ścieżki plików, testy dla tego repozytorium i kryteria review.
  Jeśli jedno zadanie zmienia oba repozytoria, jawnie wymień pliki i testy z
  obu; rozważ rozdzielenie go, gdy pozwala to na niezależne, poprawne review.
- Nie planuj ręcznego commitu przez implementera ani reviewera i nie zakładaj,
  że sam wpis TASK lub zmiana wskaźnika submodule zastępuje commit w
  `hq_platform`.

### Zakres planowania

1. Opisz aktorów i sekwencję: provisioning pod tenant’em, przygotowanie
   możliwości claimingu, bezpieczne przekazanie danych klientowi, claim przez
   Customer User i zachowanie urządzenia po transferze.
2. Porównaj server-side i device-side claiming dla tej lampy. Zarekomenduj
   wariant, uzasadnij go brakiem ekranu oraz opisz warunki, które mogłyby
   zmienić rekomendację. Nie zakładaj, że device-side MQTT helper jest
   wymagany dla server-side flow.
3. Określ, który komponent generuje sekret claimingu, gdzie i jak długo jest
   przechowywany, jak jest powiązany z konkretnym urządzeniem, jak trafia do
   klienta oraz jak jest unieważniany. Rozważ jednorazowość, TTL, ponowienie,
   wygaśnięcie, ponowne wydanie, błędny claim, logowanie i ochronę przed
   ujawnieniem sekretu. Nie proponuj sekretu wspólnego dla urządzeń, sekretu
   zaszytego na stałe w firmware ani umieszczania go w telemetryce.
4. Ustal granice odpowiedzialności firmware, ThingsBoard PE, ewentualnego
   backendu/portalu oraz procesu produkcyjnego. Dla każdego komponentu
   określ wymagane API/protokół, uprawnienia i dane wejściowe/wyjściowe.
5. Zweryfikuj w dokumentacji PE właściwe ustawienia claimingu, semantykę
   `claimingData`/device-side claim, endpoint Customer oraz zachowanie
   Customer User. Wypisz założoną wersję ThingsBoard i wszystkie kwestie
   zależne od wersji/edition, których nie da się potwierdzić z repozytorium.
6. Oceń skutki po claimie: widoczność urządzenia i jego telemetrii/atrybutów,
   dostęp do RPC i OTA, dalsze połączenie MQTT istniejącego urządzenia,
   provisioning przy restarcie lub utracie poświadczeń, reclaim/zwrot do
   tenant’a oraz reset fabryczny. Nie zakładaj, że claim zmienia MQTT access
   token; potwierdź zachowanie dla docelowej wersji PE.
7. Określ, czy potrzebna jest zmiana firmware. Jeśli tak, wskaż minimalną
   odpowiedzialność firmware i uzasadnij ją. Nie dodawaj lokalnego UI ani
   zmiany Wi-Fi provisioning, jeśli nie są konieczne do bezpiecznego
   przekazania sekretu.

### Wymagania dla planu

- Uwzględnij zadanie zapisujące w dokumentacji projektu potwierdzony przepływ,
  wybraną metodę claimingu, granice zaufania, cykl życia sekretu oraz wersję i
  edition ThingsBoard. Odróżnij zweryfikowane fakty od nierozstrzygniętych
  założeń.
- Jeśli proponujesz zmiany w `hq_platform`, uwzględnij konieczność jawnego
  skonfigurowania `hq_led_lamp` i `hq_platform` jako dwóch targetów
  `git.repositories` przed uruchomieniem implementacji. Konfiguracja znajduje
  się poza repozytorium produktu i może wymagać osobnego przygotowania przez
  właściciela środowiska; nie zakładaj, że automat sam ją zmieni i zatwierdzi.
- Zaplanuj review wszystkich skonfigurowanych repozytoriów. Reviewer zatwierdza
  zmiany, ale nie wykonuje commitów. Po akceptacji commit w każdym repozytorium
  wykonuje Pythonowy kontroler `dev-automation`; `hq_platform` będzie
  commitowany tylko wtedy, gdy jest jawnie skonfigurowanym targetem. Nie
  zakładaj, że commit wskaźnika submodule zastępuje commit zmian wewnątrz
  `hq_platform`.
- Rozdziel zakres według rzeczywistych odpowiedzialności: firmware
  `hq_led_lamp`, `hq_platform`, backend/automatyzacja, konfiguracja PE,
  dokumentacja i testy. Nie proponuj zmian komponentu bez uzasadnienia.
- Uwzględnij testy integracyjne na docelowym ThingsBoard PE z Customer User,
  sekret błędny/wygasły, ponowienia, reboot lampy, dostęp do RPC/OTA, reclaim,
  regresje provisioningu i kontrolę, że sekret nie trafia do logów. Zaznacz
  testy wymagające PE lub fizycznego urządzenia jako wymagające tego
  środowiska; nie przedstawiaj ich jako już zaliczonych.
- Określ bezpieczną kolejność wdrożenia, rollback i zachowanie już
  provisionowanych lamp.

### Ograniczenia produktu

- Nie wykonuj operacji na produkcyjnym ThingsBoard ani na urządzeniu.
- Nie wypisuj, nie kopiuj i nie utrwalaj tokenów, haseł, kluczy API ani
  sekretów z konfiguracji, logów lub środowiska. W przykładach używaj
  wyłącznie jawnych placeholderów.
- Nie traktuj provisioning device key/secret ani MQTT access token jako
  claiming secret.
- Nie rekomenduj automatycznego przekazywania urządzenia wyłącznie na
  podstawie dostępności `tb_claim_device()`. Wyjaśnij, kto inicjuje i kto
  kończy transfer własności.
- Nie rozszerzaj zakresu o ogólne refaktoryzacje, zmianę modelu lampy ani
  niepowiązane funkcje.
- Przy każdym twierdzeniu zależnym od ThingsBoard wskaż źródło: dokumentację
  PE, konkretny endpoint/konfigurację albo zweryfikowany kod i wersję.