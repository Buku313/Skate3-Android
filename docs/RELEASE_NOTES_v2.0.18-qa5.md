# Skate 3 Mobile v2.0.18 QA 5

This is an opt-in diagnostic build that installs over earlier **Skate 3 QA** builds and preserves QA app data.

## RP5 intermittent black-screen fix

QA 4 proved that the Retroid Pocket 5 can render the Quality profile's 1280x720 scene target, but testers reported that startup only succeeded roughly half the time.

The QA 4 profile copied Performance's visible feature settings, but one internal pipeline gate still used the profile number. Quality therefore compiled optional outline, spline, and shadow shader families even though those features were disabled. Cold and partially warmed shader caches could take different startup paths.

QA 5 fixes the mismatch:

- The 1280x720 Quality target remains enabled.
- The pipeline builder now follows the active lean-scene flag instead of assuming every Quality profile needs the full optional PSO set.
- Renderer installation applies the same defensive lean-feature guard.
- The startup log must report `lean Android set` for this QA Quality profile.
- Stable v2.0.17 and its update channel remain unchanged.

## RP5 test

1. Install QA 5 directly over QA 4.
2. Keep the System Driver selected.
3. Leave High-End / Quality selected.
4. Fully close and reopen the QA app at least five times.
5. Report how many launches reach visible gameplay and attach diagnostics from any failed attempt.

## Português do Brasil

A QA 4 confirmou que o RP5 consegue renderizar o alvo de 1280x720, mas uma verificação interna ainda compilava shaders opcionais desativados. Isso fazia a inicialização depender do estado do cache de shaders.

A QA 5 faz o compilador seguir o modo de cena leve usado neste teste. Instale sobre a QA 4, mantenha o Driver do Sistema e abra o jogo pelo menos cinco vezes. Envie o diagnóstico de qualquer tentativa com tela preta.

## Legal

The APK contains no retail Skate 3 game data or Title Update package. Testers must provide their own legally obtained game dump. This is an unofficial, non-commercial fan project and is not affiliated with Electronic Arts.
