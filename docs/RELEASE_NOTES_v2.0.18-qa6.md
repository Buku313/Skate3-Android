# Skate 3 Mobile v2.0.18 QA 6

This QA build targets an intermittent early-boot hang reported on the Retroid Pocket 5.

The failed QA 5 diagnostic showed a live audio thread but no first presentation, native pipeline creation, scene target, or crash. The guest render thread stopped immediately after its first offscreen 2D captures.

QA 6 tightens the boot capture boundary:

- Only the four verified 2D vertex layouts and three supported primitive types are accepted.
- Individual and per-frame 2D payloads have conservative size limits.
- Invalid transient draw state is rejected before reading guest memory.
- The first twelve guest frames now log capture begin and completion markers.

Install QA 6 over the previous QA app, keep the System Driver selected, fully close the app between attempts, and test at least five launches. Attach diagnostics from any failed launch.

The stable app and stable update channel are unchanged. The APK includes no game data.

## Português do Brasil

Esta versão QA corrige uma possível trava durante a captura 2D no início do jogo e adiciona marcadores de diagnóstico aos primeiros quadros. Instale sobre a versão QA anterior, mantenha o Driver do Sistema e teste pelo menos cinco inicializações completas. Envie o diagnóstico de qualquer tentativa que ficar em tela preta.
