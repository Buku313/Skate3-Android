# Skate 3 Mobile v2.0.18 QA 3

This is an opt-in testing build that installs as **Skate 3 QA** beside stable v2.0.17. It updates QA 1 or QA 2 in place and preserves QA app data.

## QA 3 Quality-mode hotfix

- Fixes a Retroid Pocket 5 regression that occurred after switching from Performance to Quality.
- Keeps Quality at a 720-line scene target with full materials, vegetation, original draw/LOD range, shadows, and SSAO.
- Removes the experimental neighboring-cell stream probe from Quality and returns world loading to the game's vanilla stream focus.
- Keeps all QA 2 Title Update retry and Portuguese-language improvements.

The RP5 tombstone shows a guest render-dispatch abort in `sub_828DF518`, not a Vulkan driver crash. Performance was stable with the custom stream probe disabled. Quality had enabled a 100-meter probe that could introduce extra streamed objects into that virtual dispatcher. QA 3 removes that unsafe difference without reducing Quality to the Performance renderer.

## What to test

1. Install QA 3 directly over QA 1 or QA 2.
2. Start in Performance and enter gameplay.
3. Select High-End / Quality, apply, restart, and enter the same location.
4. Play for at least ten minutes and report whether loading, vegetation, shadows, audio, and gameplay remain stable.

## Português do Brasil

Esta versão corrige uma regressão no Retroid Pocket 5 depois da troca do modo Desempenho para Qualidade. A Qualidade mantém a cena em 720 linhas, materiais completos, vegetação, distância original, sombras e SSAO, mas volta a usar o carregamento de mundo padrão e seguro do jogo.

Instale a QA 3 diretamente sobre a QA 1 ou QA 2. Os dados do aplicativo QA serão preservados. Entre no jogo com o modo Desempenho, depois mude para **High-End / Quality**, aplique, reinicie e teste o mesmo local por pelo menos dez minutos.

## Legal

The APK contains no retail Skate 3 game data or Title Update package. Testers must provide their own legally obtained game dump. This is an unofficial, non-commercial fan project and is not affiliated with Electronic Arts.
