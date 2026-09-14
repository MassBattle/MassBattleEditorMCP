# ArmyVFX source-faithful MassBattle Batch FX translation

ArmyVFX contains 27 Niagara system entry points under `/Game/ArmyVFX/Niagara`.
The other files in the imported pack are their materials, textures, meshes, and
other dependencies. Acceptance is therefore 27/27 effect systems, not 157
independent effects.

The translation invariant is:

```text
BatchE([C0, C1, ..., Cn]) == [E(C0), E(C1), ..., E(Cn)]
```

`E` is not redesigned. Every target preserves the source emitters, renderers,
materials, curves, simulation targets, events, module defaults, and timing. The
only intentional change is the scheduler/input ABI used by MassBattleFrame:

- one-shot work consumes the Burst Niagara Data Channel;
- persistent work consumes the Attached controller arrays;
- hybrid systems use both channels without duplicating the source visual graph.

The current manifest contains eight pure Burst mappings, thirteen pure Attached
mappings, five hybrid Burst+Attached mappings, and one two-step Burst mapping.

## Translation compiler

`Scripts/Translate-ArmyVFXFaithfulBatch.py` inventories, repairs or creates,
compares, compiles, saves, and validates all 27 targets and their renderer CDOs.

```powershell
python Scripts/Translate-ArmyVFXFaithfulBatch.py --action plan
python Scripts/Translate-ArmyVFXFaithfulBatch.py --action apply --stop-on-error
```

Generated assets use these roots:

```text
/Game/ArmyVFX/WinyunqRelease/Effects/NS_MB_*_Batch
/Game/ArmyVFX/WinyunqRelease/Renderers/BP_FxRenderer_*_Batch
```

Every renderer system declares the complete Attached array ABI, even when its
recipe is Burst-only. MassBattleFrame uploads these parameters to every renderer
component, so unused declarations prevent `OverrideParameter ... was not found`
warnings without changing the recipe or source visuals.

The three source systems authored with `SpawnPerUnit` use an exact source
duplicate plus an Attached controller emitter. Their controller stores
`Particles.Previous.Position = Particles.Position` after spawn and before the
array-driven update so source particle sampling has valid motion history. The
Demo enforces the manifest's same-speed invariant: jet trails use 6000 uu/s and
rocket smoke uses 2200 uu/s.

## Demo and acceptance

Open and play:

```text
/Game/ArmyVFX/WinyunqRelease/Demo/L_ArmyVFX_FaithfulBatch_27
```

The level contains one placed `AMassBattleFxRenderer` actor for every SubType and
one `AMassBattleArmyVFXDemoActor`. It creates no source or unbatched Niagara
components; all 33 recipe steps call `UMassBattleFuncLib::SpawnBatchedFx`.

- `Space`: replay all 27 mappings
- `A/D` or Left/Right: select an entry
- `R`: replay the selected entry
- `V`: print acceptance diagnostics

The gallery automatically warms the renderers, triggers all entries after three
seconds, and repeats every twelve seconds.

Successful acceptance requires all of the following:

- translation report: 27 requested, 27 passed, 0 failed;
- level: 27 registered renderers and 27 matching Niagara target assets;
- runtime: 27 effects with render batches and Niagara components;
- path purity: `source_components=0` and `unbatched_components=0`;
- log hygiene: no Niagara or data-interface runtime diagnostic.

Reports and screenshots are written to:

```text
Saved/MassBattleEditorMCP/Validation/ArmyVFX_FaithfulBatch_Manifest.json
Saved/MassBattleEditorMCP/Validation/ArmyVFX_FaithfulBatch_Report.json
Saved/MassBattleEditorMCP/Validation/ArmyVFX_FaithfulBatch_Runtime_Report.json
Saved/MassBattleEditorMCP/Validation/ArmyVFX_FaithfulBatch_Demo_Burst.png
Saved/MassBattleEditorMCP/Validation/ArmyVFX_FaithfulBatch_Demo_Attached.png
```
