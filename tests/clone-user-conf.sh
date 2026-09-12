#!/bin/bash
# Fabrique tests/user.conf : une COPIE de la configuration reelle de
# l'utilisateur (~/.tmux.conf), pour que les tests s'executent dans les memes
# conditions que sa session (plugins TPM, theme tmux2k, ses raccourcis).
#
#   bash tests/clone-user-conf.sh          # rafraichit le clone
#
# Le fichier de l'utilisateur n'est JAMAIS modifie : on ne fait que le lire.
# Deux retouches, uniquement pour que les tests ne debordent pas sur ses
# affaires :
#   * "prefixe + r" rechargeait ~/.tmux.conf : pointe sur le clone ;
#   * tmux-resurrect ecrivait dans ~/.tmux/resurrect (le fuzz tape
#     prefixe+Ctrl+s) : redirige vers le repertoire de travail des tests.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HOME/.tmux.conf}
DST=$HERE/user.conf
WORK=${TMUXV_WORK:-/tmp/tmuxv-tests}

[ -r "$SRC" ] || { echo "configuration introuvable : $SRC" >&2; exit 1; }

{
	echo "# CLONE de $SRC"
	echo "# Fabrique le $(date '+%F %T') par tests/clone-user-conf.sh."
	echo "# NE PAS EDITER : relancer le script pour le mettre a jour."
	echo
	sed "s|source-file ~/.tmux.conf|source-file $DST|g" "$SRC"
	echo
	echo "# ── Ajouts pour les tests uniquement ─────────────────────────────"
	echo "set -g @resurrect-dir '$WORK/resurrect'"
} > "$DST"

echo "clone : $DST ($(wc -l < "$DST") lignes)"
diff -B <(sed "s|source-file ~/.tmux.conf|source-file $DST|g" "$SRC") \
     <(sed -e '1,4d' -e '/── Ajouts pour les tests/,$d' "$DST") \
     >/dev/null && echo "contenu identique a l'original (hors en-tete et ajouts)"
