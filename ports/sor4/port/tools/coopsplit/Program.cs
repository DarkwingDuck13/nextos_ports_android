using System; using System.Linq; using System.Collections.Generic;
using Mono.Cecil; using Mono.Cecil.Cil;
// coopsplit <dll>
//   Habilita co-op local 2/3/4P no SOR4.
//   CommonLib.platform::update_game_pad_state_array roda 3 fases/frame:
//     Fase 1: preenche slot[0..3] com os pads fisicos distintos (get_game_pad_state(i)) -> OK.
//     Fase 2: FUNDE slot[0] |= slot[1..3] (todo input cai no Player 1).
//     Fase 3: ZERA slot[1..3] (isConnected=false) -> is_controller_connected(1/2/3)=false -> P2 nunca entra.
//   FIX: truncar o metodo logo apos a Fase 1 (inserir 'ret' onde comeca a Fase 2), descartando
//   fusao+limpeza. Resultado: slots = pads fisicos -> P2/P3/P4 conseguem dar join na selecao de
//   personagem. Seguro p/ single-player (com 1 pad, slots 1-3 ja vem isConnected=false da Fase 1).
//   Ponto de corte (robusto, sem offset hardcoded): o 'blt' que fecha o loop da Fase 1, cujo
//   proximo par de instrucoes e 'ldc.i4.1' + 'stloc V_4' (init do contador da Fase 2).
class CoopSplit {
  static int Main(string[] a){
    if(a.Length<1){ Console.Error.WriteLine("uso: coopsplit <dll>"); return 2; }
    var asm=AssemblyDefinition.ReadAssembly(a[0], new ReaderParameters{ReadWrite=true});
    int patched=0;
    foreach(var t in AllTypes(asm.MainModule)) foreach(var m in t.Methods){
      if(!m.HasBody) continue;
      if(m.Name!="update_game_pad_state_array") continue;
      var instrs=m.Body.Instructions;
      // localizar o blt da Fase 1 (seguido por ldc.i4.1 + stloc V_4)
      int cut=-1;
      for(int i=0;i+2<instrs.Count;i++){
        var ins=instrs[i];
        if(ins.OpCode!=OpCodes.Blt && ins.OpCode!=OpCodes.Blt_S) continue;
        var n1=instrs[i+1]; var n2=instrs[i+2];
        if(n1.OpCode!=OpCodes.Ldc_I4_1) continue;
        bool stV4 = (n2.OpCode==OpCodes.Stloc_S||n2.OpCode==OpCodes.Stloc)
                    && n2.Operand is VariableDefinition vd && vd.Index==4;
        if(!stV4) continue;
        if(cut!=-1){ Console.Error.WriteLine("ERRO: padrao de corte ambiguo (mais de um match)"); return 3; }
        cut=i; // mantem ate o blt (inclusive); remove tudo depois
      }
      if(cut==-1){ Console.Error.WriteLine("ERRO: ponto de corte (Fase1->Fase2) nao encontrado em "+t.FullName+"."+m.Name); return 4; }

      // seguranca: nenhuma instrucao MANTIDA pode saltar p/ a regiao REMOVIDA
      var removed=new HashSet<Instruction>();
      for(int i=cut+1;i<instrs.Count;i++) removed.Add(instrs[i]);
      for(int i=0;i<=cut;i++){
        var ins=instrs[i];
        if(ins.Operand is Instruction tgt && removed.Contains(tgt)){
          Console.Error.WriteLine("ERRO: instrucao mantida @"+i+" salta p/ regiao removida; abortando"); return 5; }
        if(ins.Operand is Instruction[] arr && arr.Any(removed.Contains)){
          Console.Error.WriteLine("ERRO: switch mantido salta p/ regiao removida; abortando"); return 5; }
      }
      // o blt mantido aponta p/ o inicio do loop da Fase 1 (mantido); seu fall-through passa a ser o ret.
      int before=instrs.Count;
      var il=m.Body.GetILProcessor();
      while(instrs.Count>cut+1) il.Remove(instrs[instrs.Count-1]);
      var ret=il.Create(OpCodes.Ret);
      il.Append(ret);
      Console.WriteLine("coopsplit: "+t.FullName+"."+m.Name+" truncado apos Fase 1 ("
        +before+" -> "+instrs.Count+" instrucoes; cut idx="+cut+", blt="+instrs[cut].OpCode+" -> "+((Instruction)instrs[cut].Operand).OpCode+")");
      patched++;
    }
    if(patched==0){ Console.Error.WriteLine("ERRO: metodo update_game_pad_state_array nao encontrado"); return 6; }
    asm.Write(); Console.WriteLine("total coopsplit: "+patched);
    return 0;
  }
  static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition m){ foreach(var t in m.Types){ yield return t; foreach(var x in Nested(t)) yield return x; } }
  static IEnumerable<TypeDefinition> Nested(TypeDefinition t){ foreach(var x in t.NestedTypes){ yield return x; foreach(var y in Nested(x)) yield return y; } }
}
