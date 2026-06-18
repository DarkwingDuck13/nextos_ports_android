using System;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

static class PatchRe4Managed
{
    static bool HasString(MethodBody body, string text)
    {
        return body.Instructions.Any(i => i.OpCode == OpCodes.Ldstr && Equals(i.Operand, text));
    }

    static MethodDefinition FindMethod(TypeDefinition type, string name)
    {
        var method = type.Methods.FirstOrDefault(m => m.Name == name);
        if (method == null)
            throw new InvalidOperationException("Method not found: " + type.FullName + "::" + name);
        return method;
    }

    static MethodReference FindMethodReference(ModuleDefinition module, string name, Func<MethodReference, bool> predicate)
    {
        var method = module.Types
            .SelectMany(t => t.Methods)
            .Where(m => m.HasBody)
            .SelectMany(m => m.Body.Instructions)
            .Select(i => i.Operand as MethodReference)
            .FirstOrDefault(mr => mr != null && mr.Name == name && predicate(mr));
        if (method == null)
            throw new InvalidOperationException("Could not locate method reference: " + name);
        return module.Import(method);
    }

    static GenericInstanceMethod BuildFindObjectOfType(ModuleDefinition module, TypeReference targetType)
    {
        var template = module.Types
            .SelectMany(t => t.Methods)
            .Where(m => m.HasBody)
            .SelectMany(m => m.Body.Instructions)
            .Select(i => i.Operand as GenericInstanceMethod)
            .FirstOrDefault(gim => gim != null && gim.ElementMethod.Name == "FindObjectOfType");
        if (template == null)
            throw new InvalidOperationException("Could not locate FindObjectOfType<T> template in assembly");

        var imported = module.Import(template.ElementMethod);
        var method = new GenericInstanceMethod(imported);
        method.GenericArguments.Add(targetType);
        return method;
    }

    static MethodReference FindDebugLog(ModuleDefinition module)
    {
        return FindMethodReference(module, "Log", mr => mr.Parameters.Count == 1);
    }

    static MethodReference FindInvoke(ModuleDefinition module)
    {
        return FindMethodReference(module, "Invoke",
            mr => mr.Parameters.Count == 2 && mr.Parameters[0].ParameterType.MetadataType == MetadataType.String);
    }

    static MethodReference FindSetTimeScale(ModuleDefinition module)
    {
        return FindMethodReference(module, "set_timeScale", mr => mr.Parameters.Count == 1);
    }

    static void InsertDebugLog(ILProcessor il, Instruction target, MethodReference debugLog, string message)
    {
        il.InsertBefore(target, Instruction.Create(OpCodes.Ldstr, message));
        il.InsertBefore(target, Instruction.Create(OpCodes.Call, debugLog));
    }

    static void InsertInvoke(ILProcessor il, Instruction target, MethodReference invoke, string methodName, float delay)
    {
        il.InsertBefore(target, Instruction.Create(OpCodes.Ldarg_0));
        il.InsertBefore(target, Instruction.Create(OpCodes.Ldstr, methodName));
        il.InsertBefore(target, Instruction.Create(OpCodes.Ldc_R4, delay));
        il.InsertBefore(target, Instruction.Create(OpCodes.Call, invoke));
    }

    static void ReplaceStringLiteral(MethodDefinition method, string oldValue, string newValue)
    {
        var replaced = false;
        foreach (var ins in method.Body.Instructions)
        {
            if (ins.OpCode == OpCodes.Ldstr && Equals(ins.Operand, oldValue))
            {
                ins.Operand = newValue;
                replaced = true;
            }
        }

        if (!replaced)
            throw new InvalidOperationException("String not found in " + method.DeclaringType.FullName + "::" + method.Name + ": " + oldValue);
    }

    static void PatchVMainMenuStart(TypeDefinition type)
    {
        var start = FindMethod(type, "Start");
        var showMainMenuPage = FindMethod(type, "ShowMainMenuPage");
        var body = start.Body;
        var il = body.GetILProcessor();
        var debugLog = FindDebugLog(type.Module);
        var invoke = FindInvoke(type.Module);
        var animatorField = type.Fields.FirstOrDefault(f => f.Name == "animator");
        if (animatorField == null)
            throw new InvalidOperationException("Field not found: vMainMenu::animator");

        if (HasString(body, "CODEX vMainMenu.Start"))
        {
            Console.WriteLine("vMainMenu.Start already patched");
            return;
        }

        var stloc0 = body.Instructions.FirstOrDefault(i => i.OpCode == OpCodes.Stloc_0);
        if (stloc0 == null || stloc0.Next == null)
            throw new InvalidOperationException("Unexpected vMainMenu.Start body");

        var next = stloc0.Next;
        var cameraType = body.Variables[0].VariableType;
        var findCamera = BuildFindObjectOfType(type.Module, cameraType);
        var findAnimator = BuildFindObjectOfType(type.Module, animatorField.FieldType);

        il.InsertBefore(next, Instruction.Create(OpCodes.Ldloc_0));
        il.InsertBefore(next, Instruction.Create(OpCodes.Brtrue, next));
        il.InsertBefore(next, Instruction.Create(OpCodes.Call, findCamera));
        il.InsertBefore(next, Instruction.Create(OpCodes.Stloc_0));

        var ret = body.Instructions.LastOrDefault(i => i.OpCode == OpCodes.Ret);
        if (ret == null)
            throw new InvalidOperationException("No ret in vMainMenu.Start");
        var skipAnimatorFallback = Instruction.Create(OpCodes.Nop);
        il.InsertBefore(ret, Instruction.Create(OpCodes.Ldarg_0));
        il.InsertBefore(ret, Instruction.Create(OpCodes.Ldfld, animatorField));
        il.InsertBefore(ret, Instruction.Create(OpCodes.Brtrue, skipAnimatorFallback));
        il.InsertBefore(ret, Instruction.Create(OpCodes.Ldarg_0));
        il.InsertBefore(ret, Instruction.Create(OpCodes.Call, findAnimator));
        il.InsertBefore(ret, Instruction.Create(OpCodes.Stfld, animatorField));
        il.InsertBefore(ret, skipAnimatorFallback);
        InsertDebugLog(il, ret, debugLog, "CODEX vMainMenu.Start");
        il.InsertBefore(ret, Instruction.Create(OpCodes.Ldarg_0));
        il.InsertBefore(ret, Instruction.Create(OpCodes.Call, showMainMenuPage));
        InsertInvoke(il, ret, invoke, "ShowMainMenuPage", 0.25f);

        Console.WriteLine("Patched vMainMenu.Start");
    }

    static void PatchMainMenuControllerStart(TypeDefinition type)
    {
        var start = FindMethod(type, "Start");
        var body = start.Body;
        var il = body.GetILProcessor();
        var debugLog = FindDebugLog(type.Module);
        var invoke = FindInvoke(type.Module);
        var setTimeScale = FindSetTimeScale(type.Module);

        if (HasString(body, "CODEX MainMenuController.Start"))
        {
            Console.WriteLine("MainMenuController.Start already patched");
            return;
        }

        var ret = body.Instructions.LastOrDefault(i => i.OpCode == OpCodes.Ret);
        if (ret == null)
            throw new InvalidOperationException("No ret in MainMenuController.Start");
        il.InsertBefore(ret, Instruction.Create(OpCodes.Ldc_R4, 1.0f));
        il.InsertBefore(ret, Instruction.Create(OpCodes.Call, setTimeScale));
        InsertDebugLog(il, ret, debugLog, "CODEX MainMenuController.Start");
        InsertInvoke(il, ret, invoke, "openOptions", 0.25f);

        Console.WriteLine("Patched MainMenuController.Start");
    }

    static void PatchShowMainMenuPage(TypeDefinition type)
    {
        var method = FindMethod(type, "ShowMainMenuPage");
        var body = method.Body;
        if (HasString(body, "CODEX ShowMainMenuPage"))
        {
            Console.WriteLine("vMainMenu.ShowMainMenuPage already patched");
            return;
        }

        var il = body.GetILProcessor();
        var debugLog = FindDebugLog(type.Module);
        var first = body.Instructions.First();
        InsertDebugLog(il, first, debugLog, "CODEX ShowMainMenuPage");
        Console.WriteLine("Patched vMainMenu.ShowMainMenuPage");
    }

    static void PatchOpenOptions(TypeDefinition type)
    {
        var method = FindMethod(type, "openOptions");
        var body = method.Body;
        if (HasString(body, "CODEX openOptions"))
        {
            Console.WriteLine("MainMenuController.openOptions already patched");
            return;
        }

        var il = body.GetILProcessor();
        var debugLog = FindDebugLog(type.Module);
        var setTimeScale = FindSetTimeScale(type.Module);
        var first = body.Instructions.First();
        il.InsertBefore(first, Instruction.Create(OpCodes.Ldc_R4, 1.0f));
        il.InsertBefore(first, Instruction.Create(OpCodes.Call, setTimeScale));
        InsertDebugLog(il, first, debugLog, "CODEX openOptions");
        Console.WriteLine("Patched MainMenuController.openOptions");
    }

    static void PatchMainMenuKeyboardControllerStart(TypeDefinition type)
    {
        var start = FindMethod(type, "Start");
        var body = start.Body;
        if (HasString(body, "CODEX MainMenu_KeyboardController.Start"))
        {
            Console.WriteLine("MainMenu_KeyboardController.Start already patched");
            return;
        }

        var il = body.GetILProcessor();
        var debugLog = FindDebugLog(type.Module);
        var first = body.Instructions.First();
        InsertDebugLog(il, first, debugLog, "CODEX MainMenu_KeyboardController.Start");
        Console.WriteLine("Patched MainMenu_KeyboardController.Start");
    }

    static void PatchSceneChangeStrings(TypeDefinition type)
    {
        var method = FindMethod(type, "onSceneChange");
        if (HasString(method.Body, "CODEX load scene"))
        {
            Console.WriteLine("EasyAudioUtility_SceneManager.onSceneChange already tagged");
            return;
        }

        ReplaceStringLiteral(method, " is opend", " CODEX is opend");
        ReplaceStringLiteral(method, " is found", " CODEX is found");
        ReplaceStringLiteral(method, " audio replaced", " CODEX audio replaced");
        Console.WriteLine("Patched EasyAudioUtility_SceneManager.onSceneChange strings");
    }

    static void PatchLoadingScreenStrings(TypeDefinition type)
    {
        var method = FindMethod(type, "fillLoadingBar");
        if (HasString(method.Body, "CODEX load scene"))
        {
            Console.WriteLine("LoadingScreen.fillLoadingBar already tagged");
            return;
        }

        ReplaceStringLiteral(method, "load scene", "CODEX load scene");
        Console.WriteLine("Patched LoadingScreen.fillLoadingBar string");
    }

    public static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: patch_re4_managed <input-dll> <output-dll>");
            return 2;
        }

        var input = args[0];
        var output = args[1];
        var asm = AssemblyDefinition.ReadAssembly(input);
        var module = asm.MainModule;

        var vMainMenu = module.Types.FirstOrDefault(t => t.FullName == "EviLA.AddOns.RPGPack.MenuSystem.vMainMenu");
        if (vMainMenu == null)
            throw new InvalidOperationException("Type not found: vMainMenu");
        PatchVMainMenuStart(vMainMenu);
        PatchShowMainMenuPage(vMainMenu);

        var mainMenuController = module.Types.FirstOrDefault(t => t.FullName == "MainMenuController");
        if (mainMenuController == null)
            throw new InvalidOperationException("Type not found: MainMenuController");
        PatchMainMenuControllerStart(mainMenuController);
        PatchOpenOptions(mainMenuController);

        var keyboardController = module.Types.FirstOrDefault(t => t.FullName == "MainMenu_KeyboardController");
        if (keyboardController == null)
            throw new InvalidOperationException("Type not found: MainMenu_KeyboardController");
        PatchMainMenuKeyboardControllerStart(keyboardController);

        var loadingScreen = module.Types.FirstOrDefault(t => t.FullName == "LoadingScreen");
        if (loadingScreen == null)
            throw new InvalidOperationException("Type not found: LoadingScreen");
        PatchLoadingScreenStrings(loadingScreen);

        var sceneManager = module.Types.FirstOrDefault(t => t.FullName == "EasyAudioUtility_SceneManager");
        if (sceneManager == null)
            throw new InvalidOperationException("Type not found: EasyAudioUtility_SceneManager");
        PatchSceneChangeStrings(sceneManager);

        asm.Write(output);
        Console.WriteLine("Wrote " + output);
        return 0;
    }
}
