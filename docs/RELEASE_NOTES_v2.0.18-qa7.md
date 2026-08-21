# Skate 3 Mobile v2.0.18 QA 7

This QA build targets startup hangs on the AYN Thor and Retroid Pocket 5.

- Android now keeps the boot frontend and startup movies on the complete emulated presentation path.
- Native Vulkan pipeline compilation waits until the game has actually queued world content.
- A false boot-time gameplay signal can no longer switch the following EA movie into native loading mode.
- A four-second watchdog completes only the first continuously stuck frontend movie. Later career movies are not affected.
- The stable app and stable update channel remain unchanged.

Use the System Driver first. Fully close the QA app between attempts and test at least five cold launches. On a successful test, confirm that you reached gameplay. On a failed test, attach the launcher diagnostics and full log to the existing device issue.

The APK includes no game data.

## Português do Brasil

Esta versão QA corrige travamentos na inicialização do AYN Thor e Retroid Pocket 5. A tela inicial e os vídeos de abertura usam a apresentação completa, a compilação Vulkan espera o carregamento real do mapa e um temporizador recupera somente o primeiro vídeo de abertura caso ele trave. Use primeiro o Driver do Sistema, feche totalmente o aplicativo entre os testes e faça pelo menos cinco inicializações.
