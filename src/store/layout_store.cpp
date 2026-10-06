#include "layout_store.h"

#include <Preferences.h>

static const char *NVS_NAMESPACE = "glowscreen";
static const char *NVS_KEY_BLOB = "layout";
static const char *NVS_KEY_MAGIC = "magic";

// Signature + version du format binaire. ⚠️ À INCRÉMENTER À CHAQUE
// modification de `struct Layout` : un cache d'un ancien format serait sinon
// relu de travers et l'écran afficherait n'importe quoi. Le contrôle
// `len == sizeof(Layout)` rattrape un changement de taille, mais PAS un
// changement de champs à taille constante.
//   v02 : champ `mode` dans LayoutButton (contrat v1.3)
//   v03 : pages, grille, icône numérique, 32 boutons (contrat v2.0)
//   v04 : mode `view`, `readonly`, `id` du schéma 3 (contrat v3.0)
//
// ⚠️ v04 n'est PAS qu'une question de format : les `id` d'un même bouton
// DIFFÈRENT d'un schéma à l'autre (le schéma 2 retire les tuiles `view` et
// renumérote). Un cache v04 relu par une 2.2.0 après retour arrière enverrait
// à `press` des `id` du schéma 3 interprétés en schéma 2 : le bouton d'à côté
// — portail compris. Dans l'autre sens, un cache v03 relu par la 2.3.0 aurait
// le même défaut. Les deux firmwares rejettent donc le cache de l'autre et
// attendent leur premier `layout` : un écran vide deux secondes, jamais un
// écran qui ment.
//   v05 : libellés, titres et nom sur 49 octets (24 caractères accentués, 2.3.1).
//         La taille change, ce qui suffirait (len == sizeof) ; le magic suit
//         quand même la règle : il est la seule garde des changements à taille
//         constante, et une règle à exceptions finit oubliée.
static const uint32_t STORE_MAGIC = 0x47533205;  // "GS2" + version 05

// ⚠️ `Layout` fait ~1,6 ko et la tâche Arduino n'a que 8 ko de PILE.
// Ces tampons de travail sont donc STATIQUES. En variables locales, le seul
// chemin `save() -> load()` en empilerait deux, sous un appelant qui tient
// déjà la mise en page fraîche — près de 5 ko de pile, sous HTTPClient.
// En v1.4 la structure faisait 316 octets et la question ne se posait pas.
static Layout s_loadScratch;
static Layout s_saveScratch;

// Compare deux mises en page en IGNORANT l'état des boutons.
//
// `state` est volatil : il change quand une lampe s'allume, y compris depuis
// l'application Jeedom ou un interrupteur mural. Le comparer ferait réécrire
// la NVS à chaque allumage, pour une information que le cache n'a de toute
// façon pas vocation à conserver — il ne sert qu'à peupler l'écran pendant les
// deux secondes précédant le premier `layout`. La flash a un nombre de cycles
// d'écriture fini.
static bool sameExceptState(const Layout &a, const Layout &b) {
  if (a.version != b.version || a.poll != b.poll || a.schema != b.schema ||
      a.count != b.count || a.pageCount != b.pageCount || a.cols != b.cols ||
      a.rows != b.rows || a.swipe != b.swipe || a.clock != b.clock ||
      a.readonly != b.readonly) {
    return false;
  }
  if (strcmp(a.device, b.device) != 0 || strcmp(a.name, b.name) != 0) return false;

  for (uint8_t i = 0; i < a.pageCount && i < LAYOUT_MAX_PAGES; i++) {
    if (a.pages[i].parent != b.pages[i].parent ||
        a.pages[i].hasParent != b.pages[i].hasParent ||
        strcmp(a.pages[i].title, b.pages[i].title) != 0) {
      return false;
    }
  }
  for (uint8_t i = 0; i < a.count && i < LAYOUT_MAX_BUTTONS; i++) {
    const LayoutButton &x = a.buttons[i];
    const LayoutButton &y = b.buttons[i];
    if (x.id != y.id || x.color != y.color || x.mode != y.mode || x.icon != y.icon ||
        x.page != y.page || x.slot != y.slot || x.target != y.target ||
        strcmp(x.label, y.label) != 0) {
      return false;
    }
  }
  return true;
}

bool LayoutStore::begin() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) {
    // L'espace de noms n'existe pas encore : on le crée en écriture.
    if (!p.begin(NVS_NAMESPACE, false)) {
      Serial.println("[nvs] ouverture impossible");
      return false;
    }
  }
  p.end();
  _ready = true;
  Serial.printf("[nvs] prêt (blob de %u octets par mise en page)\n",
                (unsigned)sizeof(Layout));
  return true;
}

bool LayoutStore::load(Layout &out, const char *expectedDevice) {
  if (!_ready) return false;
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, true)) return false;

  bool ok = false;
  const uint32_t magic = p.getUInt(NVS_KEY_MAGIC, 0);
  const size_t len = p.getBytesLength(NVS_KEY_BLOB);
  if (magic == STORE_MAGIC && len == sizeof(Layout)) {
    Layout &tmp = s_loadScratch;
    if (p.getBytes(NVS_KEY_BLOB, &tmp, sizeof(tmp)) == sizeof(tmp) &&
        tmp.count <= LAYOUT_MAX_BUTTONS && tmp.pageCount <= LAYOUT_MAX_PAGES &&
        tmp.pageCount >= 1 && tmp.cols >= 3 && tmp.cols <= LAYOUT_MAX_COLS &&
        tmp.rows >= 2 && tmp.rows <= LAYOUT_MAX_ROWS) {
      tmp.device[sizeof(tmp.device) - 1] = '\0';
      if (expectedDevice && strcmp(tmp.device, expectedDevice) != 0) {
        // Boutons appartenant à un autre écran : on refuse de les afficher.
        // Cas réel : carte reflashée à partir d'une NVS clonée.
        Serial.printf("[nvs] cache d'un autre écran (%s != %s), effacé\n",
                      tmp.device, expectedDevice);
        p.end();
        clear();
        return false;
      }
      out = tmp;
      ok = true;
      Serial.printf("[nvs] cache relu : \"%s\" v%ld, %u boutons sur %u page(s)\n",
                    out.name, (long)out.version, out.count, out.pageCount);
    }
  } else if (len != 0 || magic != 0) {
    // Cas normal après une montée de version : le cache v02 est ignoré, le
    // premier `layout` le remplacera. Rien de grave, mais il faut le dire.
    Serial.printf("[nvs] cache présent mais incompatible (magic %08lx, %u octets), ignoré\n",
                  (unsigned long)magic, (unsigned)len);
  }
  p.end();
  return ok;
}

bool LayoutStore::save(const Layout &in) {
  if (!_ready) return false;

  // Évite une écriture inutile si seule la STRUCTURE compte (voir
  // sameExceptState : l'état des boutons est volontairement exclu).
  Layout &current = s_saveScratch;
  if (load(current, in.device) && sameExceptState(current, in)) return true;

  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return false;
  const size_t written = p.putBytes(NVS_KEY_BLOB, &in, sizeof(in));
  p.putUInt(NVS_KEY_MAGIC, STORE_MAGIC);
  p.end();

  const bool ok = (written == sizeof(in));
  Serial.printf("[nvs] cache %s (v%ld, %u octets)\n", ok ? "écrit" : "NON écrit",
                (long)in.version, (unsigned)sizeof(in));
  return ok;
}

void LayoutStore::clear() {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return;
  p.clear();
  p.end();
  Serial.println("[nvs] cache effacé");
}
