#include "fw_version.h"

// Marqueur de version gravé dans le binaire, dérivé de GLOWSCREEN_FW_VERSION.
//
// Deux protections, contre deux risques distincts :
//
//  1. `__attribute__((used))` empêche le COMPILATEUR d'éliminer un symbole
//     qu'il croit inutilisé.
//  2. La compilation se fait avec `-fdata-sections` et l'édition de liens avec
//     `--gc-sections` : l'ÉDITEUR DE LIENS peut encore supprimer une section
//     que personne ne référence, et `used` ne l'en empêche pas. La garantie
//     vient donc d'une référence réelle à l'exécution — `setup()` l'affiche
//     sur le port série au démarrage. Ce n'est pas un artifice : c'est aussi
//     la trace qui permet d'identifier un binaire depuis la console.
//
// Toute modification ici doit être suivie d'une vérification :
//     strings -a .pio/build/cyd/firmware.bin | grep GLOWSCREEN32-FW:
// qui doit renvoyer exactement une ligne.
extern "C" const char GLOW_FW_TAG[] __attribute__((used)) =
    GLOW_FW_TAG_PREFIX GLOWSCREEN_FW_VERSION;
