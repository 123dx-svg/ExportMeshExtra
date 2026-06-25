# Isolated Single-PAK Export (方向B)

## Problem

`UPakExportSubsystem::StartPakExport` currently runs `RunUAT BuildCookRun -iterate ... -stage -pak`, which:
1. Reuses the whole project's pre-cooked content (iterate) → pak holds the entire project.
2. Lets the project's chunk rules split content into pakchunk0/9/10/11.
3. The Completing state copies only `FoundPaks[0]` (pakchunk0).

Result: a map whose assets land in pakchunk9/10/11 (e.g. `Asset_Hyundai3`) produces a pak missing those assets, while a map in pakchunk0 (e.g. `L_S_2`) works.

## Goal

Produce ONE isolated `.pak` per selected level, containing ONLY that level + its dependencies — small, self-contained, independent of the project's chunk configuration, and non-destructive to the project's normal cooked data. No permanent INI modification.

## Chosen Approach — AssetRegistry dependency walk + direct UnrealPak

This is the standard "asset/DLC pak" pattern. It reuses cooked files (fast) and gives full control over pak contents.

### Flow (rewrite of LaunchUATProcess + TickPakExport state machine)

1. **Cook phase** — run `RunUAT BuildCookRun -project=... -cook -Map=<level> -platform=Win64 -clientconfig=Shipping -iterate -nocompileeditor -utf8output -skipstage` (cook only, NO -stage/-pak/-archive/-chunk). Keep `-iterate` for speed (cook reuse is fine; we control packaging ourselves). This ensures the level + deps exist under `Saved/Cooked/Windows/<Project>/`.
2. **Dependency walk** — via `IAssetRegistry::GetDependencies(PackageName, ..., EDependencyCategory::Package, EDependencyQuery::Hard)` recursively from the selected level package(s); collect the transitive hard-dependency package set (include the level package itself).
3. **Resolve cooked files** — for each package name, map `/Game/Path/Asset` → `Saved/Cooked/Windows/<Project>/Content/Path/Asset.{uasset|umap}` plus sibling `.uexp`, `.ubulk`, `.uptnl` if present. Also include global cooked files needed for mounting (AssetRegistry.bin is optional for SimOne mounting — decide; default: skip).
4. **Build UnrealPak response file** — one line per cooked file: `"<absolute cooked path>" "<mount path>"` where mount path is `../../../<Project>/Content/...` (UE pak mount convention).
5. **UnrealPak phase** — run `UnrealPak.exe <OutputPak> -create=<responsefile> -compress` (single process). On success, the chosen `OutputPakPath` IS the final pak (no copy needed).

### State machine changes

- `EPakExportState` gains an intermediate phase: `Cooking` → `Packing` → `Completing`.
- `LaunchUATProcess` split into `LaunchCookProcess` and `LaunchPakProcess`.
- Progress: Cook phase parses existing `(N/D)`; Pak phase parses UnrealPak `Writing entries (x/y)` or just shows indeterminate.
- Completing: no chunk search / FoundPaks[0]; the output pak is created directly at `OutputPakPath`.

## Open Decisions (confirm before/within implementation)

1. **Soft references**: default = include hard deps only. If SimOne needs soft-referenced assets at runtime, add `EDependencyQuery::Soft` walk (optional flag). DECISION NEEDED — default hard-only.
2. **Cook target platform suffix**: `Windows` vs `WindowsClient`/`WindowsNoEditor` cooked dir name — detect actual `Saved/Cooked/<X>` dir at runtime (already handled by Primary/Alternate platform dir logic).
3. **Multiple selected levels**: union their dependency sets into one pak, OR one pak per level. Current StartPakExport takes an array → default: union into the single chosen output pak.

## Risks

- Cook commandlet `-Map=` + `-skipstage` must actually limit cook to the level + deps (verify it doesn't pull bCookAll / Primary Asset Labels). If the project force-cooks labels, the cooked dir has extra content — but since WE pick files via dependency walk, extra cooked files are harmless (just not included). This is the key robustness win of the dependency-walk approach.
- UnrealPak mount path format must match what SimOne expects when mounting. Verify against an existing working SimOne pak.
- `.ubulk`/`.uexp` sibling detection must be complete or textures/meshes load broken.

## Out of Scope

- No change to GLB/thumbnail export.
- No permanent INI edits.
- No change to the project's own packaging settings.

## Verification

- Export `Asset_Hyundai3` → resulting single pak contains the Hyundai mesh/material/textures (verify by listing pak contents with `UnrealPak.exe <pak> -list`).
- Export `L_S_2` → still complete.
- Pak size is proportional to the asset (small), not the whole project.
- Mount the pak in SimOne → asset loads.
