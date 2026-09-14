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
				"DeveloperSettings",
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
				"Blutility",
				"UMG",
				"UMGEditor",
				"ToolMenus",
				"PropertyEditor",
				"Settings",
				"Niagara",
				"NiagaraEditor",
				"AnimToTexture",
				"AnimToTextureEditor",
				"AnimationBlueprintLibrary",
				"MaterialEditor",
				"StructUtils",
				"AssetTools",
				"AssetRegistry",
				"MeshUtilities",
				"SkeletalMeshUtilitiesCommon",
				"MeshConversion",
				"MeshDescription",
				"SkeletalMeshDescription",
				"StaticMeshDescription",
				"Json",
				"JsonUtilities",
				"Sockets",
				"Networking"
			}
		);
	}
}
