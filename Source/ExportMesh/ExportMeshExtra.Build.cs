// Some copyright should be here...

using UnrealBuildTool;

public class ExportMeshExtra : ModuleRules
{
	public ExportMeshExtra(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"MeshMergeUtilities",
				"AssetRegistry",
				"UnrealEd",
				"DesktopPlatform",
				"MeshDescription",
				"StaticMeshDescription",
				"EditorScriptingUtilities",
				"Blutility",
				"UMGEditor",
				"ToolMenus",
				"ContentBrowser",
				"GLTFExporter",
				"GeometryCore",
				"GeometryFramework",
				"GeometryScriptingCore",
				"GeometryScriptingEditor",
				"Json",
				"JsonUtilities",
				"RenderCore",  // For FlushRenderingCommands
				"RHI", "CinematicCamera" // For rendering thread synchronization
				// ... add private dependencies that you statically link with here ...
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
