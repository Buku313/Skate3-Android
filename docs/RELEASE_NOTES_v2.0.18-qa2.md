# Skate 3 Mobile v2.0.18 QA 2

This is an opt-in testing build that installs as **Skate 3 QA** beside stable v2.0.17. It updates QA 1 in place and preserves QA 1 app data.

## QA 2 setup hotfix

- Retries the verified Title Update download three times when the community server returns a temporary HTTP error or broken response.
- Preserves the completed ISO extraction between attempts and after failure. There is no need to start over.
- Replaces the raw HTTP error with clear recovery instructions.
- Translates the retry progress and recovery message into Brazilian Portuguese.
- Keeps **Select Title Update File** available for a compatible package the tester already has.

XboxUnity was returning HTTP 502 across its homepage, metadata API, and download endpoint when this build was published. A client update cannot repair a remote outage, but QA 2 handles transient failures safely and explains what to do. Do not start over. Retry later or use your own compatible Title Update package.

QA 2 also includes all compatibility changes from QA 1: the FMV safety timeout, asynchronous Android menu shader compilation, ARMv8-A baseline, stereo Android audio diagnostics, portable Quality profile, and English/Brazilian Portuguese launcher.

## Português do Brasil

Esta é uma versão opcional para testes que instala como **Skate 3 QA** ao lado da versão estável v2.0.17. Ela atualiza a QA 1 sem apagar os dados do aplicativo QA.

O XboxUnity estava retornando HTTP 502 na página principal, API de metadados e download durante a publicação desta versão. Isso não é uma falha do Retroid Pocket 5. A QA 2 tenta o download três vezes, preserva a extração concluída e mostra instruções claras. **Não recomece a instalação.** Tente novamente mais tarde ou use **Selecionar Arquivo da Title Update** com um pacote compatível que você já possua.

## Legal

The APK contains no retail Skate 3 game data or Title Update package. Testers must provide their own legally obtained game dump. This is an unofficial, non-commercial fan project and is not affiliated with Electronic Arts or XboxUnity.
