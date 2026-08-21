# Skate 3 Mobile v2.0.18 QA 4

This is an opt-in diagnostic build that installs over earlier **Skate 3 QA** builds and preserves QA app data.

## QA 4 RP5 black-screen isolation

The QA 3 RP5 report contained no native crash. The game and audio threads remained active, and the RB + Start settings blur opened, but the native renderer never logged a completed gameplay frame. This is a live black-screen presentation failure.

QA 4 makes Quality a controlled resolution-only test:

- Quality uses the exact scene feature set proven by Performance.
- Quality still selects its 1280x720 scene target.
- Extra materials, vegetation, shadows, SSAO, distant streaming, and other Quality-only expansions are temporarily held back.
- Title Update retries and English/Brazilian Portuguese launcher support remain included.

If QA 4 displays gameplay, the 720p target is safe and one of the held-back feature groups caused the black screen. Those groups can then return incrementally. If QA 4 remains black, the problem is isolated to the Quality target-size path.

## RP5 test

1. Install QA 4 directly over QA 3.
2. Keep the System Driver selected.
3. Start in Performance and confirm gameplay displays.
4. Switch to High-End / Quality, apply, and restart.
5. Report whether the same gameplay location displays or remains black.

## Português do Brasil

A QA 3 não apresentou uma nova falha nativa, mas o renderizador também não concluiu nenhum quadro de gameplay. O jogo e o áudio continuaram ativos atrás de uma tela preta.

Na QA 4, o modo Qualidade usa temporariamente os mesmos recursos de cena já verificados no modo Desempenho, mas mantém o alvo de renderização em 1280x720. Se a imagem aparecer, saberemos que a resolução de 720p é segura e que um dos recursos visuais adicionais causava a tela preta. Instale diretamente sobre a QA 3 e mantenha o Driver do Sistema selecionado.

## Legal

The APK contains no retail Skate 3 game data or Title Update package. Testers must provide their own legally obtained game dump. This is an unofficial, non-commercial fan project and is not affiliated with Electronic Arts.
