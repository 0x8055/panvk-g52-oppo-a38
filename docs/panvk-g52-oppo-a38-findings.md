# PanVK en Mali-G52 MC2 — Oppo A38 (kernel moderno) vs Redmi 13C (kernel 4.19)

Reporte de pruebas corridas nativamente en Termux, replicando y extendiendo el trabajo de
[LukeValen/panvk-mali-g52](https://github.com/LukeValen/panvk-mali-g52), en un segundo dispositivo
con el mismo SoC pero un kernel mucho más reciente. Objetivo: aislar qué partes del trabajo dependen
del kernel/DDK y cuáles son verdaderamente portables entre dispositivos con el mismo chip.

## Dispositivos comparados

| | Redmi 13C (original) | Oppo A38 (este reporte) | Infinix X6853 (fork G57, referencia) |
|---|---|---|---|
| SoC | MT6769V/CZ (mt6768) | MT6768 | Helio G99 |
| GPU | Mali-G52 MC2 | Mali-G52 MC2 | Mali-G57 MC2 (Valhall) |
| Kernel | 4.19.191 | **6.6.118-android15** | 6.12.38-android16 |
| Android | 14 (API 34) | 15 | 16 |
| Kbase UAPI | 11.38 | **11.46** | 11.46 |
| Root | No | No | No |

Dato clave: aunque G52 y G57 son chips distintos, el Oppo A38 comparte el **mismo UAPI (11.46)** que
el dispositivo G57, no el 11.38 del Redmi 13C — confirma que el UAPI lo determina el kernel/DDK, no el
modelo de GPU.

## Metodología

- Todo compilado 100% nativo en Termux (clang 21.1.8, meson 1.12.0 vía pip, ninja 1.13.2), sin cross-compile.
- Base: fork [funnymdzz/mesa](https://github.com/funnymdzz/mesa) (backend `kbase_kmod.c`, resuelve
  descubrimiento del GPU sin nodo DRM).
- Patches del driver aplicados desde `panvk-driver-patch/` de LukeValen (`apply_kbase_patch.py`,
  `fix_kbase_patch.py`, `gpu_variant_fix.patch`, `exec_init_order_fix.patch`), con ajustes propios
  donde el script no aplicaba a este dispositivo (detallado abajo).
- Build dirigido solo a `src/panfrost/vulkan/libvulkan_panfrost.so` (no el árbol completo) para ahorrar
  tiempo/batería en un teléfono de gama baja.

## Hallazgo 1 — Stride del `base_jd_atom` (dato inédito)

Bisección directa contra `/dev/mali0` vía `KBASE_IOCTL_JOB_SUBMIT`, con un atom dependency-only
(`core_req=0`, no toca hardware, seguro de correr):

```
stride=48 -> EINVAL   (el valor que sí funciona en el Redmi 13C, UAPI 11.38)
stride=56 -> OK       (nuevo, no reportado por ningún proyecto hasta ahora)
stride=64 -> OK       (el valor documentado por el fork G57, UAPI 11.46)
```

Ningún otro valor probado (0,16,40,44,52,60,72,80) fue aceptado. Conclusión: en UAPI 11.46 el kernel
exige un atom de al menos 56 bytes; 64 es el valor más fácil de justificar por compatibilidad con lo ya
documentado en G57.

## Hallazgo 2 — Detección del GPU es idéntica entre kernels

`vkEnumeratePhysicalDevices` devuelve `count=1`, `deviceID=0x74021000`, con las mismas dos advertencias
que en el Redmi 13C (`EXEC_INIT EPERM`, `unknown gpu_id`) antes de aplicar los fixes de Fase 5/6. **La
fase de descubrimiento del dispositivo no está afectada por la diferencia de kernel** — solo lo está el
envío de jobs (stride). Es una separación clara y útil: la parte de detección del proyecto es portable
tal cual; la parte de envío de jobs no.

## Hallazgo 3 — `gpu_variant_fix` y `exec_init_order_fix` son 100% portables

Ambos parches, diseñados y probados originalmente en kernel 4.19, funcionaron sin modificación alguna
en kernel 6.6.118:
- Tras aplicarlos, `"Mali unknown 0x74021000 MC2"` → `"Mali-G52 r1 MC2"` (reconocimiento correcto del modelo).
- La advertencia `KBASE_IOCTL_MEM_EXEC_INIT failed: Operation not permitted` desaparece por completo.

## Hallazgo 4 — Bug en `apply_kbase_patch.py`: `core_req` desactualizado respecto al README

El script aplica `core_req=0x04` para la cadena vertex/tiler/compute. Pero la Fase 4 del propio README
de LukeValen documenta que el valor que en verdad funcionó fue `0x16` (`T|CS|V` combinado:
Tiler `0x04` + Compute `0x02` + Value-writeback `0x10`). `0x04` solo trae el bit de Tiler — insuficiente.
Parece un descuido del repo (script no sincronizado con la documentación final), no algo específico de
este dispositivo. Corregido con un `sed` puntual antes de continuar.

## Hallazgo 5 — Falso positivo por `assert()` silencioso en builds `release`

`-Dbuildtype=release` define `NDEBUG`, lo cual convierte cada `assert(!ret)` del código del driver
parcheado en un no-op. Resultado observado: `vkQueueSubmit ok` y `vkQueueWaitIdle ok` se imprimían
**aunque el job hubiera fallado realmente** (fault de hardware silenciado). Sin instrumentación
adicional (agregamos `fprintf(stderr, ...)` con el `event_code` real), cualquier build hecho con esta
misma configuración reportaría éxito incorrectamente. Vale la pena que el proyecto documente esto como
advertencia metodológica para cualquiera que reproduzca las pruebas.

## Hallazgo 6 — Compute real: `JOB_CONFIG_FAULT` (0x40), distinto al fault del G52 original

Con stride=64 y `core_req=0x16` ya corregidos, y la instrumentación de depuración activa:

```
PANVK_KBASE_DEBUG: submitting jc=0x5f7fd1040 core_req=0x16
PANVK_KBASE_DEBUG: poll returned 1, revents=0x1
PANVK_KBASE_DEBUG: read 24 bytes, event_code=0x40 atom_number=0
```

`0x40 = JOB_CONFIG_FAULT`. Es un fault **distinto y más temprano** en la tubería que el
`0x58 DATA_INVALID_FAULT` que encontró LukeValen en su Fase 3 con el Redmi 13C: `0x58` significa que el
job fue aceptado y ejecutado pero el dato no aplicó bien; `0x40` significa que el job fue **rechazado en
la etapa de validación**, antes de tocar hardware. No reportado todavía en ningún hilo conocido.

Réplica con un job `WRITE_VALUE` simple (mismo que usó LukeValen para su Fase 3), adaptado a stride=64:
mismo resultado, `event_code=0x40`. Confirma que el problema no es específico del compute complejo del
driver — ocurre incluso con el job más simple posible, una vez expandido a 64 bytes.

### Verificación byte a byte (descarta un bug de offset simple)

Volcado del buffer antes y después del envío son **idénticos** — el GPU nunca llegó a ejecutar nada, ni
escribió evidencia de intento. Se verificó el contenido contra las macros `*_pack` reales generadas por
Meson para este build (`v7_pack.h`):

- Header (bytes 0–31): `type=2` (WRITE_VALUE), `index=1`, `invalidate_cache=1`, `next=0`,
  `exception_status=0` — todo coincide exactamente con lo que el código pidió escribir.
- Payload (bytes 32–63): `address=0x7ffffe100` (el `target_addr` correcto), `type=3` (`ZERO`),
  `immediate_value=0x1234567890ABCDEF` — también coincide exactamente.

**Conclusión: la codificación de bytes es correcta.** El fault no es un simple error de offset del lado
de userspace.

### Hipótesis del reordenamiento — investigada y refutada

La hipótesis inicial (que el fork G57 sugería vagamente: un `base_jd_atom` de 64 bytes con campos
reordenados respecto al de 48) **se descartó con una bisección directa**. Metodología: con un atom
dependency-only (`core_req` intacto vs. puesto en distintos offsets candidatos), solo escribir en el
**byte 44** cambia el resultado frente al baseline (`DONE` instantáneo con todo en cero). Escribir el
mismo valor en 48, 52, 56 o 60 no tiene ese efecto por sí solo. **Conclusión: `core_req` sigue en el
byte 44, sin reordenar.** El struct de 48 bytes original permanece intacto; lo que cambia es que los
kernels/DDK más nuevos **agregan bytes al final** (48→56 o 48→64), no que reorganicen lo existente.

Se descartó también, por prueba directa, la hipótesis de que `compat_core_req` (bytes 34-35, campo de
16 bits del struct v2 original) deba coincidir con `core_req`: escribir el mismo valor ahí no cambió el
resultado (`0x40` en ambos casos).

### Campo real localizado en la cola de 64 bytes: offset 52-55

Bisección palabra por palabra (0 vs. `0xFFFFFFFF`) sobre los 16 bytes agregados (48-63), manteniendo un
job `WRITE_VALUE` real y válido en el resto del atom:

```
byte 48-51 -> sin efecto (0x40 con cualquiera de los dos valores)
byte 52-55 -> ÚNICO offset con efecto: 0x00000000 -> 0x40 (fault instantáneo)
                                        0xFFFFFFFF -> timeout (sin evento en 1.5s)
byte 56-59 -> sin efecto (0x40 con cualquiera de los dos valores)
byte 60-63 -> sin efecto (0x40 con cualquiera de los dos valores)
```

**El único campo real y activo en la extensión de 64 bytes está en los bytes 52-55.** Los demás son
relleno sin efecto observable.

## Fase 2 — Ingeniería inversa contra el binario real del kernel

Sin documentación pública que resolviera el campo del offset 52-55, se localizó y analizó estáticamente
(sin root, sin cargar el módulo) el driver `mali_kbase` real compilado para UAPI 11.46: el archivo
`mali_kbase_mt6768_r49.ko`, extraído de las propiedades del propio dispositivo Oppo A38 e identificado
por nombre, y descargado desde el kernel fuente publicado de otro dispositivo con el mismo SoC
(`linx3141/android_device_bluefox_NX1-kernel`, kernel 6.6.82 — no es el kernel exacto del A38, pero sí la
misma revisión de DDK, que es lo que determina el layout del struct). Confirmado vía
`readelf -p .modinfo`: `version=r49p1-03bet0 (UK version 11.46)` — coincidencia exacta de UAPI.

Análisis con `nm`/`objdump -dr` (símbolos no eliminados, reubicaciones intactas):

- **Tamaño real del struct: 56 bytes, no 64.** En `kbase_jd_submit`, la instrucción justo antes de la
  copia real del atom (`bl _copy_from_user`) es `mov w2, #0x38` (56 en decimal). Esto explica de forma
  retroactiva y consistente los tres hallazgos empíricos anteriores: por qué la bisección de stride
  aceptó 56 y 64 por igual (el kernel solo lee los primeros 56 bytes de cualquier stride declarado ≥56);
  por qué el byte 52-55 tuvo efecto (es el último campo real del struct); y por qué 56-63 no tuvo ningún
  efecto (nunca se copian).
- **Máscara real de `core_req`: `0x61F`.** En `jd_run_atom`, `core_req` vive en el offset interno `0x124`
  del struct del atom, y se enmascara contra `0x61F` para decidir si el atom se completa sin tocar
  hardware — confirma con un valor concreto lo que Luke describió en abstracto sobre
  `BASE_JD_REQ_ATOM_TYPE`.
- **Bit 17 de `core_req` dispara clasificación de "renderpass".** Dentro de `kbasep_js_add_job`, el bit
  17 (`0x20000`) activa una rama de código que compara un campo interno del atom contra el mismo campo
  del atom del que depende (`pre_dep`) — el patrón clásico de emparejar atoms de la misma pasada de
  renderizado incremental, consistente con el "renderpass_id" que mencionó el fork G57. **Confirmado en
  código real, no en memoria/recuerdo.** Sin embargo, nuestro job de prueba tiene ese bit en cero y no
  configura dependencias — por lo que esta rama se salta por completo en nuestro caso; no explica
  directamente nuestro fault.
- **Ningún software rechaza nuestro atom específico.** Se rastreó el camino de código completo que
  recorre un job como el nuestro (`core_req=0x10`, sin dependencias, sin bits de renderpass):
  `jd_run_atom` → `kbasep_js_add_job` (retorna éxito limpio, sin ninguna llamada de error) →
  `kbase_backend_run_atom` (solo gestiona el ring buffer de la cola de hardware) →
  `kbase_backend_slot_update` → `kbase_job_hw_submit`. Esta última función arma los registros de
  hardware (`JS_HEAD`/`JS_AFFINITY`/`JS_CONFIG`) a partir de `core_req` y los escribe directo a la GPU
  vía `kbase_reg_write32/64` — **sin ninguna validación explícita de tipo de job en el camino**. El único
  `dev_err` en toda esta cadena de funciones corresponde a un timeout de lectura de registro, no a
  contenido inválido.

**Conclusión de esta fase:** no se encontró ningún punto de software, en toda la ruta desde el ioctl
hasta la escritura de registros de hardware, que rechace nuestro atom específico. Esto inclina fuertemente
la balanza hacia que `JOB_CONFIG_FAULT` sea un **fault genuino de hardware** — la GPU misma rechazando la
configuración ya recibida — y no una validación de software que se pudiera seguir leyendo en este
binario. Llegar más allá de este punto requeriría el manual de arquitectura de silicio de ARM para esta
variante exacta de Mali-G52 (no público), o una comparación directa contra un kernel que sí funcione.

### Confirmación oficial: el struct de 56 bytes ya era correcto

Un tercer proyecto (`Noysz/panvk-g99-jm`, investigando G57/v9 con el mismo UAPI 11.46) señaló que el
driver de kernel `mali_kbase` lo publica ARM bajo GPLv2 — el propio kernel, separado del blob de
userspace cerrado. Localizado el header oficial correspondiente (`jm/mali_base_jm_kernel.h`, vía un
mirror público del código fuente de ARM) y confirmado `struct base_jd_atom_v2` campo por campo:

```
jc            @ 0   (8 bytes)
udata         @ 8   (16 bytes)
extres_list   @ 24  (8 bytes)
nr_extres     @ 32  (2 bytes)
jit_id[2]     @ 34  (2 bytes)   — reemplazó a "compat_core_req" desde UK 11.20
pre_dep[2]    @ 36  (4 bytes)
atom_number   @ 40  (1 byte)
prio          @ 41  (1 byte)
device_nr     @ 42  (1 byte)
jobslot       @ 43  (1 byte)    — nuevo, solo relevante con BASE_JD_REQ_JOB_SLOT
core_req      @ 44  (4 bytes)
renderpass_id @ 48  (1 byte)    — nuevo, solo relevante con START/END_RENDERPASS
padding[7]    @ 49  (7 bytes)   — "Unused. Must be zero."
──────────────────────────────
TOTAL: 56 bytes
```

**Esto confirma, con fuente oficial de ARM y no solo ingeniería inversa, que el struct de 56 bytes y la
posición de `core_req` en el byte 44 ya eran correctos.** También aclara dos cosas que quedaron mal
identificadas durante la sesión de ingeniería inversa:

- El bit 17 de `core_req` que se observó en `kbasep_js_add_job` **no es renderpass** — es
  `BASE_JD_REQ_JOB_SLOT` (selección explícita de slot de hardware). El renderpass real usa los bits 18
  y 19 (`BASE_JD_REQ_START/END_RENDERPASS`), que nuestro job nunca activó.
- El "campo real" que la bisección localizó en los bytes 52-55 **no corresponde a `renderpass_id`**
  (que es un solo byte en el offset 48, no una palabra en 52-55) — cae dentro del relleno documentado
  como "debe ser cero". El cambio de comportamiento observado al escribirle `0xFFFFFFFF` fue,
  casi con certeza, comportamiento indefinido por violar relleno reservado, no un campo funcional real.
  Con el relleno correctamente en cero (como estuvo en la mayoría de las pruebas), el atom estaba
  100% bien formado desde el principio.

**Conclusión de esta fase (superada por el hallazgo siguiente):** con el struct de userspace confirmado
correcto en su totalidad, el `JOB_CONFIG_FAULT` no podía explicarse por un error de formato del atom —
en ese momento de la investigación esto apuntaba a un fault de hardware/silicio como límite final. **No
lo era.** La causa real se encontró justo después: ver "Causa raíz encontrada y corregida" más abajo.

## Causa raíz encontrada y corregida: el `stride` declarado, no el contenido del atom

Para cerrar la investigación de forma reproducible, se construyó `definitive_test.c`: una prueba que usa
el struct `base_jd_atom_v2` **oficial**, tal cual lo define ARM (nombres de campo reales, sin un solo
offset calculado a mano, con `_Static_assert(sizeof(...) == 56)` para atrapar en tiempo de compilación
cualquier discrepancia). Con `stride = sizeof(struct base_jd_atom_v2)` (56, calculado por el compilador,
no hardcodeado), el resultado fue:

```
event_code=0x58 (BASE_JD_EVENT_DATA_INVALID_FAULT)
```

**No `0x40`.** `0x58` es un fault real de hardware — el job llegó a la GPU, se ejecutó, y el dato no
aplicó bien — exactamente el mismo código que documentó LukeValen en su propia Fase 3, el paso
*anterior* a lograr renderizado real en su dispositivo. Para aislar la variable de forma inequívoca, se
repitió la prueba cambiando **únicamente** el `stride` de 56 a 64 (todo lo demás — struct, campos,
valores — bit a bit idéntico):

```
stride=56 -> event_code=0x58  (DATA_INVALID_FAULT — el job SÍ llega a hardware)
stride=64 -> event_code=0x40  (JOB_CONFIG_FAULT   — rechazado antes de tocar hardware)
```

**El `stride` declarado en el ioctl, por sí solo, decide si el kernel toma la ruta de envío real a
hardware o una ruta de rechazo temprano** — no el contenido del atom, que era idéntico en ambas
pruebas. El valor `64` que se usó durante gran parte de la sesión (heredado sin cuestionar del fork G57,
cuyo struct de 64 bytes es válido para *su* UAPI pero no necesariamente para todas las revisiones)
resultó ser exactamente la causa que nos mantuvo atascados. El valor correcto para UAPI 11.46 en este
dispositivo es el tamaño real y oficial del struct: **56**.

### Confirmado con el driver real, no solo con la prueba aislada

Se aplicó el mismo fix (`stride = 64` → `stride = 56`) directamente en el driver PanVK ya parcheado
(`panvk_vX_gpu_queue.c`, función `panvk_kbase_submit_and_wait`), se recompiló, y se repitió la prueba de
compute real de Vulkan (`panvk_compute_test.c` — pipeline completo: `vkCreateInstance` →
`vkCreateComputePipelines` → `vkQueueSubmit` → `vkQueueWaitIdle` → lectura del buffer):

```
PANVK_KBASE_DEBUG: submitting jc=0x5f7fd1040 core_req=0x16
PANVK_KBASE_DEBUG: read 24 bytes, event_code=0x1 atom_number=1
buffer after dispatch, first u32=0x00000309 (expected 0x00000309 = 777)
```

**`event_code=0x1` (`DONE` real, no el falso positivo de `assert()` silenciado que documentamos antes) y
el buffer con el valor exacto esperado (777).** Compute real de GPU, verificado de punta a punta a
través de PanVK/Vulkan real — no un harness aislado — en un Mali-G52 MC2 con kernel 6.6.118-android15 y
UAPI 11.46. Es el mismo hito que logró LukeValen en el Redmi 13C (kernel 4.19.191, UAPI 11.38),
reproducido de forma independiente en un segundo dispositivo con una revisión de DDK distinta, gracias a
la combinación de dos fixes: `core_req=0x16` (Hallazgo 4, corregido antes en esta sesión) y
`stride=56` (este hallazgo).

### Recomendación: no volver a hardcodear el stride

El valor correcto (56) es específico de esta UAPI/DDK — el propio Redmi 13C usa 48. Hardcodear
cualquier valor fijo es frágil por diseño; es literalmente el mismo tipo de error que causó esta
investigación completa, solo que con un número distinto. La recomendación para el proyecto es un probe
en tiempo de ejecución: al abrir `/dev/mali0`, enviar un atom inofensivo (`core_req=0`, no toca
hardware) con candidatos de stride en orden ascendente, y quedarse con el primero que el kernel acepte
sin `EINVAL` — ya confirmado empíricamente que el kernel valida el stride contra un conjunto de tamaños
conocidos, no contra "cualquier valor lo bastante grande" (nuestra propia bisección rechazó 60 pero
aceptó 56 y 64). Esto haría el driver portable entre kernels sin volver a tropezar con este mismo bug.

### El intento de conseguir un `.ko` del Redmi 13C (UAPI 11.38)

Se buscó, sin éxito, un módulo `mali_kbase` equivalente para la revisión de DDK del Redmi 13C (UAPI
11.38), con la intención original de comparar directamente el código de validación entre ambas
versiones. Xiaomi no ha publicado el código fuente del kernel para el Redmi 13C 4G (nombre en clave
`gale`/`gust`) — confirmado por solicitudes de la comunidad abiertas y sin responder en el rastreador de
issues de `MiCode/Xiaomi_Kernel_OpenSource` en GitHub. Con la causa raíz ya resuelta por otra vía, esto
dejó de ser necesario para cerrar la investigación.

## Fases 4-9: replicando la metodología de Luke (Oppo A38)

Con el bug del stride resuelto, se replicó el resto del recorrido documentado por Luke en su propio
repositorio, fase por fase, con resultados prácticamente idénticos:

- **Fase 4 (compute real vía PanVK)**: confirmado — ver hallazgo anterior.
- **Fase 5 (reconocimiento de modelo)** y **Fase 6 (orden EXEC_INIT/JIT_INIT)**: mismos patches de Luke
  (`gpu_variant_fix.patch`, `exec_init_order_fix.patch`), aplicados sin modificación, mismos resultados
  (`"Mali-G52 r1 MC2"`, advertencia de `EXEC_INIT` desaparecida).
- **Fase 7 (draw call offscreen real)**: `panvk_draw_test.c` de Luke, sin tocar una sola línea —
  triángulo dibujado, píxel central `RGBA(255,0,0,255)` exacto, igual que su resultado documentado.
- **Fase 8 (WSI/X11)**: omitida a propósito — no es prerrequisito del WSI de Android, que es el objetivo
  real para Winlator.
- **Fase 9 (WSI Android / AHardwareBuffer)**: `android_wsi_ahb_fixes.patch` de Luke aplicado limpio.
  Compilar el build de plataforma Android sí exigió un fix adicional que Luke no documentó (ver
  siguiente sección). Una vez resuelto, mismo resultado que el suyo — `AHardwareBuffer_allocate()`
  contra `libnativewindow.so` real del sistema, `vkGetAndroidHardwareBufferPropertiesANDROID()` contra
  el driver, devolviendo valores correctos (`allocationSize=16384`, exacto para un buffer RGBA8 de
  64×64; `memoryTypeBits=0x5`, distinto de cero).

### Hallazgo nuevo: binarios de build desalineados para el cargador de Bionic

Al compilar por primera vez con `-Dplatforms=android`, varias herramientas de codegen usadas durante el
propio build (`mesa_clc`, `vtn_bindgen2`, `panfrost_compile` — generan código a partir de kernels
OpenCL/SPIR-V como parte del proceso de compilación, no parte del driver final) fallaban al *ejecutarse*
con `error: executable's TLS segment is underaligned... needs to be at least 64 for ARM64 Bionic`. Es un
problema de toolchain documentado en el ecosistema Termux desde hace años (mismo error reportado en
paquetes no relacionados como Valgrind, PHP, Go), sin una solución limpia a nivel de banderas de
compilación — se resuelve parchando el ELF ya compilado, corrigiendo tanto `p_align` como el "skew"
(`p_vaddr % 64`) del segmento `PT_TLS`, no solo `p_align` (una corrección parcial deja el binario
igual de roto, con un error de "skew" distinto). Aparte, `AHardwareBuffer_acquire`/`_release` (API 26+)
aparecían como "no disponibles" pese a compilar con el nivel de API correcto declarado — el chequeo de
disponibilidad de Clang se rige por el *target triple* del compilador, no por la macro `__ANDROID_API__`
que se puede pasar con `-D`; hubo que forzar `-target aarch64-linux-android35` explícitamente.

No documentado por Luke en su propio repositorio — posible diferencia de versión de toolchain entre su
sesión y esta, o que ya tuviera esas herramientas compiladas de una sesión previa sin volver a
enlazarlas desde cero.

## Más allá del punto documentado por Luke: `panvk_android_import_ahb_memory()` corregido

Luke dejó explícitamente pendiente en su propio commit: *"Not yet done: `panvk_android_import_ahb_memory()`
in `panvk_android.c` still assumes `data[0]` for the actual `vkAllocateMemory` import path — same class
of bug, same fix needed there next."* Se aplicó el mismo patrón que él ya usó y validó en
`vk_common_GetAndroidHardwareBufferPropertiesANDROID` (`src/vulkan/runtime/vk_android.c`): en vez de
asumir que el fd del dma-buf vive en `handle->data[0]`, probar cada fd con `lseek(SEEK_END)` (que solo
tiene éxito sobre un dma-buf real) y quedarse con el primero que lo confirme.

Con instrumentación de depuración añadida para verificarlo sin ambigüedad, el resultado en este
dispositivo:

```
dma_buf_index=1 (de 3 fds) dma_buf_fd=6 dma_buf_size=16384 data[0]=5
vkAllocateMemory (import AHB real) ok
```

**`data[0]` (fd=5) no era el dma-buf real** — confirma exactamente el mismo hallazgo de hardware que
hizo Luke en su propio dispositivo, ahora reproducido de forma independiente en un Mali-G52 con un
gralloc fallback distinto. El dma-buf real estaba en `data[1]` (fd=6, 16384 bytes — coincide exacto con
el tamaño esperado del buffer RGBA8 de 64×64). Sin el fix, `panvk_android_import_ahb_memory()` habría
intentado usar el fd equivocado y fallado. Con el fix, `vkAllocateMemory` importando el
`AHardwareBuffer` real tuvo éxito de punta a punta — primera vez que se confirma este camino específico
en cualquiera de los proyectos relacionados (G52 o G57).

## Segundo bug encontrado y corregido: `DRM_FORMAT_MOD_INVALID` no manejado en `panvk_image_get_explicit_mod()`

Con `panvk_android_import_ahb_memory()` ya corregido, el siguiente paso natural era confirmar que la
memoria importada sirve para algo real: enlazarla a una `VkImage` de verdad (`vkCreateImage` +
`VkExternalMemoryImageCreateInfo` + `vkAllocateMemory` con `VkMemoryDedicatedAllocateInfo`) y
renderizar algo en ella. Esto activa una ruta de código completamente distinta a la ya probada
(`panvk_android_ahb_image_init()` → `vk_android_get_ahb_layout()` → código compartido de Mesa), nunca
antes ejercida por ninguno de los proyectos relacionados — el primer intento terminó en
`Segmentation fault` (`exit code 139`).

Se rastreó la causa con el mismo método usado para el bug del `stride` al principio de la sesión:
instrumentación de depuración añadida nivel por nivel, marcador por marcador, hasta aislar la línea
exacta. La cadena completa: `panvk_android_ahb_image_init()` → `vk_android_get_ahb_layout()` (código
compartido de Mesa, no específico de PanVK) → `vk_gralloc_to_drm_explicit_layout()` — todas confirmadas
correctas, devolviendo datos sensatos (`num_planes=1`, `stride=256`, formato `DRM_FORMAT_ABGR8888`) —
hasta llegar a `panvk_image_init()` → `panvk_image_get_explicit_mod()`, donde:

```c
uint64_t mod = explicit->drmFormatModifier;   // = DRM_FORMAT_MOD_INVALID
...
assert(panvk_image_can_use_mod(image, iusage, mod, false));  // casi seguro falla, pero es no-op en release
return mod;   // devuelve DRM_FORMAT_MOD_INVALID de todas formas
```

**Sin gralloc de fabricante real** (`"No gralloc hwmodule detected"`, presente en cada corrida de esta
sesión), el fallback de Mesa reporta `DRM_FORMAT_MOD_INVALID` como modifier — un valor centinela legítimo
que significa "no se conoce un modifier de tiling real", pero que esta función nunca esperaba recibir: no
hay ningún `if` que lo intercepte, solo un `assert()` como única defensa — y es exactamente el mismo
patrón de `NDEBUG` silenciando asserts que ya se documentó como Hallazgo 5 al principio de la sesión,
ahora encontrado por segunda vez, en un lugar completamente distinto del driver.

**Fix aplicado:** sustituir `DRM_FORMAT_MOD_INVALID` por `DRM_FORMAT_MOD_LINEAR` (la convención estándar
de "sin tiling", universalmente soportada) antes de usarlo:

```c
if (mod == DRM_FORMAT_MOD_INVALID)
   mod = DRM_FORMAT_MOD_LINEAR;
```

**Resultado, con el fix aplicado — validación completa de punta a punta:**

```
vkCreateImage (respaldada por AHB) ok
vkAllocateMemory (dedicada, ligada a VkImage) ok
vkBindImageMemory ok
vkQueueSubmit ok
vkQueueWaitIdle ok
pixel central despues del clear: R=0 G=0 B=255 A=255 (esperado: 0,0,255,255 = azul)
```

Exacta coincidencia. Una `VkImage` respaldada por `AHardwareBuffer` importado, limpiada a azul puro por
un comando real de GPU y leída de vuelta correctamente — la primera confirmación, en cualquiera de los
proyectos relacionados (G52, G57, G99, G720), de que una imagen respaldada por AHB es un render target de
GPU genuinamente funcional a través de este camino.

### ⚠️ Corrección importante: el fix de `DRM_FORMAT_MOD_LINEAR` no está confirmado como correcto

El proyecto G720 (`wonderkast02/panvk-g720-kbase-csf`), bastante más avanzado que este trabajo, incluye
en su propio README la advertencia explícita: *"Nunca convertir `DRM_FORMAT_MOD_INVALID` en
`DRM_FORMAT_MOD_LINEAR` por suposición."* — exactamente el fix aplicado arriba. La prueba que lo validó
(limpiar la imagen a un color sólido y leerlo de vuelta) es demasiado simple para detectar un layout
incorrecto: al leer a través del mismo driver que hizo la suposición, el resultado es consistente consigo
mismo incluso si `LINEAR` no es en realidad el layout que el hardware o un consumidor externo (una
textura real con mipmaps, un formato comprimido, o cualquier cosa fuera de este mismo proceso) esperaría.
**Este hallazgo queda marcado como fix tentativo, no como solución confirmada** — desbloqueó el
progreso y permitió llegar hasta aquí, pero la causa raíz de por qué `LINEAR` sería incorrecto (y cuál
sería la sustitución correcta) no estaba investigada en ese momento (ver el seguimiento más abajo).

## Seguimiento: medir el handle de gralloc en vez de suponer (parche 0004)

La advertencia del proyecto G720 era correcta, y la pregunta abierta resultó ser medible: qué produce realmente el allocator y dónde lo dice el handle.

**Hacia dónde apuntaba la investigación.** [JimVulkan/mesa-unified-drivers](https://github.com/JimVulkan/mesa-unified-drivers) interpreta directamente el handle de gralloc de ARM. Un "alloc format" de 64 bits guarda el formato HAL en la palabra baja y los bits de layout (AFBC) en la alta; el allocator de MediaTek de un Galaxy A31 se comparó contra `0x311`. Solo conserva `LINEAR` como respaldo para handles que no reconoce. Su parser busca la información del plano en magic+15 (Exynos) o magic+21 (A31). En este teléfono registra "unknown handle layout" (Winlator, `mali-panvk` r3), porque aquí está en magic+16.

**Método.** Programas pequeños alocan `AHardwareBuffer`s con el allocator del sistema y vuelcan el handle nativo (`AHardwareBuffer_getNativeHandle`): una primera prueba (`tests/panvk-driver-patch/ahb_handle_probe.c`: 64x64 y 500x500, tres usages) y `tests/panvk-driver-patch/ahb_probe2.c` (200x100, RGBA8 y RGBX8, ocho combinaciones de usage).

**Layout del handle en el A38** (3 fds, 62 ints). fd0 no reporta tamaño (`-1`; el código de JimVulkan indica que MediaTek pone `-1` primero), fd1 es el buffer, fd2 es una región de 6480 bytes en todas las muestras. Entre los ints: magic `0x3141592` en `ints[18]`, el alloc format (u64) en `ints[29..30]` y `{stride en bytes, ancho, alto}` del plano 0 en `ints[34..36]`. Otros ints también cambian en los buffers comprimidos (por ejemplo `ints[14]` e `ints[30]`); no se decodificaron.

**Resultados** (200x100):

| usage | alloc format (palabra alta) | tamaño del buffer | layout |
|---|---|---|---|
| solo `GPU_SAMPLED` | `0` | 86016 | lineal |
| `SAMPLED + COLOR` con lectura CPU, escritura CPU o `COMPOSER_OVERLAY` | `0` | 86016 | lineal |
| solo `COLOR`, `SAMPLED + COLOR`, `SAMPLED + COLOR + NO_AFBC` | `0xc410000` | 135168 | comprimido |

La palabra alta separa los dos casos en 16 de 16 muestras (RGBA8 y RGBX8 dieron resultados idénticos). La primera prueba muestra el mismo patrón: 64x64 con lectura CPU son 16384 bytes y solo-GPU son 69632 (128x128x4 + 4096); 500x500 son 1024000 frente a 1064960 (512x512x4 + 16384).

La aritmética cuadra: el buffer lineal tiene 208 px x 4 = 832 bytes de stride, 83200 bytes, redondeados a 21 páginas. El otro es 256x128 (alineado a 128) x 4 = 131072 bytes de cuerpo más 4096 de header, consistente con AFBC (superbloques de 16x16, headers en tiles). Las flags del modifier **no** se verificaron. El bit privado `NO_AFBC` (`1 << 29`) no tiene efecto en este allocator; basta el uso de CPU o del composer para que el buffer sea lineal, lo que coincide con las notas de JimVulkan sobre el allocator de MediaTek.

**El parche 0004** (`src/util/u_gralloc/u_gralloc_fallback.c`) actúa solo cuando el handle encaja con esa huella (3 fds, 62 ints, magic en `ints[18]`). Palabra alta `0` y stride del plano igual a `pixel_stride x bpp` dan `DRM_FORMAT_MOD_LINEAR`. Una palabra alta distinta de cero se rechaza con un error explícito. Cualquier otro handle se comporta como antes.

Verificado en el dispositivo:

```
info.modifier=0x0
pixel central despues del clear: R=0 G=0 B=255 A=255        (panvk_ahb_image_test, uso de CPU + composer)

MESA: error: ARM gralloc: compressed buffer (alloc format 0xc410000:00000001) is not handled
FAILED vkAllocateMemory ...: VkResult=-1000072003            (panvk_ahb_image_test_gpuonly, VK_ERROR_INVALID_EXTERNAL_HANDLE)
```

**Callejones sin salida y correcciones.**
- Se sugirió que el buffer del test original probablemente era AFBC y que por eso `LINEAR` estaba mal. No lo era: el test pide `CPU_READ_RARELY`, que mantiene el buffer lineal. El parche 0003 acertaba para ese buffer, solo que sin demostrarlo.
- Se consideró la interposición del símbolo `hw_get_module` dentro de un proceso Android real como la razón por la que el gralloc de otro proyecto funcionaba. Descartado: ese driver también registra "Using fallback gralloc implementation" dentro de Winlator.
- Añadir `NO_AFBC` al usage del test era innecesario en este allocator (ver arriba).

**Lo que sigue siendo una suposición.**
- El parche 0003 sigue siendo necesario cuando quien llama pasa un `DRM_FORMAT_MOD_INVALID` explícito, como hace el wrapper de Winlator según el README de LukeValen. En ese camino no hay handle que inspeccionar, y `LINEAR` sería incorrecto para un buffer solo-GPU.
- La huella está confirmada solo en el A38.
- La evidencia de "lineal" es la correlación medida; la lectura del pixel sigue pasando por el mismo driver. Falta una comprobación independiente con `AHardwareBuffer_lock()`.
- Los buffers comprimidos se rechazan, no se soportan.

**Observaciones relacionadas en el mismo dispositivo** (detalle en el README, "Field notes"): el driver de FristOneRR elige de forma independiente el stride 56 en el A38 y renderiza cubos de prueba de D3D9 a D3D11 en Winlator; el README de LukeValen documenta el modifier `INVALID` que pone el wrapper y un arreglo de `SYNC_FD`; las notas de JimVulkan dicen que el kernel de MediaTek rechaza los ioctls de sync de DRM.

## Estado: investigación activa

Con esto, este reporte no solo alcanza paridad con el estado público del proyecto de Luke — lo supera en
un punto concreto y verificado. El seguimiento de gralloc se apoya en el análisis previo de JimVulkan sobre el layout del handle de ARM; lo nuevo aquí es la tabla de mediciones del A38 y la huella del handle. Otros proyectos ya han acreditado o reproducido de forma independiente partes de este trabajo (ver README, "Field notes" y "Credits").

## Próximos pasos sugeridos

1. **Reportar el fix del stride a LukeValen y al mantenedor de `apply_kbase_patch.py`** (el driver de FristOneRR ya sondea el stride en ejecución y acredita este repo) — es el hallazgo más
   importante de toda la sesión: el valor `64` (copiado del fork G57 sin verificar contra el tamaño real
   del struct para esta UAPI) es la causa del `JOB_CONFIG_FAULT`, no ningún problema de hardware. Vale
   la pena implementar el probe dinámico sugerido arriba en vez de fijar 56 como nuevo valor hardcodeado.
2. Hecho: la Fase 7 de Luke (draw call real, offscreen) se replicó en este dispositivo sin cambios (ver "Fases 4-9" más arriba).
3. Reportar el bug de `core_req=0x04` en `apply_kbase_patch.py` al mantenedor (ya corregido a `0x16`
   para esta sesión).
4. Recomendar añadir una nota en el README sobre el efecto de `NDEBUG` en los `assert()` de verificación
   — sin la instrumentación manual que agregamos, este mismo fix habría sido invisible (el build en
   `release` reportaba éxito incluso con el `stride` incorrecto).

5. Verificar el caso lineal de forma independiente (`AHardwareBuffer_lock()` sobre un buffer legible por CPU) en vez de leer a través del mismo driver.
6. Decidir qué hacer con un `DRM_FORMAT_MOD_INVALID` explícito que ponga un wrapper (el parche 0003 sigue siendo una suposición ahí); una opción es comprobar el handle importado cuando se enlaza la memoria.
7. Compartir el layout del handle del A38 (plano en magic+16, alloc format en magic+11, la tabla de usages) con los proyectos cuyos parsers no lo reconocen.

## Entorno de referencia (para reproducir)

- Termux, clang 21.1.8, meson 1.12.0, ninja 1.13.2, libclc 21.1.3-1
- `meson setup build -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dbuildtype=release -Dbuild-tests=false -Dpanfrost-kmds=kbase,panthor`
- `ninja -C build -j1` (o `-j4` en rebuilds incrementales pequeños)
- `libvulkan_panfrost.so` resultante: ~20MB, 1014 targets, sin errores
- Fixes aplicados sobre `funnymdzz/mesa`: `core_req` de la cadena vertex/tiler/compute corregido de
  `0x04` a `0x16`; `stride` de `KBASE_IOCTL_JOB_SUBMIT` corregido de `64` a `56`
- Build de plataforma Android (`-Dplatforms=android`): ver el README, "Build configuration that worked".
