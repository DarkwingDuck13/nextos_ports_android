# NFS Most Wanted (Mali-450 so-loader) — HANDOFF p/ próxima sessão (2026-06-15)

Device .164 (senha nextos). Port em `~/nextos_ports_android/ports/nfs/`.
Build: `./build.sh`. Rodar no device: `cd /storage/roms/nfs && ./go.sh` (ou os g*.sh).
Ghidra: `~/re-tools` (`export GHIDRA_INSTALL_DIR=~/re-tools/ghidra_12.1.2_PUBLIC JAVA_HOME=~/re-tools/jdk-21.0.11+10`).

## ✅ O QUE JÁ FUNCIONA (não regredir!)
- **Render completo** (era 100% preto). Causas resolvidas: getTotalMemory=0→2048MB;
  density softfp/hardfp (pcs aapcs no retorno float); 🔑getWidth/getHeight=0→1280×720
  (Primary view 0x0→glViewport 0x0→tudo culled); OBB→ler do disco (isObbAssets→0).
- **TEXTO/fontes** reais do jogo (font_shim.c + stb_truetype carregando gothambook.ttf).
  Bugs resolvidos: JNI Call*MethodV recebem va_list (não varargs); make_jstring COPIA.
- **LOGOS do splash**: EA, Firemonkeys, NEED FOR SPEED MOST WANTED renderizam PERFEITOS.
  (atlas_rebind: sprites de imagem do .sba ligam tex=0=preto; religa o último atlas
  não-quadrado nos draws sem textura. egl_shim.c, PADRÃO ligado, NFS_NOATLASHACK desliga.)
- **EULA** renderiza com texto legível (painéis escuros + "PRIVACY/USER AGREEMENT/CONFIRM").

## ❌ ISSUE A — "LOGO BRANCA / decorações ainda bugadas" (multi-atlas)
SINTOMA: depois do MOST WANTED, a tela de disclaimer ("All experiences...") tem o
**spinner de loading e as decorações (speed_lines/triangles/blur) LAVADOS/brancos**, e
qualquer sprite de imagem após os atlas de menu carregarem fica errado.
CAUSA-RAIZ: os sprites de IMAGEM do .sba (logos/ícones/decorações) **ligam tex=0**
(nenhuma textura) → GLES2 amostra (0,0,0,1) preto opaco. O atlas COLORIDO carrega certo
mas o **link sprite→atlas resolve NULL na engine — game-wide** (só texto/glyph e quads
sólidos ligam textura). O atlas_rebind contorna religando o último atlas grande não-
quadrado, MAS após o splash carregam vários atlas de menu/loading (tex 9-19 ~1020x1016)
→ g_nfs_atlas_tex muda → decorações do splash bindam o atlas ERRADO → lavadas.
⚠️ Per-programa FALHOU (shaders compartilhados texto+logo → religava textura de texto).
PRÓXIMOS PASSOS (em ordem de promessa):
1. **FIX REAL = achar por que a engine binda tex=0** p/ os sprites .sba. O atlas sobe como
   GL tex N (glGenTextures→glBindTexture(N)→glTexImage2D) mas o sprite binda 0 no draw →
   o id da textura no objeto Texture do .sba está 0, OU o sprite referencia outro objeto.
   RE: decompilar quem chama glBindTexture (thunk @0x00b2647c) e glGenTextures (@0x00b26534)
   — refs são PIC NÃO-resolvidas pelo Ghidra; precisa rodar auto-analysis OU scanner
   movw/movt próprio (tools_armdis.py). Achar o registro "Add texture: <name>" e o lookup
   sprite→texture. Diag prontos: NFS_DRAWLOG (fbo/tex/prog/blend por draw), NFS_TEXLOG
   (uploads + "ATLAS candidate"), NFS_TEXDUMP (salva atlas), NFS_SHADERDUMP (fonte dos
   shaders: sh4=texture2D, sh5=varColor*texture2D, sh6=constantColor*texture2D).
2. Heurística melhor (paliativo): correlacionar tex=0 draw ao atlas certo. Difícil sem o
   link. Tentado: per-programa (falhou), global último-atlas (atual, erra multi-atlas).

## ❌ ISSUE B — CONTROLES / passar do CONFIRM (navegação)
SINTOMA: nenhum input (touch nem tecla/gamepad) faz o EULA reagir → preso no CONFIRM.
INFRA JÁ PRONTA (main.c, loop de render):
- **Touch**: escreve `/storage/roms/nfs/tap.txt` com "x y" (px) → DOWN+UP em
  nativeTouchScreenEvent. Floats x,y passados como BITS via int (softfp). EULA: CONFIRM
  em (640, 534); texto de aceite vermelho y172-220 (sem checkbox visível).
- **Tecla/gamepad**: escreve `key.txt` com keycode Android (19=UP 20=DOWN 21=LEFT
  22=RIGHT 23=CENTER 66=ENTER 96=A 97=B 4=BACK) → nativeOnPhysicalKeyDown.
- **NFS_FORCEINPUT=1**: zera a flag global "input desabilitado" (@VA 0xadfd44,
  base=kdown-0x54cb98) todo frame.
DIAGNÓSTICO (por que é descartado):
- TOUCH: nativeTouchScreenEvent(0x54d764) → itera **f_55a244 (lista de handlers de toque)**
  que retorna 0. A comparação usa um VIEW-object; passamos `fake_this`, mas a engine
  registrou o handler p/ o **GameGLSurfaceView REAL** (queried via getGameGLSurfaceView
  JNI) → match falha. Coords chegam CERTAS (verificado o ABI).
- KEY: nativeOnPhysicalKeyDown processa (bl 0x5447d0) mas EULA não reage mesmo c/ flag=0
  (pode ser touch-only OU mesma falta de handler/view).
PRÓXIMOS PASSOS (em ordem de promessa):
1. **BYPASS DO EULA (mais promissor p/ VER o menu sem resolver input):** o jogo gateia o
   EULA por um flag de aceite. Strings: `/active_accepted`, `ActiveAccepted: failed to
   write user accept to "<path>"`, flow `published/flow/active_accept_eula.sb` (lido no
   boot, confirmado NFS_FSPATHLOG). FALTA: achar o PATH onde o aceite é gravado/checado
   (não é um dos .sb de flow — é storage de save/preference). Capturar via hook de
   open/stat (my_open/my_fopen logam; o aceite pode usar stat/access). Criar o arquivo
   no device → EULA pulado → menu aparece. Decompilar a fn que loga "ActiveAccepted:
   failed to write" p/ ver o path e o formato.
2. Passar o **GameGLSurfaceView REAL** como obj no nativeTouchScreenEvent (capturar o
   objeto que a engine usa via getGameGLSurfaceView no jni_shim e reusar como fake_this
   do touch).
3. Registrar o touch listener / investigar a thread de input.
4. Gamepad via MogaController (nativeOnKeyEvent/nativeOnMotionEvent recebem KeyEvent/
   MotionEvent OBJETOS — precisa fakear via jni_shim; DPAD=0x13-0x16, A=0x60 no switch).

## INFRA / PEGADINHAS
- Wrappers GL via TABELA nfs_shims[] (so_resolve usa dlsym, GetProcAddress NÃO pega GL core).
- /dev/fb0 NÃO reflete Mali → usar glReadPixels (NFS_SEQSHOT salva seq_NNNN.raw na vfat;
  converter: Image.frombytes RGBA 1280x720 + FLIP_TOP_BOTTOM). Writes /tmp falham durante nfs.
- ES mascarado. Matar nfs ANTES de scp do binário (file busy). FMOD spam via syscall = thrash.
- Splash: copiei splash_1500.sba (real) por cima dos stubs splash_1775/1333.sba (262B) no
  disco em published.1x/texturepacks_ui/ (16:9 carregava stub vazio).
- Fixes Mali do Bully portados: highp→mediump frag (my_glShaderSource), GL_TEXTURE_MAX_LEVEL
  skip + mipmap→LINEAR (my_glTexParameteri). Ref: ~/nextos_ports_android/ports/bully/src.
