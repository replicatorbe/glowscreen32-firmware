// Interface graphique : bandeau d'état, grille tactile paginée, bandeau de
// navigation. Écran 320x240 en rotation paysage.
//
// Principe de rafraîchissement : rien n'est redessiné en boucle. Chaque
// modification (état d'un bouton, appui, bandeau, minute qui change) ne
// repeint que sa zone — l'écran n'a pas de framebuffer en RAM (aucune PSRAM).
//
// Les tuiles sont composées dans un SPRITE de la taille d'une case puis
// poussées d'un bloc : c'est ce qui supprime le clignotement. Si l'allocation
// échoue (heap fragmentée), on retombe sur un tracé direct — dégradé mais
// fonctionnel. Voir ui/theme.h pour le calcul de taille.
//
// ⚠️ Tous les index de bouton manipulés ici sont des index GLOBAUX dans
// `Layout::buttons`, pas des positions dans la grille affichée. Un bouton
// d'une page non affichée existe, a un état, et peut être rafraîchi sans rien
// redessiner.
#pragma once

#include <TFT_eSPI.h>

#include "../model/layout.h"
#include "theme.h"

// État de la liaison, pour le bandeau.
enum class UiLink : uint8_t {
  Unknown,  // pas encore d'information
  Ok,       // dernière requête réussie
  Error,    // serveur joignable mais en erreur, ou injoignable
  Offline,  // pas de Wi-Fi
};

// Ce que le doigt a touché. La grille ne décide rien : elle rapporte, et
// main.cpp arbitre (un `nav` se traite localement, un bouton part en réseau).
struct UiHit {
  enum Kind : uint8_t {
    None,    // zone morte
    Button,  // `index` = index global dans Layout::buttons
    Back,    // chevron de retour du bandeau bas
    Page,    // pastille de page : `index` = numéro de page
  } kind = None;
  int8_t index = -1;
};

// Ce que l'écran plein ou la superposition en cours masque de la grille.
//
// ⚠️ Pourquoi un état explicite : jusqu'en v2.1, un rafraîchissement du
// bandeau (perte du Wi-Fi, résultat d'un ping) repeignait sa moitié droite
// PAR-DESSUS l'écran d'enrôlement. Désormais chaque écran plein se déclare en
// prenant la main, et la grille cesse de dessiner : le modèle continue d'être
// mis à jour (états, page, bandeau), seul le tracé est suspendu, et uncover()
// repeint tout d'un coup.
enum class UiCover : uint8_t {
  None,  // grille et bandeau visibles
  Grid,  // superposition sur la grille (message) : seul le bandeau se repeint
  All,   // écran plein (enrôlement, réglages, OTA…) : rien ne se repeint
};

// Contenu de l'écran Réglages (menu local, contrat v2.2).
struct SettingsInfo {
  const char *mac = "";
  char        ip[16] = {0};
  char        ssid[33] = {0};
  int         rssi = 0;
  bool        wifiUp = false;
  const char *fw = "";
  const char *partition = "";
  uint32_t    uptimeS = 0;
  uint32_t    heap = 0;
  uint32_t    block = 0;
  const char *host = "";
  const char *api = "";       // état de la liaison Jeedom, en clair
  bool        portalOpen = false;
  bool        confirmReboot = false;  // second appui attendu sur « Redémarrer »
};

// Contenu de l'écran du portail de secours.
struct PortalInfo {
  const char *apName = "";
  const char *apPass = "";
  const char *apIp = "";
  const char *targetSsid = "";
  bool        staUp = false;
  uint8_t     clients = 0;
  const char *trial = "";  // issue du dernier essai de réglages (vide : aucun)
};

// Zones tactiles des écrans pleins.
enum class ScreenHit : uint8_t {
  None,
  Back,        // bandeau haut : retour
  Calibrate,   // Réglages
  Portal,      // Réglages
  Reboot,      // Réglages
  ClosePortal, // Portail
};

class ButtonGrid {
 public:
  void begin(TFT_eSPI *tft);

  // Installe la mise en page à afficher. `layout` doit rester valide (le
  // pointeur est conservé) et redessine tout. `keepPage` : rester sur la
  // page courante si elle existe encore — un changement de configuration
  // fait dans Jeedom ne doit pas renvoyer à l'accueil quelqu'un qui est sur
  // une sous-page. Sinon, retour à la page d'accueil.
  // `values` : valeurs volatiles des tuiles `view` (schéma 3), indexées comme
  // les boutons ; nullptr = « — » partout.
  void setLayout(const Layout *layout, const TileValues *values, bool keepPage = false);

  // Remplace les pointeurs par une mise en page de MÊME STRUCTURE, sans rien
  // redessiner : échange du double tampon quand seuls des états ont bougé.
  void adoptLayout(const Layout *layout, const TileValues *values) {
    _layout = layout;
    _values = values;
  }

  // Redessine tout l'écran (bandeau + grille + navigation).
  void drawAll();

  // Masquage par un écran plein ou une superposition (voir UiCover).
  UiCover cover() const { return _cover; }
  void setCover(UiCover c) { _cover = c; }
  // Fin du masquage : repeint tout.
  void uncover();

  // --- Navigation --------------------------------------------------------
  // Purement locale : elle ne dépend JAMAIS du réseau, et fonctionne donc sur
  // le seul cache NVS, écran coupé de Jeedom (contrat v2.0).
  uint8_t page() const { return _page; }
  bool goToPage(uint8_t page);  // false si la page n'existe pas
  bool goBack();                // remonte au parent ; false s'il n'y en a pas
  bool nextPage();
  bool prevPage();

  // --- Écrans pleins -----------------------------------------------------
  // Tous passent la grille en UiCover::All : plus rien ne se repeint
  // par-dessus eux tant qu'uncover() n'a pas été appelée.
  //
  // Écran d'enrôlement : la carte n'est pas encore déclarée dans Jeedom
  // (réponse `unknown_device`). On affiche sa MAC en grand pour que
  // l'utilisateur crée l'équipement sans brancher de câble série. `ssid` :
  // réseau associé, ou nullptr hors ligne.
  void drawEnroll(const char *macPretty, const char *macRaw, const char *ssid);

  // L'OTA prend une bonne minute : l'utilisateur doit comprendre que l'écran
  // n'est pas planté.
  void drawOtaScreen(const char *fromVersion, const char *toVersion);
  void drawOtaProgress(uint8_t pct);
  void drawOtaFailure(const char *reason);

  // Menu local (appui maintenu 5 s sur le bandeau, contrat v2.2).
  void drawSettings(const SettingsInfo &s);
  void drawSettingsInfo(const SettingsInfo &s);  // seules les lignes d'information
  ScreenHit settingsHitTest(int16_t x, int16_t y) const;

  // Portail de secours : nom du point d'accès et mot de passe, en grand.
  void drawPortal(const PortalInfo &p);
  void drawPortalStatus(const PortalInfo &p);    // seule la ligne d'état
  ScreenHit portalHitTest(int16_t x, int16_t y) const;

  // Calibration tactile à l'écran : cible `i` sur `n` ; `i` == `n` est la
  // cible de CONTRÔLE, au centre.
  void drawCalibTarget(uint8_t i, uint8_t n);
  // Titre et détail COURTS : une ligne de 16 px ne tient que ~300 px.
  void drawCalibOutcome(bool saved, const char *title, const char *detail);

  // Commande `message` : par-dessus la grille (UiCover::Grid), le bandeau
  // reste vivant. Un toucher le ferme sans actionner de bouton (main.cpp).
  void drawMessage(const char *text);

  // Commande `identify` : nom, MAC, IP — de quoi trouver l'écran au mur.
  void drawIdentify(const char *name, const char *mac, const char *ip, const char *fw);

  // Écran d'attente avant redémarrage.
  void drawNotice(const char *title, const char *detail);

  void setFwVersion(const char *v);

  // --- Bandeau haut ------------------------------------------------------
  //
  // Règle imposée par le contrat v2.0 : quand tout va bien, le bandeau affiche
  // `info` et l'heure. L'état de la liaison n'apparaît QUE s'il est mauvais.
  // Un voyant « API OK » affiché 99,9 % du temps n'informe personne et occupe
  // la seule zone utile de l'écran ; l'œil cesse de le lire, donc il ne
  // remarque pas non plus le jour où il passe au rouge.
  void setWifi(bool up, int rssi = 0);
  void setApi(UiLink link, const char *detail);

  // Chaîne DÉJÀ FORMATÉE par le plugin (« 21.4 °C »), ou nullptr. La carte
  // n'interprète rien : elle ne connaît même pas la notion de température.
  void setInfo(const char *info);

  // Horloge : epoch UTC de référence + décalage local, recalés à chaque ping
  // et égrenés localement entre deux. Pas de NTP — le serveur est déjà la
  // référence de temps, et une carte sans Internet doit continuer à la donner.
  void setClock(uint32_t epochUtc, int32_t tzOffsetSec);
  // À appeler dans loop() : ne repeint que si la minute affichée a changé.
  void tickClock();

  void drawBanner();

  // Repeint UNIQUEMENT la moitié droite du bandeau (anomalie, ou info +
  // heure). Appelée à chaque ping : repeindre le bandeau entier ferait
  // clignoter le titre trente fois par heure pour rien.
  void drawStatus();

  // --- Boutons -----------------------------------------------------------
  // `index` est TOUJOURS un index global dans Layout::buttons.
  void setPressed(int8_t index);  // -1 = aucun
  int8_t pressed() const { return _pressed; }

  // Repeint une seule case, après que l'appelant a mis à jour le modèle.
  // Sans effet si le bouton n'est pas sur la page affichée.
  void refreshButton(uint8_t index);

  // Marque un bouton « en attente de confirmation » : sa pastille est dessinée
  // en creux tant que le `ping` suivant n'a pas tranché (contrat v1.3 :
  // `press` renvoie un état attendu, pas constaté). N'entraîne pas de
  // redessin : l'appelant enchaîne avec refreshButton().
  void setPending(uint8_t index, bool pending);
  bool isPending(uint8_t index) const {
    return index < LAYOUT_MAX_BUTTONS && _pending[index];
  }
  void clearPending();

  // Ce que le point écran désigne.
  UiHit hitTest(int16_t x, int16_t y) const;

 private:
  void drawGrid();
  void drawNav();
  // Titre de page. Séparé de drawStatus() pour qu'il puisse être redessiné
  // seul, quand la largeur du bloc d'état change.
  void drawTitle();
  void drawButton(uint8_t index, bool highlight);
  void drawEmptyCell(uint8_t slot);
  // Rectangle de la case `slot` dans la grille courante.
  void slotRect(uint8_t slot, int16_t &x, int16_t &y, int16_t &w, int16_t &h) const;
  // Compose une tuile dans le sprite (ou directement si le sprite manque).
  void paintTile(const LayoutButton &b, bool highlight, bool pending,
                 int16_t x, int16_t y, int16_t w, int16_t h);
  // Tuile `view` (schéma 3) : libellé + icône, VALEUR en grand, couleur du ton.
  void paintViewTile(TFT_eSPI &g, const LayoutButton &b, const TileValue *v,
                     const uint8_t *&fontSlot, int16_t ox, int16_t oy, int16_t w, int16_t h);
  // (Re)dimensionne le sprite à la taille EXACTE d'une tuile de la grille
  // courante. Appelée à chaque changement de géométrie.
  void ensureSprite();
  bool haveSprite() const { return _sprite != nullptr; }
  uint8_t cols() const { return _layout && _layout->cols ? _layout->cols : 3; }
  uint8_t rows() const { return _layout && _layout->rows ? _layout->rows : 3; }
  // Rangées RÉELLEMENT occupées sur la page affichée. La grille s'ajuste à
  // son contenu : 4 boutons sur une grille 3x3 tiennent en 2 rangées, ce qui
  // donne des tuiles de 100x91 au lieu de 100x59 — et supprime le grand
  // rectangle noir qui occupait sinon la moitié basse de l'écran.
  uint8_t rowsUsed() const;
  uint8_t slotsShown() const { return (uint8_t)(cols() * rowsUsed()); }
  // Heure locale courante, ou false si l'horloge n'a jamais été recalée.
  bool localHm(uint8_t &hh, uint8_t &mm) const;

  TFT_eSPI     *_tft = nullptr;
  TFT_eSprite  *_sprite = nullptr;
  int16_t       _spriteW = 0;  // 0 tant qu'aucun sprite n'est alloué
  int16_t       _spriteH = 0;
  const Layout *_layout = nullptr;
  const TileValues *_values = nullptr;

  uint8_t _page = 0;
  int8_t  _pressed = -1;
  bool    _pending[LAYOUT_MAX_BUTTONS] = {false};

  UiCover _cover = UiCover::None;
  bool gridShown() const { return _tft && _cover == UiCover::None; }
  bool bannerShown() const { return _tft && _cover != UiCover::All; }
  // En-tête commun des écrans pleins : bandeau, chevron de retour, titre.
  void drawScreenHeader(const char *title, bool back);

  bool   _wifiUp = false;
  int    _rssi = 0;
  UiLink _api = UiLink::Unknown;
  char   _apiLabel[20] = "--";
  char   _fwVersion[16] = {0};
  char   _info[LAYOUT_INFO_LEN] = {0};  // 16 caractères accentués

  uint32_t _clockEpoch = 0;    // epoch UTC du dernier recalage
  uint32_t _clockAtMs = 0;     // millis() correspondant
  int32_t  _tzOffset = 0;      // secondes, DST comprise
  bool     _clockSet = false;
  int16_t  _shownMinute = -1;  // hh*60+mm actuellement peint
  int16_t  _statusLeft = -1;   // bord gauche du bloc d'état, -1 = inconnu
};
