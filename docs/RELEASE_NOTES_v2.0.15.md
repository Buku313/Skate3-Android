# Skate 3 Mobile v2.0.15

Targeted native callback hotfix based on an AYN Thor crash report.

## What the report proved

- Vulkan and the Qualcomm Adreno 740 System Driver initialized successfully.
- The guest address space, native renderer, High-End profile, pipelines, HDR,
  shadows, and FMV planes initialized before the crash.
- A grouped-operation worker called stale target `0xFFFDFFFF` from guest return
  address `0x82B3CE2C`.
- The generated retail function checks this optional callback as a boolean and
  follows its normal cleanup path when it returns false.

## Fix

- Recover that exact return address by returning false from the optional
  content-validation callback.
- Keep all unknown indirect calls at other return addresses fatal so missing
  recompilation and memory corruption remain visible.
- Keep System Driver as the recommended default. This was not a Turnip or
  Vulkan initialization failure.

## Verification

- Native ARM64 debug and release builds
- Android lint and APK assembly
- RG406V System Driver launch to the Skate 3 title screen at 60 FPS
- No new native crash during the RG406V launch path
- AYN Thor confirmation is still requested because this stale callback does not
  reproduce on the RG406V

The APK does not contain retail Skate 3 game data. Use a legally obtained game
dump and follow the setup instructions in the repository.
