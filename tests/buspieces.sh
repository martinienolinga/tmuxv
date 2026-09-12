#!/bin/bash
# PIECES JOINTES : les agents s'echangent des fichiers (captures d'ecran,
# images, journaux). Le bus les garde (disque, et base en mode MariaDB), le
# message porte une ligne [piece:ID], et tmuxv donne a l'agent destinataire le
# chemin local du fichier. Le MCP des agents envoie, relit et capture.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
MCP=${TMUXV_MCP:-/home/martinien/claude-agent-server/target-pieces/release/claude-agent-mcp}
TESTDB='mysql:///claude_agent_test?socket=/run/mysqld/mysqld.sock'
V=$D/torture/buspieces; rm -rf $V; mkdir -p $V/work $V/bin; cd $V/work
export HOME=$V/home; mkdir -p "$HOME"
# Jamais le port de la production (4319) : le premier libre a partir de 4350.
printf 'set -g mouse on\nset -g @bus-port 4350\n' > $V/conf
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
S=/tmp/pc_$$
fmt(){ $TM -S $S display-message -p "$1" 2>/dev/null; }
bus(){ echo "http://127.0.0.1:$(fmt '#{bus_port}')"; }
b64(){ base64 -w0 "$1"; }
PIECES=$HOME/.local/share/tmuxv/pieces

# Une vraie image PNG, et un fichier de 7 Mo (au-dessus de la limite).
python3 - "$V/capture.png" <<'EOF'
import sys, zlib, struct
w, h = 64, 32
raw = b''.join(b'\x00' + b''.join(bytes((x * 4 % 256, y * 8 % 256, 128)) for x in range(w)) for y in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
EOF
head -c 7340032 /dev/urandom > $V/gros.bin

cat > $V/bin/agent.sh <<'EOF'
#!/bin/bash
# L'invite en BAS de l'ecran, comme Claude Code : tmuxv ne livre qu'a un
# agent dont l'invite est visible dans les dernieres lignes.
printf '\033[?2004h'; for i in $(seq 1 80); do echo; done; echo "agent pret"; echo "❯ "
exec 3<&0
stdbuf -o0 cat <&3 >> "$1"
EOF
chmod +x $V/bin/agent.sh

xterm -geometry 140x40 -fa Monospace -fs 10 -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null & XP=$!
sleep 3
B=$(bus); echo "bus du serveur de test : $B ($(fmt '#{bus_mode}'))"
ok "le bus de test n'est pas sur le port de la production" "$([ "${B##*:}" != 4319 ] && echo oui || echo NON)" "oui"

echo "--- memoire : aller-retour d'une image ---"
R=$(printf '{"name":"capture.png","mime":"image/png","data":"%s"}' "$(b64 $V/capture.png)" | curl -s --max-time 5 -X POST $B/piece --data-binary @-)
echo "  reponse : $R"
ID=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))' 2>/dev/null)
ok "un identifiant est rendu" "$(echo -n "$ID" | wc -c)" "32"
ok "la taille est la bonne" "$(echo "$R" | grep -o '"size":[0-9]*' | cut -d: -f2)" "$(stat -c %s $V/capture.png)"
ok "le fichier est range sous son nom" "$([ -f $PIECES/$ID/capture.png ] && echo oui || echo NON)" "oui"
curl -s --max-time 5 -D $V/h.txt -o $V/retour.png $B/piece/$ID
ok "le fichier revient identique" "$(cmp -s $V/capture.png $V/retour.png && echo oui || echo NON)" "oui"
ok "avec son type" "$(grep -i '^content-type' $V/h.txt | tr -d '\r' | awk '{print $2}')" "image/png"
ok "et son nom" "$(grep -i '^x-piece-name' $V/h.txt | tr -d '\r' | awk '{print $2}')" "capture.png"

echo "--- memoire : ce qui doit etre refuse ---"
R=$(printf '{"name":"../../../evil.sh","data":"%s"}' "$(echo coucou | base64)" | curl -s -X POST $B/piece --data-binary @-)
ID2=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))' 2>/dev/null)
ok "un nom piege reste dans son dossier" "$(ls $PIECES/$ID2 2>/dev/null)" "evil.sh"
ok "rien n'a ete ecrit au-dessus" "$(ls $HOME/.local/share/evil.sh $HOME/evil.sh $PIECES/../evil.sh 2>/dev/null | wc -l)" "0"
ok "un type fantaisiste devient generique" "$(echo "$R" | grep -o '"mime":"[^"]*"')" '"mime":"application/octet-stream"'
ok "au-dela de 6 Mo : refuse" "$(printf '{"name":"gros.bin","data":"%s"}' "$(b64 $V/gros.bin)" | curl -s -o /dev/null -w '%{http_code}' -X POST $B/piece --data-binary @-)" "413"
ok "base64 invalide : refuse" "$(curl -s -o /dev/null -w '%{http_code}' -X POST $B/piece -d '{"name":"x","data":"@@@"}')" "400"
ok "identifiant inconnu : 404" "$(curl -s -o /dev/null -w '%{http_code}' $B/piece/0123456789abcdef0123456789abcdef)" "404"
ok "identifiant piege : refuse" "$(curl -s -o /dev/null -w '%{http_code}' "$B/piece/..%2F..%2Fetc")" "400"
ok "le serveur est vivant" "$(fmt ok)" "ok"

echo "--- livraison : l'agent recoit le chemin du fichier ---"
$TM -S $S claude-manager; sleep 2
W=$($TM -S $S display -p '#{window_index}')
$TM -S $S split-window -h -t $W -e AGENT_NAME=pc-image "exec -a claude bash $V/bin/agent.sh $V/recu.txt"; sleep 1
$TM -S $S select-layout -t $W tiled; sleep 4
curl -s -X POST $B/send -d "{\"from\":\"pc-test\",\"to\":\"pc-image\",\"body\":\"regarde la capture\\n[piece:$ID] capture.png (image/png, 1 Ko)\"}" >/dev/null
curl -s -X POST $B/send -d '{"from":"pc-test","to":"pc-image","body":"et celle-ci [piece:0123456789abcdef0123456789abcdef] ailleurs.png"}' >/dev/null
k=0; until grep -q "ailleurs" $V/recu.txt 2>/dev/null || [ $k -ge 30 ]; do sleep 0.5; k=$((k+1)); done
echo "  recu : $(tr '\n' ' ' < $V/recu.txt 2>/dev/null | cut -c1-240)"
ok "le chemin local est donne a l'agent" "$(grep -c "\[fichier joint : $PIECES/$ID/capture.png\]" $V/recu.txt 2>/dev/null)" "1"
ok "une piece absente d'ici renvoie a get_attachment" "$(grep -c 'get_attachment' $V/recu.txt 2>/dev/null)" "1"

echo "--- MCP : envoyer, relire, capturer ---"
python3 - "$MCP" "$B" "$V" "$S" <<'EOF'
import json, subprocess, sys, os, base64
mcp, bus, v, sock = sys.argv[1:5]
def ok(what, got, want):
    print(("  OK    " if got == want else "  ECHEC ") + f"{what} ({got})" + ("" if got == want else f" au lieu de {want}"))
env = dict(os.environ, AGENT_BUS_URL=bus, AGENT_NAME="pc-mcp")
env.pop("TMUX", None); env.pop("TMUX_PANE", None)
def session(env, calls):
    p = subprocess.Popen([mcp], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True)
    lines = [{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18"}},
             {"jsonrpc":"2.0","method":"notifications/initialized"}]
    for i, c in enumerate(calls, 1):
        lines.append({"jsonrpc":"2.0","id":i,"method":c[0],"params":c[1]})
    out, _ = p.communicate("\n".join(json.dumps(l) for l in lines) + "\n", timeout=120)
    res = {}
    for l in out.splitlines():
        m = json.loads(l)
        res[m.get("id")] = m.get("result", m.get("error"))
    return res
r = session(env, [("tools/list", {})])
names = sorted(t["name"] for t in r[1]["tools"])
ok("les nouveaux outils sont annonces", " ".join(n for n in names if n in ("get_attachment","screenshot")), "get_attachment screenshot")
r = session(env, [("tools/call", {"name":"send_message","arguments":{"to":"pc-dest","body":"voici","attachments":[v+"/capture.png"]}})])
txt = r[1]["content"][0]["text"]
ok("send_message joint le fichier", "with 1 file(s)" in txt, True)
import urllib.request
inbox = json.load(urllib.request.urlopen(bus + "/inbox?agent=pc-dest&since=0"))
body = inbox["messages"][-1]["body"]
pid = body.split("[piece:")[1].split("]")[0] if "[piece:" in body else ""
ok("le message porte la ligne [piece:ID]", len(pid), 32)
r = session(env, [("tools/call", {"name":"get_attachment","arguments":{"id":pid}})])
c = r[1]["content"]
ok("get_attachment montre l'image", (c[0]["type"], c[0].get("mimeType")), ("image", "image/png"))
ok("... identique a l'envoi", base64.b64decode(c[0]["data"]) == open(v+"/capture.png","rb").read(), True)
r = session(env, [("tools/call", {"name":"send_message","arguments":{"to":"pc-dest","body":"x","attachments":[v+"/gros.bin"]}})])
ok("un fichier trop gros n'est pas envoye", r[1]["isError"], True)
open(v+"/page.html","w").write("<html><body style='background:#c00'><h1>Page de test</h1></body></html>")
r = session(env, [("tools/call", {"name":"screenshot","arguments":{"url":"file://"+v+"/page.html","width":640,"height":400,"send_to":"pc-dest"}})])
c = r[1]["content"]
ok("screenshot d'une page : une image", (c[0]["type"], c[0].get("mimeType")), ("image", "image/png"))
png = base64.b64decode(c[0]["data"])
ok("... au bon format", png[:8] == b"\x89PNG\r\n\x1a\n" and int.from_bytes(png[16:20],"big") == 640, True)
ok("... et envoyee", "Sent to pc-dest" in c[1]["text"], True)
# La console d'un agent : le MCP tourne alors "dans" tmuxv (TMUX, TMUX_PANE).
pane = subprocess.run(["tmuxv" if False else os.environ.get("TM", "/home/martinien/tmux/build/tmux"), "-S", sock,
                       "list-panes", "-a", "-F", "#{pane_id}"], capture_output=True, text=True).stdout.split()[0]
os.makedirs(v+"/pbin", exist_ok=True)
tm = os.environ.get("TM", "/home/martinien/tmux/build/tmux")
open(v+"/pbin/tmuxv","w").write(f"#!/bin/sh\nexec {tm} \"$@\"\n"); os.chmod(v+"/pbin/tmuxv", 0o755)
env2 = dict(env, TMUX=sock+",1,0", TMUX_PANE=pane, PATH=v+"/pbin:"+env["PATH"])
r = session(env2, [("tools/call", {"name":"screenshot","arguments":{"agent":"pc-image"}})])
c = r[1]["content"]
ok("screenshot de la console d'un agent : une image", c[0]["type"], "image")
EOF
ok "le serveur est vivant" "$(fmt ok)" "ok"

echo "--- MariaDB : la piece va en base, et en revient ---"
mysql claude_agent_test -e "drop table if exists pieces" 2>/dev/null
$TM -S $S set -g @bus-db "$TESTDB"
k=0; until [ "$(fmt '#{bus_mode}')" = "db" ] || [ $k -ge 30 ]; do sleep 0.3; k=$((k+1)); done
ok "bus en base" "$(fmt '#{bus_mode}')" "db"
R=$(printf '{"name":"base.png","mime":"image/png","data":"%s"}' "$(b64 $V/capture.png)" | curl -s -X POST $B/piece --data-binary @-)
ID3=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))' 2>/dev/null)
sleep 1
ok "la piece est en base" "$(mysql claude_agent_test -N -B -e "select size from pieces where id='$ID3'" 2>/dev/null)" "$(stat -c %s $V/capture.png)"
rm -rf $PIECES/$ID3
curl -s -o $V/retour3.png $B/piece/$ID3
ok "effacee du disque, elle revient de la base" "$(cmp -s $V/capture.png $V/retour3.png && echo oui || echo NON)" "oui"
ok "et reprend sa place sur le disque" "$([ -f $PIECES/$ID3/base.png ] && echo oui || echo NON)" "oui"
ok "le serveur est vivant" "$(fmt ok)" "ok"

$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null; true
