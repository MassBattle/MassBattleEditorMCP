using UnrealBuildTool;

public class MassBattleEditorMCP : ModuleRules
{
	public MassBattleEditorMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
			}
		);
				
		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"MassCore",
				"MassEntity",
				"MassAPI",
				"MassBattle",
				"MassBattleEditor"
			}
		);
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"EditorSubsystem",
				"Niagara",
				"NiagaraEditor",
				"AnimToTexture",
				"AnimToTextureEditor",
				"MaterialEditor",
				"StructUtils",
				"AssetTools",
				"AssetRegistry",
				"MeshUtilities",
				"MeshConversion",
				"MeshDescription",
				"StaticMeshDescription",
				"Json",
				"JsonUtilities",
				"Sockets",
				"Networking"
			}
		);
	}
}
