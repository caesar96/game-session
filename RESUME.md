# Resumen: Bug de flickering y fix

## Síntoma
Al lanzar game-session con overclock (OD) + `force_level=high`, el monitor hacía flickering intermitente. El juego (FC26) se veía inestable visualmente.

## Causa raíz
El MCLK (memoria) de la RX 6600 oscilaba en ~875 MHz al hacer transiciones en modo `auto`. Al setear `force_level=high` **antes** de aplicar los cambios OD, el driver quedaba locked con frecuencias stock, y luego al hacer `od-commit` el driver debía transicionar a las nuevas frecuencias — ese cambio causaba flickering en el monitor LG UltraGear.

## Fix
Se cambió el orden en `game-session.cpp::apply_gpu()` (`ref:330`):

**Antes (bug):**
1. force-level = high
2. profile, power-cap
3. OD: sclk, mclk, voltage → commit

**Después (fix):**
1. profile, power-cap
2. OD: sclk, mclk, voltage → commit
3. force-level = high

Esto evita la transición de frecuencias mientras el display está activo — los OD se aplican en `auto` (sin flickering) y luego se lockea `high` solo cuando las frecuencias target ya están estables.

## Verificación
Se probó con sesión de 20s: forzando `high` manualmente, los valores OD (SCLK 2750, MCLK 950, VDDGFX -5mV) permanecieron intactos. El flickering desapareció completamente.

## Config actual
- SCLK: 2650/2750
- MCLK: 950
- Voltage: -5mV
- Fan curve: aggressive
- Monitor preset: habilitado. Se configura via `GS_MONITOR_PRESET` env, `MONITOR_PRESET` env (backward compat), o `[monitor] preset = RTS` en config file. Valores: FPS, RTS, Gamer 1, Gamer 2, Vivid, Reader, HDR Effect.
