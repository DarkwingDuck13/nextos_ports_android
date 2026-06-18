// texconv — PRÉ-PROCESSADOR de texturas do SOR4 (offline/progressor).
// Converte cada .xnb ASTC -> ETC1 (opaca, Mali lê nativo, 8x leve) / RGBA8 (com alpha),
// aplica o TEXSCALE. Espelha EXATAMENTE a lógica do Texture2DReader.
// DOIS modos:
//   1) DIRETÓRIO (legado): texconv <gameassets_dir> [scale] [--dry]
//      -> converte os .xnb que JA estao no disco, no lugar.
//   2) APK (fusao extrai+converte, 1 passada / 1 progresso): texconv --apk <apk> <gameassets_dir> [scale]
//      -> le cada arquivo de assets/ DIRETO do APK: .xnb -> ETC1 e grava; resto -> copia.
//         UM unico 0->100% cobrindo tudo. Imprime "N/TOTAL  PCT%  caminho" por arquivo.
// Reusa Lz4DecoderStream do MonoGame + libsor4astc.so (sor4_astc_decode/sor4_etc1_encode).
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using MonoGame.Framework.Utilities;
using Microsoft.Xna.Framework.Content.Pipeline.Utilities.LZ4;

static class TexConv {
    [DllImport("sor4astc")] static extern int sor4_astc_decode(byte[] data, ulong len, int w, int h, int bx, int by, byte[] outRGBA);
    [DllImport("sor4astc")] static extern int sor4_etc1_encode(byte[] rgba, int w, int h, byte[] outEtc1);

    static readonly int[,] AstcBlk = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    const int SF_Color = 0;
    const int SF_RgbEtc1 = 60;
    const int ASTC_MIN = 96;

    static int Scale = 2;
    static bool Dry = false;
    static bool NoMaskFix = false;

    static int Main(string[] args){
        string apk = null;
        var pos = new List<string>();
        for (int i=0;i<args.Length;i++){
            if (args[i]=="--apk" && i+1<args.Length) apk = args[++i];
            else if (args[i]=="--dry") Dry=true;
            else if (args[i]=="--nomaskfix") NoMaskFix=true;
            else if (int.TryParse(args[i], out int sc)) Scale = sc;
            else pos.Add(args[i]);
        }
        if (Scale<1) Scale=1; if (Scale>4) Scale=4;

        int nthreads = Math.Max(1, Environment.ProcessorCount);
        { var e=Environment.GetEnvironmentVariable("SOR4_CONV_THREADS"); int t;
          if(!string.IsNullOrEmpty(e) && int.TryParse(e,out t) && t>=1) nthreads=t; }

        if (!SelfTestLz4()){ Console.Error.WriteLine("[texconv] AUTO-TESTE LZ4 FALHOU — abortando."); return 3; }

        if (apk != null){
            if (pos.Count < 1){ Console.Error.WriteLine("uso: texconv --apk <apk> <gameassets_dir> [scale]"); return 2; }
            return RunFromApk(apk, pos[0], nthreads);
        }
        if (pos.Count < 1){ Console.Error.WriteLine("uso: texconv <dir> [scale] [--dry]  |  texconv --apk <apk> <dir> [scale]"); return 2; }
        return RunFromDir(pos[0], nthreads);
    }

    // ---------- modo APK: extrai+converte numa passada (1 progresso) ----------
    static int RunFromApk(string apk, string gaDir, int nthreads){
        if (!File.Exists(apk)){ Console.Error.WriteLine("[texconv] APK nao existe: "+apk); return 2; }
        Directory.CreateDirectory(gaDir);
        List<string> entries;
        using (var probe = ZipFile.OpenRead(apk))
            entries = probe.Entries.Where(e => e.FullName.StartsWith("assets/") && !e.FullName.EndsWith("/"))
                                   .Select(e => e.FullName).ToList();
        int total = entries.Count;
        Console.WriteLine($"Extraindo + convertendo {total} arquivos (ETC1, escala 1/{Scale})...");
        Console.WriteLine($"[texconv] modo: {nthreads} thread(s)");
        Console.Out.Flush();

        int done=0, conv=0, skip=0, copy=0, err=0; long saved=0;
        var po = new ParallelOptions{ MaxDegreeOfParallelism = nthreads };
        Parallel.ForEach(entries, po,
            () => ZipFile.OpenRead(apk),               // 1 ZipArchive por thread (thread-safe)
            (name, st, za) => {
                string rel = name.Substring("assets/".Length);
                try {
                    var entry = za.GetEntry(name);
                    byte[] raw;
                    using (var s = entry.Open()){ using var ms = new MemoryStream(); s.CopyTo(ms); raw = ms.ToArray(); }
                    string dst = Path.Combine(gaDir, rel);
                    var dd = Path.GetDirectoryName(dst); if (!string.IsNullOrEmpty(dd)) Directory.CreateDirectory(dd);
                    if (rel.EndsWith(".xnb", StringComparison.OrdinalIgnoreCase)){
                        int r = ConvertBytes(raw, out byte[] outb);
                        if (r==1 && !Dry){ File.WriteAllBytes(dst, outb); Interlocked.Increment(ref conv); Interlocked.Add(ref saved, raw.Length - outb.Length); }
                        else if (r==1){ Interlocked.Increment(ref conv); }     // dry
                        else { File.WriteAllBytes(dst, raw); if (r==0) Interlocked.Increment(ref skip); else Interlocked.Increment(ref err); }
                    } else {
                        File.WriteAllBytes(dst, raw); Interlocked.Increment(ref copy);
                    }
                } catch (Exception e){ Interlocked.Increment(ref err); Console.Error.WriteLine($"[texconv] ERR {rel}: {e.Message}"); }
                int d = Interlocked.Increment(ref done);
                int pct = (int)((long)d*100/total);
                Console.WriteLine($"{d}/{total}  {pct}%  {rel}"); Console.Out.Flush();
                return za;
            },
            (za) => za.Dispose());
        Console.WriteLine($"Pronto! convertidas={conv} copiadas={copy} puladas={skip} erros={err}  (economia ~{saved/1024/1024} MB)");
        Console.Out.Flush();
        return err>0 && conv==0 ? 1 : 0;
    }

    // ---------- modo DIRETORIO (legado) ----------
    static int RunFromDir(string root, int nthreads){
        if (!Directory.Exists(root)){ Console.Error.WriteLine("[texconv] dir nao existe: "+root); return 2; }
        var files = new List<string>(Directory.EnumerateFiles(root, "*.xnb", SearchOption.AllDirectories));
        int total = files.Count;
        Console.WriteLine($"Pré-processando {total} texturas (ETC1, escala 1/{Scale})...");
        Console.WriteLine($"[texconv] modo: {nthreads} thread(s)");
        Console.Out.Flush();
        int done=0, conv=0, skip=0, err=0; long saved=0;
        var po = new ParallelOptions{ MaxDegreeOfParallelism = nthreads };
        Parallel.ForEach(files, po, f => {
            long before=0, after=0; int r;
            try { before = new FileInfo(f).Length; r = ConvertFile(f, out after); }
            catch (Exception e){ r = -1; Console.Error.WriteLine($"[texconv] ERR {f}: {e.Message}"); }
            if (r==1){ Interlocked.Increment(ref conv); Interlocked.Add(ref saved, before-after); }
            else if (r==0) Interlocked.Increment(ref skip); else Interlocked.Increment(ref err);
            int d = Interlocked.Increment(ref done);
            int pct = (int)((long)d*100/total);
            string rel; try { rel = Path.GetRelativePath(root, f); } catch { rel = f; }
            Console.WriteLine($"{d}/{total}  {pct}%  {rel}"); Console.Out.Flush();
        });
        Console.WriteLine($"Pronto! convertidas={conv} puladas={skip} erros={err}  (economia ~{saved/1024/1024} MB)");
        Console.Out.Flush();
        return 0;
    }

    static int Read7Bit(BinaryReader br){
        int result=0, bits=0; byte b;
        do { b=br.ReadByte(); result |= (b & 0x7F) << bits; bits+=7; } while ((b & 0x80)!=0);
        return result;
    }
    static void ReadXnbString(BinaryReader br){ int len=Read7Bit(br); br.ReadBytes(len); }
    static void ReadFull(Stream s, byte[] buf){
        int off=0; while (off<buf.Length){ int n=s.Read(buf,off,buf.Length-off); if (n<=0) throw new EndOfStreamException(); off+=n; }
    }
    static byte[] Lz4Compress(byte[] body){
        int maxLen = LZ4Codec.MaximumOutputLength(body.Length);
        var outArr = new byte[maxLen];
        int clen = LZ4Codec.Encode32HC(body, 0, body.Length, outArr, 0, maxLen);
        if (clen <= 0) return null;
        var r = new byte[clen]; Array.Copy(outArr, r, clen); return r;
    }
    static byte[] Lz4Decompress(byte[] comp, int decompSize){
        using var cin = new MemoryStream(comp);
        using var dec = new Lz4DecoderStream(cin);
        var outb = new byte[decompSize]; ReadFull(dec, outb); return outb;
    }
    static bool SelfTestLz4(){
        try {
            var rnd = new byte[200000];
            for (int i=0;i<rnd.Length;i++) rnd[i]=(byte)((i*131+7) ^ (i>>3));
            var c = Lz4Compress(rnd); if (c==null) return false;
            var d = Lz4Decompress(c, rnd.Length);
            if (d.Length!=rnd.Length) return false;
            for (int i=0;i<rnd.Length;i++) if (d[i]!=rnd[i]) return false;
            Console.WriteLine($"[texconv] auto-teste LZ4 OK (200KB -> {c.Length}B -> 200KB iguais)");
            return true;
        } catch (Exception e){ Console.Error.WriteLine("[texconv] selftest exc: "+e.Message); return false; }
    }

    // disco -> disco (modo diretorio)
    static int ConvertFile(string path, out long newLen){
        newLen = 0;
        byte[] raw = File.ReadAllBytes(path);
        int r = ConvertBytes(raw, out byte[] outb);
        if (r != 1) return r;
        if (Dry){ newLen = raw.Length; return 1; }
        File.WriteAllBytes(path, outb); newLen = outb.Length; return 1;
    }

    // NUCLEO: bytes XNB ASTC -> bytes XNB ETC1/Color. retorna 1=convertido(outXnb), 0=pular, -1=erro.
    static int ConvertBytes(byte[] raw, out byte[] outXnb){
        outXnb = null;
        if (raw.Length < 10 || raw[0]!='X' || raw[1]!='N' || raw[2]!='B') return 0;
        byte platform = raw[3], version = raw[4], flags = raw[5];
        bool lz4 = (flags & 0x40)!=0, lzx = (flags & 0x80)!=0;
        if (lzx) return 0;

        byte[] content;
        if (lz4){
            int decompSize = BitConverter.ToInt32(raw, 10);
            using var cin = new MemoryStream(raw, 14, raw.Length-14);
            using var dec = new Lz4DecoderStream(cin);
            content = new byte[decompSize];
            ReadFull(dec, content);
        } else {
            content = new byte[raw.Length-10];
            Array.Copy(raw, 10, content, 0, content.Length);
        }

        using var body = new MemoryStream(content);
        using var br = new BinaryReader(body);
        int trCount = Read7Bit(br);
        for (int i=0;i<trCount;i++){ ReadXnbString(br); br.ReadInt32(); }
        int sharedCount = Read7Bit(br);
        int typeIdx = Read7Bit(br);
        long headEnd = body.Position;
        if (typeIdx < 1 || typeIdx > trCount) return 0;

        int fmt = br.ReadInt32();
        if (fmt < ASTC_MIN) return 0;
        int w = br.ReadInt32(), h = br.ReadInt32(), levelCount = br.ReadInt32();
        if (w<=0 || h<=0 || levelCount<=0) return 0;
        int lvl0 = br.ReadInt32();
        if (lvl0<=0 || lvl0 > content.Length) return 0;
        byte[] astc = br.ReadBytes(lvl0);

        int nb = lvl0/16, bx=0, by=0;
        for (int i=0;i<AstcBlk.GetLength(0);i++){ int cbx=AstcBlk[i,0], cby=AstcBlk[i,1];
            if (((w+cbx-1)/cbx)*((h+cby-1)/cby) == nb){ bx=cbx; by=cby; break; } }
        if (bx==0) return 0;
        var full = new byte[w*h*4];
        if (sor4_astc_decode(astc, (ulong)lvl0, w, h, bx, by, full) != 0) return -1;

        if (!NoMaskFix){
            long sa=0; int colored=0, np=full.Length/4;
            for (int p=0;p<full.Length;p+=4){ sa+=full[p+3]; int mx=full[p]; if(full[p+1]>mx)mx=full[p+1]; if(full[p+2]>mx)mx=full[p+2]; if(mx>16) colored++; }
            if (np>0 && sa/np > 12 && (long)colored*100 < (long)np)
                for (int p=0;p<full.Length;p+=4){ byte a=full[p+3]; full[p]=a; full[p+1]=a; full[p+2]=a; }
        }

        int tw=Math.Max(w/Scale,1), th=Math.Max(h/Scale,1);
        byte[] rgba;
        if (Scale<=1){ rgba=full; }
        else {
            rgba = new byte[tw*th*4];
            for (int y=0;y<th;y++) for (int x=0;x<tw;x++){
                int sx=x*Scale, sy=y*Scale, rr=0,gg=0,bb=0,aa=0,cnt=0;
                for (int dy=0;dy<Scale;dy++) for (int dx=0;dx<Scale;dx++){
                    int px=sx+dx, py=sy+dy; if(px>=w||py>=h) continue;
                    int o=(py*w+px)*4; rr+=full[o]; gg+=full[o+1]; bb+=full[o+2]; aa+=full[o+3]; cnt++; }
                int d=(y*tw+x)*4; if(cnt==0)cnt=1;
                rgba[d]=(byte)(rr/cnt); rgba[d+1]=(byte)(gg/cnt); rgba[d+2]=(byte)(bb/cnt); rgba[d+3]=(byte)(aa/cnt);
            }
        }

        bool opaque=true;
        for (int p=3;p<rgba.Length;p+=4){ if (rgba[p] < 250){ opaque=false; break; } }
        int outFmt; byte[] outData;
        if (opaque){
            int bw=(tw+3)&~3, bh=(th+3)&~3; var etc1=new byte[(bw/4)*(bh/4)*8];
            if (sor4_etc1_encode(rgba, tw, th, etc1)==0){ outFmt=SF_RgbEtc1; outData=etc1; }
            else { outFmt=SF_Color; outData=rgba; }
        } else { outFmt=SF_Color; outData=rgba; }

        if (Dry){ outXnb = raw; return 1; }

        byte[] newBody;
        using (var nb2 = new MemoryStream()){
            using (var bw = new BinaryWriter(nb2)){
                bw.Write(content, 0, (int)headEnd);
                bw.Write(outFmt); bw.Write(tw); bw.Write(th); bw.Write(1);
                bw.Write(outData.Length); bw.Write(outData);
            }
            newBody = nb2.ToArray();
        }
        byte[] comp = Lz4Compress(newBody);
        bool useComp = comp != null && (14 + comp.Length) < (10 + newBody.Length);
        using (var outMs = new MemoryStream()){
            using (var bw = new BinaryWriter(outMs)){
                bw.Write((byte)'X'); bw.Write((byte)'N'); bw.Write((byte)'B');
                bw.Write(platform); bw.Write(version);
                if (useComp){
                    bw.Write((byte)0x40);
                    bw.Write((uint)(14 + comp.Length));
                    bw.Write(newBody.Length);
                    bw.Write(comp);
                } else {
                    bw.Write((byte)0);
                    bw.Write(10 + newBody.Length);
                    bw.Write(newBody);
                }
            }
            outXnb = outMs.ToArray();
        }
        return 1;
    }
}
