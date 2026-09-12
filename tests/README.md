# Banc de test tmuxv

Tests visuels et fonctionnels du fork, sans intervention humaine : un serveur X
sans écran (Xvfb), un xterm, des clics et des touches simulés (xdotool), des
captures comparées (ImageMagick).

## Lancer

    Xvfb :99 -screen 0 1000x650x24 &        # si pas déjà en route
    DISPLAY=:99 bash torture.sh             # les 106 tests

Variables :

    TMUXV        binaire à tester   (défaut : /home/martinien/tmux/build/tmux)
    TMUXV_WORK   répertoire de travail, captures et journal
                 (défaut : /tmp/tmuxv-tests ; résultats dans .../torture/)

Le binaire est testé **là où il est construit** ; rien n'est installé dans
`~/.local/bin`. Pour tester le paquet installé : `TMUXV=/usr/bin/tmuxv`.

## Lire les résultats

    === TOTAL: 106 tests, PASS=106 FAIL=0 CRASH=0 DETACH=0 ENDED=0 ===

  * `FAIL`   — la condition attendue est fausse ; capture `fail_T<n>.png`.
  * `CRASH`  — le serveur ne répond plus **et** `~/.tmuxv-crash.log` a grossi :
               un signal a été reçu, c'est un vrai plantage.
  * `ENDED`  — le serveur est sorti proprement (le fuzz a fermé la session).
  * `DETACH` — le serveur vit, le terminal est parti (fuzz : préfixe + d).

Seuls `FAIL` et `CRASH` sont des défauts. Croiser un `CRASH` avec
`journalctl -k | grep segfault`.

## Tester avec la vraie configuration de l'utilisateur

Les configurations minimales des tests ne montrent pas ce que l'utilisateur
voit : ses plugins TPM (tmux-resurrect, thème tmux2k), sa barre de statut, ses
raccourcis. Pour reproduire fidèlement un défaut qu'il signale :

    bash clone-user-conf.sh     # copie ~/.tmux.conf -> tests/user.conf
    DISPLAY=:99 bash userconf.sh

`~/.tmux.conf` n'est jamais modifié : le clone n'en change que deux choses, pour
que les tests ne débordent pas sur ses affaires — `préfixe + r` recharge le
clone au lieu de son fichier vivant, et `tmux-resurrect` écrit dans le
répertoire de travail des tests au lieu de `~/.tmux/resurrect` (le fuzz tape
`préfixe + Ctrl+s`).

Ce clone n'est PAS utilisé par `torture.sh` : les plugins affichent CPU, RAM et
l'heure, qui changent en permanence et casseraient les comparaisons de captures.
Le banc reste sur une configuration minimale et déterministe.

## Tests ciblés

`torture.sh` couvre l'ensemble ; les autres scripts creusent un point précis
(sélection et collage, ascenseurs, menus, poignée de redimensionnement,
gestionnaire de conversations, bus d'agents, fuite OSC...) :

    DISPLAY=:99 bash griptest.sh            # coin de redimensionnement
    DISPLAY=:99 bash holdtest.sh            # auto-répétition des ascenseurs
    python3 pastetest2.py                   # sélection, collage, mode copie
    python3 osctest.py /usr/bin/tmux        # fuite des réponses OSC (compare)

`pastetest2.py` et `osctest.py` n'ont pas besoin de X : ils injectent les
séquences (collage entre crochets, souris SGR) directement dans le pty du
client, ce qui reproduit fidèlement un terminal distant en SSH.

## Pièges appris

  * `xdotool key --window` est ignoré par xterm : passer par XTEST (donc pas de
    `--window`) et placer le pointeur dans le terminal avant de taper.
  * Attendre plus de `escape-time` après un `Escape`, sinon il est livré au
    dialogue suivant et le referme.
  * Ne jamais `pkill -f <motif>` : le motif figure dans sa propre ligne de
    commande et le shell du test se tue lui-même. Filtrer sur `pgrep -x`.
  * Chaque test doit utiliser sa propre socket `-S` : la socket `default` est
    partagée avec le tmux du système.
