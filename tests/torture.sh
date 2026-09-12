#!/bin/bash
# torture.sh - ~100-test torture bench for the tmuxv Turbo Vision UI.
# SAFETY: every server runs on a dedicated -S socket and a throw-away HOME
# (so the settings form can never touch the real ~/.tmux.conf). Never touches
# the default socket nor -L menubar.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
OUT=$D/torture; mkdir -p "$OUT"; rm -f "$OUT"/*.png "$OUT"/results.log
THOME=$OUT/home; mkdir -p "$THOME"
SOCK=""; XP=""; W=""; CL=""
PASS=0; FAIL=0; CRASH=0; DETACH=0; ENDED=0; N=0
# tmuxv ecrit un backtrace dans ~/.tmuxv-crash.log sur SIGSEGV/BUS/FPE/ILL/ABRT.
# C'est LUI qui distingue un vrai plantage d'une fin de session provoquee par le
# fuzz (clic sur la case de fermeture, Fichier>Quitter, dernier panneau tue).
CLOG=$HOME/.tmuxv-crash.log; csz(){ stat -c %s "$CLOG" 2>/dev/null || echo 0; }
CRASHSZ=$(csz)

log(){ echo "$*" | tee -a "$OUT/results.log"; }
stop(){ [ -n "$SOCK" ] && "$TM" -S "$SOCK" kill-server 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -f "$SOCK"; SOCK=""; XP=""; }
# start [geometry] [extra conf lines...]
start(){
  stop; SOCK=/tmp/torture_$$_$RANDOM; local geo=${1:-125x38}; [ $# -gt 0 ] && shift
  { printf '%s\n' "set -g mouse on" "set -g @menu-bar on" "set -g @desktop on" "set -g status on"; [ $# -gt 0 ] && printf '%s\n' "$@"; } > "$THOME/.tmux.conf"
  # Le terminal peut ne pas s'ouvrir du premier coup quand la machine est
  # chargee : on reessaie, sinon TOUT le reste du fichier compare des captures
  # prises avant/apres un rattrapage et echoue en cascade (vu).
  local essai
  for essai in 1 2 3; do
    HOME=$THOME xterm -geometry "$geo" -fa Monospace -fs 10 -xrm '*metaSendsEscape: true' \
      -e "$TM -S $SOCK -f $THOME/.tmux.conf new-session -s t" 2>/dev/null &
    XP=$!
    for i in $(seq 1 150); do
      sleep 0.1
      "$TM" -S "$SOCK" list-clients 2>/dev/null | grep -q . && break
    done
    sleep 0.7
    kill -0 "$XP" 2>/dev/null && \
      [ -n "$("$TM" -S "$SOCK" list-clients -F x 2>/dev/null)" ] && break
    kill "$XP" 2>/dev/null; "$TM" -S "$SOCK" kill-server 2>/dev/null; sleep 0.5
  done
  CL=$("$TM" -S "$SOCK" list-clients -F '#{client_name}' 2>/dev/null | head -1)
  W=$(xdotool search --pid "$XP" 2>/dev/null | tail -1); [ -z "$W" ] && W=$(xdotool search --name xterm | tail -1)
  # exact pixel size of the top-level, to restore it faithfully after resizes.
  # Sans identifiant de fenetre, xwininfo attend indefiniment : la batterie
  # entiere restait bloquee dessus (vu). On le borne et on le saute.
  XW0=; XH0=
  [ -n "$W" ] && read -r XW0 XH0 <<<"$(timeout 5 xwininfo -id "$W" 2>/dev/null | awk '/Width:/{w=$2} /Height:/{h=$2} END{print w, h}')"
}
t(){ timeout 4 "$TM" -S "$SOCK" "$@" 2>&1; }
fmt(){ t display-message -p "$1"; }
srv(){ timeout 4 "$TM" -S "$SOCK" display-message -p ok 2>/dev/null | grep -q '^ok$'; }
alive(){ srv && { [ -z "$XP" ] || kill -0 "$XP" 2>/dev/null; }; }
shot(){ import -window root "$OUT/$1.png" 2>/dev/null; }
diffn(){ compare -metric AE "$OUT/$1.png" "$OUT/$2.png" null: 2>&1 | awk '{print int($1)}'; }
# Keys go through XTEST (no --window: xterm silently drops synthetic XSendEvent
# keys). Focus is PointerRoot on a WM-less Xvfb, so park the pointer inside the
# terminal first (30,100 = row 5, col 3: desktop area, harmless).
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.12; }
pfx(){ key ctrl+b; sleep 0.1; key "$1"; sleep 0.35; }
click(){ xdotool mousemove "$1" "$2" click 1; sleep 0.3; }
drag(){ xdotool mousemove "$1" "$2" mousedown 1; sleep 0.1; xdotool mousemove "$3" "$4"; sleep 0.1; xdotool mousemove "$5" "$6"; sleep 0.15; xdotool mouseup 1; sleep 0.35; }
wheel(){ xdotool mousemove "$1" "$2"; for i in $(seq 1 "$4"); do xdotool click "$3"; sleep 0.03; done; sleep 0.3; }
cx(){ echo $(( $1*8+4 )); }   # column -> pixel x (Monospace 10 = 8x17)
cy(){ echo $(( $1*17+11 )); } # row -> pixel y
rect(){ read -r DX DY DW DH DZ <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h} #{window_desktop_zoomed}')"; AT=1; }
eqf(){ [ "$(fmt "$1")" = "$2" ]; }
# check <name> '<condition evaluated with eval>'
check(){ N=$((N+1)); local name=$1 cond=$2
  # Le serveur muet = vrai plantage. Serveur vivant mais terminal parti =
  # simple detachement (le fuzz peut tirer prefixe+d) : on rattache, sans
  # compter de plantage.
  if ! srv; then
    shot "dead_T$N"; local now; now=$(csz)
    if [ "$now" != "$CRASHSZ" ]; then
      CRASHSZ=$now; CRASH=$((CRASH+1)); log "T$N CRASH  $name (signal, cf. $CLOG)"
    else
      ENDED=$((ENDED+1)); log "T$N ENDED  $name (serveur sorti proprement, pas de plantage)"
    fi
    start; return
  fi
  if [ -n "$XP" ] && ! kill -0 "$XP" 2>/dev/null; then
    DETACH=$((DETACH+1)); log "T$N DETACH $name (client parti, serveur vivant)"; start; return; fi
  if eval "$cond"; then PASS=$((PASS+1)); log "T$N PASS   $name"
  else FAIL=$((FAIL+1)); log "T$N FAIL   $name  [$(fmt '#{window_width}x#{window_height} mode=#{pane_in_mode} win=#{session_windows} panes=#{window_panes} rect=#{window_desktop_x},#{window_desktop_y},#{window_desktop_w}x#{window_desktop_h}')]"; shot "fail_T$N"; fi
}
# menu title pixel positions (row 0)
MX_FICHIER=40; MX_EDITION=110; MX_AFFICH=190; MX_PANNEAU=270; MX_FENETRE=343; MX_SESSION=415; MX_PARAM=498; MX_AIDE=570; MY=11
# settings form geometry at 125x38 (w=118,h=34,px=3,py=2): list rows 5..29
# (scrollbar column 119), buttons on row 33 - OK at local x 95, Annuler at 104.
F_OKX=812; F_CANX=900; F_BTNY=572; F_SBX=956; F_TRK0=113; F_TRK1=487
F_BELL_NONE_X=455; F_MOUSE_X=420
# Les LIGNES du formulaire ne sont plus codees en dur : il liste les options
# globales de session (tableaux exclus) par ordre alphabetique, @-options
# comprises - en ajouter une decale tout ce qui suit. On retrouve donc le rang
# d'une option dans `show-options -g`, dont l'ordre est le meme (arbre trie).
# Premier champ sur la ligne ecran 5 (a 125x38).
# Rang d'une option dans la liste du formulaire (meme ordre que show-options -g,
# l'arbre est trie). On NE calcule PLUS une position a l'ecran : ajouter une
# option decalait tout, et pouvait meme pousser la cible hors de la zone
# visible. On navigue au clavier : le formulaire fait defiler tout seul pour
# garder la selection visible.
form_rank(){
  t show-options -g | grep -vE '^[@a-z-]+\[' | awk -v n="$1" '{ if ($1 == n) { print NR - 1; exit } }'
}
# La fenetre a un champ de recherche, qui a le focus a l'ouverture : on tape le
# nom, la correspondance exacte vient en tete, Bas la selectionne. Plus de rang
# a calculer (sections, options de fenetre et tableaux l'auraient fausse).
form_goto(){          # amene la selection sur l'option $1
  xdotool mousemove 30 100; xdotool type --delay 15 "$1"; sleep 0.3
  xdotool key Down; sleep 0.2
}
KEYS=(a b c d e f g h i j k l m n o p q r s t u v w x y z 0 1 2 3 4 5 6 7 8 9 Return Escape Tab space Up Down Left Right Next Prior Home End F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12 comma period slash semicolon apostrophe bracketleft bracketright minus equal BackSpace Delete Insert)
rk(){ echo "${KEYS[RANDOM%${#KEYS[@]}]}"; }
# Fuzz du PREFIXE : on retire les touches qui detruisent legitimement la
# session ou le client - d/D (detach) et x (kill-pane, confirme par un 'y'
# tire au hasard juste apres). Elles rendraient le banc non deterministe
# alors qu'elles sont deja couvertes par T94 (detach) et les tests de menu.
PKEYS=(); for k in "${KEYS[@]}"; do case $k in d|D|x) ;; *) PKEYS+=("$k");; esac; done
rpk(){ echo "${PKEYS[RANDOM%${#PKEYS[@]}]}"; }

############################ A - menu bar ############################
start
shot a0
check "A1 server+client up, menu bar on"                  'eqf "#{@menu-bar}" on'
click $MX_FICHIER $MY; shot a1
check "A2 click Fichier opens a menu (screen changed)"    '[ "$(diffn a0 a1)" -gt 500 ]'
xdotool mousemove $MX_EDITION $MY; sleep 0.4; shot a2
check "A3 hover switches to Edition (changed)"            '[ "$(diffn a1 a2)" -gt 200 ]'
click $MX_EDITION $MY; sleep 0.3; shot a3
check "A4 re-click closes menu (screen restored)"         '[ "$(diffn a0 a3)" -lt 300 ]'
click $MX_AFFICH $MY; key Escape; sleep 0.4; shot a4
check "A5 Escape closes menu"                             '[ "$(diffn a0 a4)" -lt 300 ]'
click $MX_AFFICH $MY; sleep 0.3; click $((MX_AFFICH+10)) 45
check "A6 menu item Decouper horizontalement splits"      'eqf "#{window_panes}" 2'
t kill-pane -t :.1
key alt+f; sleep 0.4; shot a6
check "A7 Alt+f opens Fichier"                            '[ "$(diffn a0 a6)" -gt 500 ]'
key Escape; sleep 0.3
for x in $MX_FICHIER $MX_EDITION $MX_AFFICH $MX_PANNEAU $MX_FENETRE $MX_SESSION $MX_PARAM $MX_AIDE; do xdotool mousemove $x $MY click 1; sleep 0.08; done; key Escape; sleep 0.4
check "A8 rapid clicks on all 8 titles"                   'alive'
pfx B; pfx B; sleep 0.4; shot a8
check "A9 menu bar off/on restores screen"                '[ "$(diffn a0 a8)" -lt 300 ]'
for i in 1 2 3 4 5 6 7 8 9 10; do xdotool mousemove $MX_FICHIER $MY click 1; sleep 0.05; xdotool mousemove $MX_AIDE $MY; sleep 0.05; done; key Escape
check "A10 hover storm across bar"                        'alive'

############################ B - desktop windows ############################
start
rect; shot b0
check "B1 desktop on, rect sane"                          '[ "$DW" -gt 20 ] && [ "$DH" -gt 5 ]'
TX=$(cx $((DX+DW/2))); TY=$(cy $((AT+DY)))
drag $TX $TY $((TX+40)) $((TY+34)) $((TX+80)) $((TY+68)); rect
check "B2 drag title moves window (+10,+4)"               '[ "$DX" -ge 5 ] && [ "$DY" -ge 3 ]'
TX=$(cx $((DX+DW/2))); TY=$(cy $((AT+DY)))
drag $TX $TY 200 20 2 12; rect
check "B3 drag beyond top-left clamps to 0,0"             '[ "$DX,$DY" = "0,0" ]'
TX=$(cx $((DX+DW/2))); TY=$(cy $((AT+DY)))
drag $TX $TY 700 400 995 630; rect
check "B4 drag beyond bottom-right survives"              'alive'
t set -g @desktop off; t set -g @desktop on; sleep 0.3; rect
TX=$(cx $((DX+DW/2))); TY=$(cy $((AT+DY)))
drag $TX $TY 500 300 300 100; rect; W0=$DW; H0=$DH
GX=$(cx $((DX+DW-1))); GY=$(cy $((AT+DY+DH-1)))
drag $GX $GY $((GX+40)) $((GY+34)) $((GX+80)) $((GY+51)); rect
check "B5 grip resize grows window"                       '[ "$DW" -gt "$W0" ] && [ "$DH" -gt "$H0" ]'
GX=$(cx $((DX+DW-1))); GY=$(cy $((AT+DY+DH-1)))
drag $GX $GY $((GX-300)) $((GY-200)) $(cx $((DX+2))) $(cy $((AT+DY+1))); rect
check "B6 grip resize to tiny keeps a minimum"            '[ "$DW" -ge 8 ] && [ "$DH" -ge 3 ]'
GX=$(cx $((DX+DW-1))); GY=$(cy $((AT+DY+DH-1)))
drag $GX $GY $((GX+200)) $((GY+150)) $((GX+400)) $((GY+300)); rect
for i in $(seq 1 12); do GX=$(cx $((DX+DW-1))); GY=$(cy $((AT+DY+DH-1))); d=$(( (i%2)*2-1 )); xdotool mousemove $GX $GY mousedown 1 mousemove $((GX+d*24)) $((GY+d*17)) mouseup 1; sleep 0.12; rect; done
check "B7 12 rapid resizes: not stuck in copy-mode"       'eqf "#{pane_in_mode}" 0'
t send-keys -t t 'echo cursorcheck' Enter; sleep 0.4
check "B8 after resizes the shell still answers"          '[ "$(t capture-pane -p -t t | grep -c cursorcheck)" -ge 1 ]'
rect; ZX=$(cx $((DX+DW-4))); ZY=$(cy $((AT+DY))); X0=$DX; Y0=$DY; W0=$DW; H0=$DH
click $ZX $ZY; rect
check "B9 zoom box maximises"                             '[ "$DZ" = 1 ] && [ "$DW" -gt "$W0" ]'
ZX=$(cx $((DX+DW-4))); ZY=$(cy $((AT+DY))); click $ZX $ZY; rect
check "B10 unzoom restores exact rect"                    '[ "$DZ" = 0 ] && [ "$DX,$DY,$DW,$DH" = "$X0,$Y0,$W0,$H0" ]'
t new-window; sleep 0.3; rect
CBX=$(cx $((DX+3))); CBY=$(cy $((AT+DY)))
click $CBX $CBY; sleep 0.3
check "B11 close box kills the window"                    'eqf "#{session_windows}" 1'
rect; GX=$(cx $((DX+DW-1))); GY=$(cy $((AT+DY+DH-1)))
drag $GX $GY $(cx $((DX+10))) $((GY-100)) $(cx $((DX+9))) $(cy $((AT+DY+3))); rect
check "B12 very narrow window (no zoom box) alive"        'alive'
GX=$(cx $((DX+DW-1))); GY=$(cy $((AT+DY+DH-1)))
drag $GX $GY $((GX+200)) $GY $((GX+400)) $(cy $((AT+DY+2))); rect
check "B13 2-3 row window (no scrollbar) alive"           'alive'
t set -g @desktop off; t set -g @desktop on
for i in $(seq 1 10); do t new-window; done; sleep 0.5
check "B14 10 extra windows cascade"                      'eqf "#{session_windows}" 11'
for i in $(seq 1 11); do t next-window; sleep 0.05; done
check "B15 cycling 11 windows"                            'alive'
for i in $(seq 1 10); do t kill-window -t :$((11-i)) >/dev/null 2>&1; done

############################ C - desktop scrollbar ############################
start
t send-keys -t t 'seq 1 400' Enter; sleep 1
rect; SBX=$(cx $((DX+DW-1))); UPY=$(cy $((AT+DY+1))); DNY=$(cy $((AT+DY+DH-2))); T0=$(cy $((AT+DY+2))); T1=$(cy $((AT+DY+DH-3)))
click $SBX $UPY
check "C1 up-arrow scrolls exactly 1 line"                'eqf "#{scroll_position}" 1'
for i in $(seq 1 10); do xdotool mousemove $SBX $UPY click 1; sleep 0.06; done; sleep 0.3
check "C2 10 rapid up-arrows -> 11"                       'eqf "#{scroll_position}" 11'
drag $SBX $((T0+50)) $SBX $((T0+20)) $SBX $T0
check "C3 drag thumb to top = history_size"               '[ "$(fmt "#{scroll_position}")" = "$(fmt "#{history_size}")" ]'
drag $SBX $T0 $SBX $((T1-100)) $SBX $T1
check "C4 drag thumb to bottom returns live"              'eqf "#{pane_in_mode}" 0'
click $SBX $DNY
check "C5 down-arrow when live is a no-op"                'eqf "#{pane_in_mode}" 0'
MID=$(( (T0+T1)/2 )); click $SBX $MID; SP=$(fmt '#{scroll_position}'); HS=$(fmt '#{history_size}')
check "C6 track middle click ~ half"                      '[ "$SP" -gt $((HS/3)) ] && [ "$SP" -lt $((HS*2/3)) ]'
click $SBX $T1; sleep 0.2
wheel $(cx $((DX+10))) $(cy $((AT+DY+5))) 4 5
check "C7 wheel over content enters copy-mode"            'eqf "#{pane_in_mode}" 1'
t send-keys -t t -X cancel; sleep 0.2
t new-window; sleep 0.3; rect; SBX=$(cx $((DX+DW-1))); T0=$(cy $((AT+DY+2))); T1=$(cy $((AT+DY+DH-3)))
click $SBX $(( (T0+T1)/2 ))
check "C8 empty history: track click stays live"          'eqf "#{pane_in_mode}" 0'
t kill-window; sleep 0.3
t set -g history-limit 60000; t new-window; t send-keys -t t 'seq 1 40000' Enter; sleep 3
rect; SBX=$(cx $((DX+DW-1))); T0=$(cy $((AT+DY+2))); T1=$(cy $((AT+DY+DH-3)))
drag $SBX $((T1-17)) $SBX $(( (T0+T1)/2 )) $SBX $T0
check "C9 huge history drag to top"                       '[ "$(fmt "#{scroll_position}")" = "$(fmt "#{history_size}")" ]'
GX=$(cx $((DX+DW-1))); GY=$(cy $((AT+DY+DH-1)))
drag $GX $GY $((GX-40)) $((GY-34)) $((GX-60)) $((GY-51))
check "C10 grip resize while in copy-mode resets mode"    'eqf "#{pane_in_mode}" 0'
t kill-window

############################ D - settings form ############################
start
shot d0; t display-form -c "$CL"; sleep 0.5; shot d1
check "D1 form opens (screen changed)"                    '[ "$(diffn d0 d1)" -gt 2000 ]'
key Escape; sleep 0.8; shot d2
check "D2 Escape closes form (restored)"                  '[ "$(diffn d0 d2)" -lt 300 ]'
t display-form -c "$CL"; sleep 0.4; key Return; sleep 0.6; shot d3
check "D3 Enter flashes OK then closes"                   '[ "$(diffn d0 d3)" -lt 300 ]'
t display-form -c "$CL"; sleep 0.4; xdotool mousemove 30 100; xdotool type--delay 5 "garbage"; key Escape; sleep 0.8
check "D4 typing then Escape leaves @desktop"             'eqf "#{@desktop}" on'
t display-form -c "$CL"; sleep 0.4; xdotool mousemove 30 100; xdotool type--delay 1 "$(head -c 700 /dev/zero | tr '\0' 'x')"; sleep 0.5
check "D5 700 chars into an input (bounded)"              'alive'
for i in $(seq 1 10); do key Next; done; for i in $(seq 1 10); do key Prior; done; key ctrl+Next; key ctrl+Prior
check "D6 PgUp/PgDn x10 + Ctrl-PgUp/PgDn"                 'alive'
drag $F_SBX $F_TRK0 $F_SBX 300 $F_SBX $F_TRK1; drag $F_SBX $F_TRK1 $F_SBX 300 $F_SBX $F_TRK0
check "D7 drag form scrollbar down and up"                'alive'
wheel 500 300 5 30; wheel 500 300 4 30
check "D8 wheel x30 both ways"                            'alive'
key Escape; sleep 0.8
F_MOUSE_Y=$(form_row_y mouse)
t display-form -c "$CL"; sleep 0.4; form_goto mouse; key space; key Return; sleep 0.6
check "D9 uncheck mouse + OK applies live"                'eqf "#{mouse}" 0'
t set -g mouse on
check "D9b config written in TEST home only"              'grep -q "mouse" "$THOME/.tmux.conf"'
t display-form -c "$CL"; sleep 0.4; xdotool windowsize "$W" 480 340; sleep 0.8; shot d10
check "D10 shrink terminal while form open"               'alive'
key Escape; sleep 0.8; xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
check "D10b restore size"                                 'eqf "#{client_width}" 125'
xdotool windowsize "$W" 320 170; sleep 0.8; t display-form -c "$CL"; sleep 0.4
check "D11 form on tiny terminal: refused, alive"         'alive'
xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
t display-form -c "$CL"; sleep 0.3; t display-form -c "$CL"; sleep 0.3; key Escape; sleep 0.8; shot d12
check "D12 open form twice then Escape"                   '[ "$(diffn d0 d12)" -lt 300 ]'
F_BELL_Y=$(form_row_y bell-action)
# Espace passe au choix SUIVANT : on calcule le nombre d'appuis depuis la
# valeur courante, dans l'ordre affiche par le formulaire.
BA_ORDER="none any current other"; BA_NOW=$(t show-options -gv bell-action)
BA_N=$(awk -v o="$BA_ORDER" -v now="$BA_NOW" 'BEGIN{n=split(o,a," ");for(i=1;i<=n;i++){if(a[i]==now)c=i;if(a[i]=="none")w=i}print (w-c+n)%n}')
t display-form -c "$CL"; sleep 0.4; form_goto bell-action
for i in $(seq 1 "$BA_N"); do key space; done
key Return; sleep 0.6
check "D13 radio bell-action=none + OK"                   'eqf "#{bell-action}" none'
t set -g bell-action any
t display-form -c "$CL"; sleep 0.4; form_goto mouse; key space; click $F_CANX $F_BTNY; sleep 0.6
check "D14 Annuler discards change"                       'eqf "#{mouse}" 1'
t display-form -c "$CL"; sleep 0.4; form_goto mouse; key space
xdotool mousemove $F_OKX $F_BTNY mousedown 1; sleep 0.2; xdotool mousemove 300 300; sleep 0.1; xdotool mouseup 1; sleep 0.4
check "D15 press OK, release elsewhere = no apply"        'eqf "#{mouse}" 1'
key Escape; sleep 0.8
t display-form -c "$CL"; sleep 0.4; for i in $(seq 1 15); do key Tab; done; for i in $(seq 1 15); do key ISO_Left_Tab; done; key Down; key Down; key Up; key Left; key Right; key BackSpace; key space
check "D16 keyboard navigation storm"                     'alive'
key Escape; sleep 0.8

############################ E - text dialogs ############################
start
shot e0
for cmd in display-about display-keys display-commands display-sessions; do
  t $cmd -c "$CL"; sleep 0.5; shot "e_$cmd"; DOPEN=$(diffn e0 "e_$cmd"); key Escape; sleep 0.8; shot e_tmp; DCLOSE=$(diffn e0 e_tmp)
  check "E ${cmd} opens and Escape closes"                '[ "$DOPEN" -gt 1500 ] && [ "$DCLOSE" -lt 300 ]'
done
t display-keys -c "$CL"; sleep 0.5
key End; key Home; for i in $(seq 1 20); do key Next; done; for i in $(seq 1 20); do key Prior; done; for i in $(seq 1 20); do key Down; done; for i in $(seq 1 20); do key Up; done; key ctrl+Next; key ctrl+Prior
check "E5 keys dialog: all scroll keys"                   'alive'
drag 975 80 975 300 975 550; drag 975 550 975 300 975 80
check "E6 keys dialog: drag thumb both ways"              'alive'
click 5 300
check "E7 click outside dialog: alive"                    'alive'
click 498 572; sleep 0.5; shot e8
check "E8 click OK closes"                                '[ "$(diffn e0 e8)" -lt 300 ]'
t display-about -c "$CL"; sleep 0.4; key Return; sleep 0.6; shot e9
check "E9 Enter flash closes about"                       '[ "$(diffn e0 e9)" -lt 300 ]'
t display-form -c "$CL"; sleep 0.3; t display-about -c "$CL"; sleep 0.3; key Escape; sleep 0.8; shot e10
check "E10 about over form replaces cleanly"              '[ "$(diffn e0 e10)" -lt 300 ]'
t display-keys -c "$CL"; sleep 0.3; key Return; xdotool windowsize "$W" 480 340; sleep 0.8
check "E11 resize during flash timer"                     'alive'
xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
t display-sessions -c "$CL"; sleep 0.3; wheel 500 300 5 40; key Escape; sleep 0.8
check "E12 wheel storm on short dialog"                   'alive'

############################ F - toggles ############################
start
rect; X0=$DX; Y0=$DY; W0=$DW; H0=$DH; shot f0
pfx F; sleep 0.3
check "F1 F: desktop off -> pane full width"              '[ "$(fmt "#{window_width}")" -gt 115 ]'
pfx F; sleep 0.3; rect
check "F2 F back: rect identical"                         '[ "$DX,$DY,$DW,$DH" = "$X0,$Y0,$W0,$H0" ]'
t split-window -h; t split-window -v; pfx F; sleep 0.3; pfx F; sleep 0.3
check "F3 toggle with 3 panes"                            'eqf "#{window_panes}" 3'
t kill-pane -a
for i in $(seq 1 20); do key ctrl+b; key B; done; sleep 0.5
check "F4 20 rapid B"                                     'eqf "#{@menu-bar}" on'
for i in $(seq 1 20); do key ctrl+b; key F; done; sleep 0.5
check "F5 20 rapid F"                                     'eqf "#{@desktop}" on'
t display-form -c "$CL"; sleep 0.3; pfx F; sleep 0.3; pfx F; sleep 0.3; key Escape; sleep 0.8
check "F6 F while form open"                              'alive'
t send-keys -t t 'seq 1 100' Enter; sleep 0.5; t copy-mode -t t; pfx F; sleep 0.3; pfx F; sleep 0.3
check "F7 F while in copy-mode"                           'alive'
t send-keys -t t -X cancel >/dev/null 2>&1
pfx B; pfx F; sleep 0.3; t display-about -c "$CL"; sleep 0.3; key Escape; sleep 0.8; pfx B; pfx F
check "F8 dialog with menubar+desktop both off"           'alive'
for i in 1 2 3 4; do t new-window; done; pfx F; sleep 0.3; pfx F; sleep 0.3
check "F9 F with 5 windows"                               'eqf "#{session_windows}" 5'
t set -g status off; pfx B; sleep 0.3; pfx B; sleep 0.3; t set -g status on
check "F10 B with status off"                             'alive'

############################ G - panes & layouts in desktop ############################
start
t split-window -h; check "G1 split -h"                    'eqf "#{window_panes}" 2'
t split-window -v; check "G2 split -v"                    'eqf "#{window_panes}" 3'
for l in even-horizontal even-vertical main-horizontal main-vertical tiled; do t select-layout $l; sleep 0.15; done
check "G3 all 5 layouts"                                  'alive'
t kill-pane; check "G4 kill-pane"                         'eqf "#{window_panes}" 2'
t resize-pane -L 5; t resize-pane -R 3; t resize-pane -U 2; t resize-pane -D 2
check "G5 resize-pane keys"                               'alive'
t resize-pane -Z; sleep 0.2; check "G6 zoom pane in desktop" 'eqf "#{window_zoomed_flag}" 1'
t resize-pane -Z
rect; BX=$(cx $((DX+DW/2))); BY=$(cy $((AT+DY+DH/2)))
drag $BX $BY $((BX-40)) $BY $((BX-80)) $BY
check "G7 drag pane border"                               'alive'
click $(cx $((DX+3))) $(cy $((AT+DY+3)))
check "G8 click selects left pane"                        'eqf "#{pane_index}" 0'
for i in $(seq 1 6); do t split-window -v >/dev/null 2>&1; done; t select-layout tiled
check "G9 many splits + tiled"                            'alive'
t break-pane; sleep 0.3
check "G10 break-pane"                                    'eqf "#{session_windows}" 2'

############################ H - terminal resize ############################
start
rect; X0=$DX; Y0=$DY; W0=$DW; H0=$DH
xdotool windowsize "$W" $((80*8+4)) $((24*17+4)); sleep 0.8
check "H1 shrink to 80x24 (clamped, alive)"               'eqf "#{client_width}" 80'
xdotool windowsize "$W" $XW0 $XH0; sleep 0.8; rect
check "H2 grow back restores rect"                        '[ "$DX,$DY,$DW,$DH" = "$X0,$Y0,$W0,$H0" ]'
xdotool windowsize "$W" 240 136; sleep 0.8
check "H3 30x8"                                           'alive'
xdotool windowsize "$W" 160 85; sleep 0.8
check "H4 20x5"                                           'alive'
xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
click $MX_FICHIER $MY; xdotool windowsize "$W" 640 408; sleep 0.8; key Escape; sleep 0.3
check "H5 resize with menu open"                          'alive'
xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
t display-commands -c "$CL"; sleep 0.3; xdotool windowsize "$W" 480 340; sleep 0.8; key Escape; sleep 0.8
check "H6 resize with msgbox open"                        'alive'
xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
for s in "800 500" "600 300" "900 600" "400 250" "$XW0 $XH0"; do xdotool windowsize "$W" $s; sleep 0.25; done; sleep 0.8
check "H7 5 rapid terminal resizes"                       'eqf "#{client_width}" 125'
t split-window -h; xdotool windowsize "$W" 300 200; sleep 0.8; xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
check "H8 resize with splits"                             'eqf "#{window_panes}" 2'

############################ I - detach / attach ############################
start
rect; X0=$DX; Y0=$DY; W0=$DW; H0=$DH
kill "$XP"; sleep 0.8; XP=""
check "I1 client killed: server survives"                 'srv'
HOME=$THOME xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $SOCK attach" 2>/dev/null & XP=$!; sleep 2; W=$(xdotool search --pid "$XP" | tail -1); rect
check "I2 re-attach same size: rect preserved"            '[ "$DX,$DY,$DW,$DH" = "$X0,$Y0,$W0,$H0" ]'
kill "$XP"; sleep 0.5; HOME=$THOME xterm -geometry 80x24 -fa Monospace -fs 10 -e "$TM -S $SOCK attach" 2>/dev/null & XP=$!; sleep 2; W=$(xdotool search --pid "$XP" | tail -1)
check "I3 re-attach smaller"                              'eqf "#{client_width}" 80'
HOME=$THOME xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $SOCK attach" 2>/dev/null & XP2=$!; sleep 2
check "I4 two clients of different sizes"                 '[ "$(t list-clients | wc -l)" = 2 ]'
kill "$XP2" 2>/dev/null

############################ J - fuzz ############################
start
for i in $(seq 1 200); do xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) click 1; done; sleep 0.5
check "J1 200 random clicks"                              'alive'
start
for i in $(seq 1 60); do xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) mousedown 1 mousemove $((RANDOM%1000)) $((RANDOM%646)) mouseup 1; done; sleep 0.5
check "J2 60 random drags"                                'alive'
for i in $(seq 1 150); do xdotool key "$(rk)"; done; sleep 0.5
check "J3 150 random keys"                                'alive'
for i in $(seq 1 100); do xdotool key ctrl+b; xdotool key "$(rpk)"; done; sleep 0.8; xdotool key Escape; xdotool key q; sleep 0.3
check "J4 100 random prefix+key"                          'alive'
start
for i in $(seq 1 100); do xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) click $((4+RANDOM%2)); done; sleep 0.5
check "J5 100 random wheel events"                        'alive'
t display-form -c "$CL"; sleep 0.3; for i in $(seq 1 100); do xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) click 1; done; for i in $(seq 1 50); do xdotool key "$(rk)"; done; sleep 0.5
check "J6 fuzz with form open"                            'alive'
xdotool key Escape; sleep 0.8
click $MX_FICHIER $MY; for i in $(seq 1 100); do xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) click 1; done; sleep 0.5
check "J7 fuzz with menu open"                            'alive'
xdotool key Escape; sleep 0.3
xdotool windowsize "$W" 320 170; sleep 0.8; for i in $(seq 1 100); do xdotool mousemove $((RANDOM%320)) $((RANDOM%170)) click 1; done; for i in $(seq 1 50); do xdotool mousemove $((RANDOM%320)) $((RANDOM%170)) mousedown 1 mousemove $((RANDOM%320)) $((RANDOM%170)) mouseup 1; done; sleep 0.5
check "J8 fuzz on 40x10 terminal"                         'alive'
xdotool windowsize "$W" $XW0 $XH0; sleep 0.8
for i in $(seq 1 300); do case $((RANDOM%4)) in 0) xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) click 1;; 1) xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) mousedown 1 mousemove $((RANDOM%1000)) $((RANDOM%646)) mouseup 1;; 2) xdotool key "$(rk)";; 3) xdotool mousemove $((RANDOM%1000)) $((RANDOM%646)) click $((4+RANDOM%2));; esac; done; sleep 0.8
check "J9 300 mixed random events"                        'alive'
stop
log "=== TOTAL: $N tests, PASS=$PASS FAIL=$FAIL CRASH=$CRASH DETACH=$DETACH ENDED=$ENDED ==="
