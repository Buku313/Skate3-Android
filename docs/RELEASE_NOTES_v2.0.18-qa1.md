# Skate 3 Mobile v2.0.18 QA 1

This is an opt-in testing build. It installs as **Skate 3 QA** beside the stable app and does not replace v2.0.17. Because Android isolates app data, testers must select their legally obtained Skate 3 ISO again inside the QA app.

## What we need tested

- Cold launch, language selection, intro video, title screen, and first gameplay entry.
- AYN Thor, Odin 2, Retroid Pocket 5 and 6, Samsung, and older ARM64 devices.
- Sound speed, crackling, missing audio, and whether disabling audio affects the next launch.
- System Driver first. Turnip remains an optional troubleshooting experiment.
- English and Brazilian Portuguese launcher language selection.

## Changes since v2.0.17

- Adds a six-second Android safety limit to the FMV fallback so a stale movie heartbeat cannot leave the screen black forever.
- Keeps Android menu shader compilation asynchronous to avoid long startup stalls on mobile drivers.
- Uses the ARMv8-A baseline and disables optional CRC instructions that caused SIGILL on older ARM64 devices.
- Requests stereo Android audio and adds better audio diagnostics.
- Makes the Quality profile portable by retaining materials, shadows, and SSAO while disabling HDR, bloom, shafts, and 2x MSAA by default.
- Adds an English and Brazilian Portuguese launcher picker, translated setup, update, repair, and bug-report flows.

## Português do Brasil

Esta é uma versão opcional para testes. Ela instala como **Skate 3 QA** ao lado do aplicativo estável e não substitui a v2.0.17. Como o Android separa os dados de cada aplicativo, será necessário selecionar novamente sua ISO de Skate 3 obtida legalmente dentro do aplicativo QA.

### O que precisamos testar

- Inicialização a frio, seleção de idioma, vídeo de introdução, tela inicial e primeira entrada no jogo.
- AYN Thor, Odin 2, Retroid Pocket 5 e 6, Samsung e dispositivos ARM64 mais antigos.
- Velocidade do som, estalos, áudio ausente e se desativar o áudio afeta a próxima inicialização.
- Teste primeiro com o Driver do Sistema. Turnip continua sendo um experimento opcional para diagnóstico.
- Seleção de idioma do launcher em inglês e português do Brasil.

Use o botão **Relatar bug / Diagnóstico do dispositivo** e inclua exatamente o que aconteceu.

## Legal

The APK contains no retail Skate 3 game data. Testers must provide their own legally obtained game dump. This is an unofficial, non-commercial fan project and is not affiliated with Electronic Arts.
