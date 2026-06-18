using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// skipcall <dll> <Method|Type.Method> <CalleeName>
//   Substitui por NOP a chamada (call/callvirt) a <CalleeName> DENTRO de <Method>.
//   Usado p/ remover o save_save_game REDUNDANTE no startup (program.initialize) -- esse
//   save dispara serialize/GC do .NET bem na init do libWwise so-loader -> race de sinais
//   (SIGSEGV) -> crash. Os saves de fim-de-fase (depois da init do audio) seguem funcionando.
//   So funciona se o call alvo for parametros-neutros no stack (save_save_game() = void sem args).
class SkipCall {
  static void Main(string[] a){
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    string method=a[1], callee=a[2]; int n=0;
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      if(!m.HasBody) continue;
      if(m.Name!=method && (t.Name+"."+m.Name)!=method && (t.FullName+"."+m.Name)!=method) continue;
      var il=m.Body.GetILProcessor();
      foreach(var ins in m.Body.Instructions.ToList()){
        if((ins.OpCode==OpCodes.Call||ins.OpCode==OpCodes.Callvirt) && ins.Operand is MethodReference mr && mr.Name==callee){
          il.Replace(ins, il.Create(OpCodes.Nop));
          n++; Console.WriteLine("skipcall: "+t.FullName+"."+m.Name+" -> NOP call "+mr.Name);
        }
      }
    }
    asm.Write(); Console.WriteLine("total skipcall: "+n);
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
