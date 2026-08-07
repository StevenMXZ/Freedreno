# freedreno sobre KGSL no alvo GLX xlib (Winlator/bionic) — conjunto mínimo

12 patches, agrupados por diretório. É o conjunto reduzido do branch
`freedreno-dri3-present` (repo `C:\Users\Sidney-PC\Projetos\mesa`): 31 commits
originais, dos quais só o necessário para o **freedreno compilar e funcionar no
alvo xlib** foi mantido. O resultado está no branch `freedreno-xlib-minimal`
(commit único, a partir do merge-base `780727e68ad`).

Verificado com `git apply --check` contra o HEAD desta árvore, um a um e todos
juntos: limpo.

## Aplicar

```bash
while read p; do git apply "patches/$p"; done < patches/series
```

## Build

O caminho completo (NDK, sysroot Termux, `libandroid-sysvshm`, cross file,
patches, compilação, empacotamento e verificação) está em `patches/build.sh`,
que é o mesmo script que o workflow do GitHub Actions executa:

```bash
patches/build.sh all
```

Sai `opengl_freedreno.tzst` na raiz da árvore. `patches/build.sh deps` instala
só as ferramentas de host (precisa de apt/sudo) e `patches/build.sh build` faz o
resto sem root.

Configuração manual equivalente:

```bash
meson setup build --cross-file cross-android-aarch64.ini --buildtype release -Db_ndebug=true -Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=kgsl -Dglx=xlib -Dplatforms=x11 -Dxlib-lease=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=true -Dglvnd=disabled -Dllvm=disabled -Dxmlconfig=disabled -Dzstd=enabled -Dlibunwind=disabled -Dvalgrind=disabled -Dbuild-tests=false -Dandroid-libbacktrace=disabled -Dvideo-codecs=
```

Sai `src/gallium/targets/libgl-xlib/libGL.so` → renomear para `libGL.so.1`
(soname via patchelf, remover rpath). Build a partir de clone nativo do WSL,
nunca de `/mnt/c` (CRLF quebra os shebangs Python).

## Os patches

| # | patch | o que faz |
|---|-------|-----------|
| 01 | `meson.build` | libera `glx=xlib` com freedreno (antes exigia swrast); detecta `sync_wait`; adiciona `dep_x11_xcb` |
| 02 | `src_util` | `sync_wait()` de fallback via `poll()` quando o NDK não fornece; linka `liblog` no Android independente de `-Dplatforms` |
| 03 | `src_freedreno_drm` | **backend KGSL** (`kgsl/*`), feature bits por backend, reset status direto, abre `/dev/kgsl-3d0` quando o fd recebido não é KGSL, alocação de imagens compartilháveis via gralloc, mensagens de erro nas falhas de probe |
| 04 | `src_freedreno_fdl` | respeita `-Dbuild-tests=false` (senão o build quebra) |
| 05 | `src_freedreno_ir3` | não derreferencia build-id NULL no disk cache |
| 06 | `src_gallium_auxiliary_target-helpers` | `sw_screen_create_named()` aceita `"freedreno"` |
| 07 | `src_gallium_drivers_freedreno` | `fd_screen_create_sw()`, recursos de scanout pelo shared image allocator, present via DRI3 e fallback para o caminho software |
| 08 | `src_gallium_frontends_glx_xlib` | não serializa CPU/GPU a cada swap; ring de 2 back buffers no `xm_st` |
| 09 | `src_gallium_targets_libgl-xlib` | linka `driver_freedreno` + `idep_xmlconfig`; flags bionic (`-landroid-sysvshm`, `-lc++_shared`, `-llog`, `-lsync`, `--undefined-version`) |
| 10 | `src_gallium_winsys_sw_xlib` | apresentação direta por DRI3/Present no winsys xlib |
| 11 | `src_mesa_main` | `check_compatible()` para de comparar depth/stencil ao ligar o drawbuffer |
| 12 | `src_gallium_frontends_glx_xlib-hud` | publica `_MESA_DRV`, `_MESA_DRV_ENGINE_NAME` e `_MESA_DRV_GPU_NAME` no drawable em `glXMakeCurrent`, que é o que revela o HUD do XServer do Winlator. Cosmético — pode ser omitido sem afetar o resto |

## O que foi removido, e por quê

Do branch original saíram 19 commits:

- **zink** (`zink_screen.c`, `zink_resource.c`, `zink_types.h`, e a habilitação
  do zink no alvo xlib) — outro backend; freedreno não precisa. Os comentários
  em 06 e 09 que citavam zink foram reescritos para freedreno.
- **alvo `dri`** (`targets/dri/meson.build`, `dri.sym.in`, `dri_sw_winsys.c`) —
  este build usa `glx=xlib`, o alvo dri nem é construído.
- **HUD no caminho DRI** (`src/glx/glxcurrent.c`) — essa metade existe porque o
  frontend dri aloca um GLXWindow distinto da Window e precisa resolver o
  drawable antes de setar a propriedade. Com `glx=xlib` esse arquivo nem é
  compilado. A metade que importa aqui é o patch 12.
- **`dri_context.c`** (propagar falha de make_current) — frontend dri, fora deste
  alvo; era diagnóstico.
- **CI e docs** (`.github/workflows/*`, `reproduce-*.sh`, `STATUS.md`) — não
  entram no build. O comando de configuração acima veio de
  `reproduce-freedreno-xlib.sh`.
- Um par commit + revert (`freedreno: wait for the frame…`) que se anulava.

Se o HUD for necessário também no caminho dri algum dia, recupere `5c7117e` do
branch `freedreno-dri3-present` — a parte de `src/glx/glxcurrent.c`.
