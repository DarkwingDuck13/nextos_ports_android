# 🎮 Controle: gamepad nativo, gptokeyb e a ABI dos eventos

O jogo Android espera input "do Android" (eventos de `InputDevice`/keycode, ou um `GameController` do SDL). O device entrega um joystick `/dev/input/js0`. Há dois caminhos — escolha por jogo.

## Caminho A — gptokeyb (teclado → o jogo lê "teclado")
Quando o engine consome **keycodes do Android** (não SDL-GameController), o jeito mais rápido é o `gptokeyb`: ele agarra o `js0` e emite **teclado**. Aí o loader mapeia `SDL_Keycode` → keycode do Android.
* Ex. (Crazy Taxi): `SDLK_UP→AKEYCODE_DPAD_UP`, `SDLK_SPACE→AKEYCODE_BUTTON_A`, `SDLK_LCTRL→AKEYCODE_BUTTON_B`, `SDLK_RETURN→AKEYCODE_BUTTON_START`.
* No `.gptk` use **só tokens válidos** do gptokeyb do device.
* O launcher sobe o `gptokeyb` em background apontando pro `.gptk` antes de lançar o jogo.

## Caminho B — gamepad nativo (o jogo tem API de controle própria)
Engines como Cocos2d-x têm callback nativo de controller. Aqui o segredo é acertar a **ABI** da função.
* **Bug clássico (Chrono Trigger):** faltava um argumento `jstring vendorName` no `nativeControllerButtonEvent`/`Axis` → todos os args deslocavam 1 posição e o controle não respondia. Conferir a assinatura exata contra o Java do jogo.
* Force `hasJoystick()→1` e `isConnected()→1` (default), e abra **todos** os pads encontrados.
* Padrão de botões: Xbox costuma "casar" sem remapear.

## ⚠️ Regra de segurança: NUNCA clone o ID do pad físico
Auto-input (uinput) que **clona o id do pad real** (ex.: `0810:0001`) quebra o controle do usuário — só reboot resolve. Se precisar injetar eventos, injete **direto** no `/dev/input/eventN` do pad real, ou capture+clone com cuidado (evcap). Ferramentas como `inject`/`evcap`/`navseq` são seguras; clonar o vendor/product não é.

## Polling vs Eventos
Alguns jogos não fazem polling (`GetGamepadButtons` nunca é chamado) e usam **eventos**. Se o controle "não entra", confirme qual modelo o jogo usa antes de reimplementar a leitura.

---
*Resumo: keycode-driven → gptokeyb + mapa SDL→Android; API própria → acerte a ABI do callback e ligue hasJoystick/isConnected. Nunca clone o id do pad físico.*
