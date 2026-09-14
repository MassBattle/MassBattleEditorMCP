# Projectile MCP Capability Contract

## Required runtime check

The repository defines a dedicated CRUD/schema/validation interface for `UMassBattleProjectileConfigDataAsset`. Always call `projectile_get_api_status` against the running editor because an old or not-yet-restarted plugin instance may still lack it.

When the API is present, use:

```text
projectile_get_api_status
projectile_list / projectile_query
projectile_get
projectile_get_schema
projectile_create
projectile_write          # source-aligned union write
projectile_validate
projectile_delete         # dry-run by default
```

The implementation must:

- accept `UMassBattleProjectileConfigDataAsset` paths;
- return a simple and full view;
- expose enum names and conditional field roles;
- preserve omitted fields;
- append/reject arrays explicitly;
- validate movement/damage struct consistency;
- validate trigger contradictions and duplicate lifecycle FX;
- read back after save;
- never mutate `.uasset` bytes directly.

`projectile_write` performs a transient preflight before mutating the target. Omitted fields remain unchanged. Array append and whole-array replacement are rejected unless explicitly requested.

## Minimum validation rules

- selected movement mode has its matching configuration;
- selected damage mode has its matching damage/debuff pair;
- DataAsset references are loadable;
- conventional projectile with damage has a valid apply condition;
- conventional one-hit projectile uses a compatible repetition mode;
- projectile-owned attack warns when Agent `TimeOfHitAction` also applies damage;
- entity-hit damage without explicit entity-hit removal is surfaced together with the Health/no-health fallback, so penetration intent is reviewed;
- OnHit and OnRemoval duplicate explosion configs are surfaced;
- lifecycle FX with ordinary Niagara/Cascade assets are flagged as unbatched/hybrid;
- recursion/submunition depth and quantities are reviewable.

## Honest fallback for an old running plugin

When `projectile_get_api_status` is absent, an AI may inspect unit attack arrays, wire an already-existing projectile reference through Unit MCP, inspect linked effects, and provide a configuration design. Report one of:

```text
projectile_design_only
projectile_asset_wired_existing
blocked_by_mcp_capability
```

Do not translate a written configuration table into a claim that the Unreal asset was changed.
