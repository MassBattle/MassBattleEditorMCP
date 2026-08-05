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
