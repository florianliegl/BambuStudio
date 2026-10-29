# Windows build in this workspace

The local application build uses Visual Studio 2022, MSVC 19.44, the Windows SDK,
and CMake 3.31. The dependency prefix is `build/official-deps/usr/local`.

## Official dependency bundle

The bundle was downloaded from the official Bambu Studio Actions run:

- Run: [31797880268](https://github.com/bambulab/BambuStudio/actions/runs/31797880268)
- Artifact: `9220135690`, `BambuStudio_dep_win64_20260814`
- Source commit: `9a530f77c23d8c3430d1dbef02e103cd8bd6480e` (`v02.08.02.60`)
- Archive SHA256: `55a5fc5b55a3424ed358e31639000fc726d4e4964b524bab128989eaa87754b5`
- Inner archive: `BambuStudio_dep_win64_20260814_vs2022.zip`

The complete `deps/` trees of `v02.08.02.60` and our stable base `v02.08.02.61`
were compared and are identical. The artifact checksum was checked against
GitHub's artifact metadata. The inner archive was extracted into
`build/official-deps`; this does not replace the installed Bambu Studio.

Some exported CMake/pkg-config metadata embeds the GitHub runner's absolute
dependency path. Relocate those text references before configuring the app:

```powershell
python scripts/relocate_ams_dependencies.py build/official-deps/usr/local
```

This changes metadata paths only, not headers or library binaries. Without this
step the linker can look for `freetype.lib` or `zlib.lib` on the CI runner's D: drive.

Although the artifact name ends in `vs2022`, its TBB and Assimp binaries were
built with a newer compiler than the locally installed MSVC. The former produced
an LTCG intermediate-language mismatch and the latter referenced newer STL
helper symbols. For this machine, those two libraries were rebuilt from the
**same pinned source versions** using the installed compiler:

```powershell
cmake -S deps -B deps/build -A x64 -DDEP_DEBUG=OFF
cmake --build deps/build --config Release --target dep_TBB dep_Assimp -- /m:2
Copy-Item deps/build/destdir/usr/local/lib/tbb.lib,`
  deps/build/destdir/usr/local/lib/tbb12.lib,`
  deps/build/destdir/usr/local/lib/tbbmalloc.lib,`
  deps/build/destdir/usr/local/lib/zlibstatic.lib `
  -Destination build/official-deps/usr/local/lib -Force
Get-ChildItem deps/build/destdir/usr/local/lib/assimp*.lib |
  Copy-Item -Destination build/official-deps/usr/local/lib -Force
Get-ChildItem deps/build/destdir/usr/local/lib/cmake/assimp-5.4/*.cmake |
  Copy-Item -Destination build/official-deps/usr/local/lib/cmake/assimp-5.4 -Force
```

No headers or other bundle libraries need replacement for these two rebuilds.
Keep the dependency tree pinned to the stable source rather than substituting
newer library releases.

The final GUI link also required native builds of **wxWidgets, OpenCV, and
OpenVDB**, for the same newer-STL-helper incompatibility. Their libraries were
replaced in the local dependency cache while preserving the SDK headers:

| Library | Matching source and local build |
| --- | --- |
| wxWidgets | `bambulab/wxWidgets` commit `a9d946902685b9946d8775f07d2a73a9b5bef394`; all public headers match the bundle byte-for-byte; generated setup differs only in installation prefix |
| OpenCV | Stable dependency recipe, version 4.6.0; native Python 3.12 selected explicitly for build configuration |
| OpenVDB | Commit `a68fd58d0e2b85f01adeb8b13d7555183ab10aa5`, the stable dependency's clang19 patch, static core target; helper executables disabled |

The wx build was resumed directly in its configured build directory after
selecting that matching revision, avoiding the superbuild's moving `master`
update step. Its 16 release `.lib` files were copied into the cache's
`lib/vc_x64_lib` directory. OpenCV's five `.lib` files were copied into
`x64/vc17/staticlib`. Only the native `libopenvdb.lib` was copied for OpenVDB.
No installed application or system runtime was replaced.

For OpenCV, this environment also needed `OPENCV_PYTHON3_VERSION:STRING=3.12`,
`PYTHON_EXECUTABLE`, `PYTHON3_EXECUTABLE`, and `PYTHON_DEFAULT_EXECUTABLE` pointing
at the native Python executable; otherwise its old detection code selected a
nonworking MSYS interpreter and passed `OFF` as a package version.

The verified OpenVDB archive has SHA256
`f353e7b99bd0cbfc27ac9082de51acf32a8bc0b3e21ff9661ecca6f205ec1d81`.
The direct static-core build used the official dependency prefix and the same
`OPENVDB_CORE_STATIC=ON`, `OPENVDB_CORE_SHARED=OFF`, `USE_BLOSC=ON`, and
`OPENVDB_BUILD_PYTHON_MODULE=OFF` settings as the stable recipe, with
`OPENVDB_BUILD_VDB_PRINT=OFF`. Its output is
`build/native-openvdb/openvdb/openvdb/Release/libopenvdb.lib`.

GitHub artifacts expire. If this bundle is no longer available, build dependencies
from the checked-out stable source using the official Windows compilation guide
and `build_win.bat`. A native Windows Perl is required for OpenSSL; an MSYS/Cygwin
Perl will not configure that native build. The upstream Node/pnpm setup is handled
by the project's own CMake configuration.

## Configure and build

From a Visual Studio developer prompt with CMake on PATH, in the repository root:

```powershell
$dependencyPrefix = (Resolve-Path build/official-deps/usr/local).Path
# Optional: keep the generated build timestamp stable across reconfiguration.
$env:SOURCE_DATE_EPOCH = (git show -s --format=%ct HEAD).Trim()
cmake -S . -B build/extended-ams-app -A x64 `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$dependencyPrefix" `
  -DSLIC3R_BUILD_TESTS=ON
cmake --build build/extended-ams-app --config Release `
  --target BambuStudio_app_gui BambuStudioDllsCopy extended_ams_lite_integration `
  -- /m:2 /p:CL_MPCount=4
```

`BambuStudioDllsCopy` supplies the runtime DLLs. The upstream build also creates
the resource directory junction required by the application. The executable is
`build/extended-ams-app/src/Release/bambu-studio.exe` after a successful build.
An executable's presence alone does not establish that the complete build passed.

Local result: both application and runtime-copy targets completed successfully.
The original application was launched and its normal Setup Wizard was inspected.
For the batch update, screen access was explicitly excluded: the rebuilt app is
checked by a live process, a main window handle and window responsiveness.
Layout and the complete interactive Preview flow remain manual checks. The build retains
upstream warnings, including `LNK4098` about CRT default libraries; no broader
release certification or full aggregate regression pass is claimed.

## Integration check

```powershell
$env:PATH = "$dependencyPrefix/bin/occt;$dependencyPrefix/bin;$env:PATH"
& ./build/extended-ams-app/tests/extended_ams_lite/Release/extended_ams_lite_integration.exe
```

This test exercises the real G-code processor with eight virtual filaments
mapped to one physical extruder. It is separate from the dependency-free planner
tests and does not validate printer firmware.

Local result with the A1 0.2 mm probe: **118 assertions in 4 test cases passed**,
including a real eight-color/four-layer model slice and normal G-code export.
Every layer retains eight colors; replay verifies four batch visits and sixteen
spool replacements. The fixture initializes schema-backed options and explicit
per-filament profiles/palette and selects its initial tool in the start macro,
as a real printer profile does. The test target links the actual HTTP and
encryption utilities required by the slicer's logging code and instantiates the
upstream NanoSVG header implementation; no printer or GUI application is started
by this test.

The fourth case uses real bundled A1 0.2 mm presets for a 12 x 8 x 0.9 mm coupon.
It validates normal slicing/export and native project save/reload/reslice with
one pause on layer 7 while slot 2 is selected. See the
[attended test instructions](A1-01.08.01.00-TEST.md); software checks do not prove
physical spool replacement.

The release's upstream aggregate `libslic3r_tests` currently fails to compile
because older OBJ, rotation and raster tests use APIs that have changed. Those
failures are not claimed as passing regressions. The dedicated target above
compiles the same real-processor integration test independently and does not
repair or suppress those unrelated upstream tests.

## Launch the test application

After the build and integration check pass:

```powershell
.\Run-Extended-AMS-Lite.cmd
```

This launcher uses `build/extended-ams-userdata` for the test application's
configuration. On first launch, select the A1 profile in the normal setup flow.
You can import an existing project/model, configure virtual filaments, slice,
and open the dry-run swap planner from Preview. No printer connection is needed
for this planning workflow.

### Network plugin and development server setting

This local development build defaults `iot_environment` to `2` (pre-production).
Its plugin installation failed because `api-pre.bambulab.net` returned an error.
Production environment `3` uses `api.bambulab.com` for the Europe region.
Select the production environment in the development build's preferences and
restart before installing the normal network plugin. If editing the separate
`build/extended-ams-userdata/BambuStudio.conf` directly, close Studio first,
back it up, and preserve its JSON plus Windows checksum format.

On 2026-10-29 the public resource API for `slicer/plugins/cloud=02.08.02.00`
returned Windows plugin **02.08.02.60**. The official package was installed only
under the custom build's `plugins` directory. Its `bambu_networking.dll` has a
valid Authenticode signature from Shanghai Lunkuo Technology Co., Ltd.
Package SHA256: `212d6c4489711b1ff80677fd1166afd38e7201374e61894ba08abc556b118e9d`.
Startup logs confirm successful module loading and creation of the network agent,
and the application opens and responds. This verifies plugin loading, not printer
connectivity or print submission. The separate configuration requires its own
normal Bambu account login and printer selection; no login tokens were copied
from the installed Studio.

The subsequent user test produced Bambu's `unsigned_studio` restriction. A loaded
and compatible plugin does **not** mean the custom executable is authorized to
print. Removing the warning does not provide that authorization. The installed
official signed Studio (also 02.08.02.61) can open the standard one-slot probe
project, and its desktop test shortcut initially targeted that official executable.
Direct print submission from the custom build remains unverified/restricted;
any future complete workflow needs a supported delivery route as well as
physical swap validation.

The subsequent [Bambu Connect integration](CLOUD-PRINTING.md) supplies that
supported file-delivery route. The test shortcut now opens the updated custom
app, whose Preview action exports and opens the slice in Connect. The original
Print path remains unchanged. This still does not implement physical batch swaps.

This is a repository build, not an installer or a self-contained portable
package: keep the repository and its resource directory in place. Automatic
spool-change printing remains unavailable; see `ARCHITECTURE.md` for the
firmware and execution constraints.
