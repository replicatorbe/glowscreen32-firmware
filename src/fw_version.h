// Version du firmware, compilée dans le binaire.
//
// Elle est envoyée à Jeedom en paramètre `fw` de `action=firmware` (contrat
// v1.4) : le plugin s'en sert pour décider s'il y a une mise à jour ET pour
// afficher la version qui tourne sur chaque écran du parc.
//
// ⚠️ À incrémenter à CHAQUE binaire publié, sinon le plugin ne verra pas la
// différence et un écran restera indéfiniment sur l'ancienne version.
//
// SOURCE UNIQUE DE VÉRITÉ : tout le reste en dérive, y compris le marqueur
// binaire ci-dessous.
#pragma once

#define GLOWSCREEN_FW_VERSION "2.3.1"

// --- Marqueur repérable dans le fichier .bin -------------------------------
//
// Pourquoi : le descripteur `esp_app_desc_t` de l'image (offset 0x20) n'est
// PAS le nôtre. Avec `framework = arduino` sous PlatformIO, il provient des
// bibliothèques Arduino précompilées et annonce invariablement
// « esp-idf: v4.4.7 … » / « arduino-lib-builder » — identique pour tous nos
// binaires. Le plugin ne peut donc pas s'en servir pour détecter une nouvelle
// version : l'OTA ne se déclencherait jamais.
//
// On grave donc un marqueur à nous, avec un préfixe assez distinctif pour
// qu'aucun autre littéral ne puisse y ressembler par accident. Le plugin
// cherche le motif dans le binaire téléversé et lit la version jusqu'au
// terminateur nul.
//
//     GLOWSCREEN32-FW:<version>\0
//
// Il ne doit exister qu'UNE occurrence, sinon l'extraction serait ambiguë :
// c'est pourquoi la chaîne est DÉFINIE dans fw_version.cpp et seulement
// déclarée ici. La définir dans l'en-tête en produirait une par unité de
// compilation.
#define GLOW_FW_TAG_PREFIX "GLOWSCREEN32-FW:"

extern "C" const char GLOW_FW_TAG[];
